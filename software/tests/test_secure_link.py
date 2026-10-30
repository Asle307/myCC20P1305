"""Independent Python peer, real C CLI, fragmented socket streams and safe files.

Requires a C compiler, pkg-config, OpenSSL/BlueZ development files and cryptography.
No radio, PSK fixture, FPGA, RTL simulation or board access is used here.
"""
import hashlib
import hmac
import os
from pathlib import Path
import shlex
import socket
import struct
import subprocess
import tempfile
import time
import unittest

from cryptography.hazmat.primitives.ciphers.aead import ChaCha20Poly1305
from cryptography.hazmat.primitives.hashes import SHA256
from cryptography.hazmat.primitives.kdf.hkdf import HKDF

ROOT = Path(__file__).resolve().parents[2]
LIMIT = 10 * 1024 * 1024
CHAT, OFFER, READY, REFUSE, CHUNK, ACK, END, COMPLETE, RECEIPT, CLOSE, PING, PONG = range(1, 13)


def header(kind, role, sequence, length):
    return struct.pack("!4sBBBBQI4s", b"SLF1", 1, kind, role, 0, sequence, length, b"\0" * 4)


def hs(kind, role):
    return b"SLH1" + bytes((kind, role, 0, 0))


def mac(key, label, transcript):
    return hmac.digest(key, b"secure-link-v1 " + label + transcript, "sha256")


class Endpoint:
    def __init__(self, case, executable, directory, role=0, key=None, timeout=1000, socket_buffer=None,
                 verbose=False):
        self.case = case
        self.root = Path(directory)
        self.root.mkdir()
        self.receive = self.root / "received"
        self.receive.mkdir(mode=0o700)
        self.psk = os.urandom(32) if key is None else key
        self.key_file = self.root / "secret.key"
        self.key_file.write_bytes(self.psk)
        self.key_file.chmod(0o600)
        self.role = role
        self.sock, child = socket.socketpair()
        if socket_buffer is not None:
            for sock in (self.sock, child):
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_SNDBUF, socket_buffer)
                sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, socket_buffer)
        self.sock.settimeout(5)
        self.logfile = tempfile.TemporaryFile()
        self.process = subprocess.Popen(
            [str(executable), "--role", "laptop" if role else "kv260", "--test-fd", str(child.fileno()),
             "--psk", str(self.key_file), "--receive", str(self.receive), "--timeout-ms", str(timeout)] +
            (["--verbose"] if verbose else []),
            pass_fds=(child.fileno(),), stdin=subprocess.PIPE, stdout=self.logfile, stderr=self.logfile)
        child.close()
        self.tx = self.rx = 0
        case.addCleanup(self.cleanup)

    def log(self):
        return os.pread(self.logfile.fileno(), 4 * 1024 * 1024, 0).decode("utf-8", errors="replace")

    def wait(self, text, timeout=5, count=1):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if self.log().count(text) >= count:
                return
            if self.process.poll() is not None:
                break
            time.sleep(0.005)
        self.case.fail(f"Missing {text!r} (count={count}), exit={self.process.poll()}:\n{self.log()}")

    def command(self, line):
        self.process.stdin.write(line.encode("utf-8") + b"\n")
        self.process.stdin.flush()

    def exact(self, size):
        result = bytearray()
        while len(result) < size:
            block = self.sock.recv(size - len(result))
            if not block:
                raise EOFError(self.log())
            result.extend(block)
        return bytes(result)

    def write(self, data, fragmented=False):
        if fragmented:
            # Odd segment sizes cover both split fields and coalesced boundaries.
            for offset in range(0, len(data), 7):
                self.sock.sendall(data[offset:offset + 7])
        else:
            self.sock.sendall(data)

    def handshake(self, corrupt=None, fragmented=False):
        if self.role == 0:
            client = hs(1, 1) + os.urandom(32)
            self.write(client, fragmented)
            server = self.exact(72)
            self.case.assertEqual(server[:8], hs(2, 0))
            transcript = client + server[:40]
            self.case.assertEqual(server[40:], mac(self.psk, b"server", transcript))
            self.case.assertNotIn("Session established:", self.log())
            auth = hs(3, 1) + mac(self.psk, b"client", transcript)
            if corrupt is True:
                auth = auth[:-1] + bytes((auth[-1] ^ 1,))
            self.write(auth, fragmented)
            if corrupt is True:
                return
        else:
            client = self.exact(40)
            self.case.assertEqual(client[:8], hs(1, 1))
            server = hs(2, 0) + os.urandom(32)
            transcript = client + server
            response = server + mac(self.psk, b"server", transcript)
            if corrupt is True:
                # Alter a challenge but leave its old MAC: transcript binding.
                response = response[:9] + bytes((response[9] ^ 1,)) + response[10:]
            self.write(response, fragmented)
            if corrupt is True:
                return
            auth = self.exact(40)
            self.case.assertEqual(auth, hs(3, 1) + mac(self.psk, b"client", transcript))
            self.case.assertNotIn("Session established:", self.log())
        self.keys = [HKDF(algorithm=SHA256(), length=32, salt=hashlib.sha256(transcript).digest(),
                          info=b"secure-link-v1 " + direction).derive(self.psk)
                     for direction in (b"kv260 to laptop", b"laptop to kv260")]
        if self.role == 0:
            self.case.assertEqual(self.exact(40), hs(4, 0) + mac(self.keys[0], b"finished", transcript))
        else:
            finished = hs(4, 0) + mac(self.keys[0], b"finished", transcript)
            if corrupt == "finished":
                self.write(finished[:-1] + bytes((finished[-1] ^ 1,)))
                return
            self.write(finished, fragmented)
        self.wait("Session established:")

    def make(self, kind, data=b"", sequence=None):
        sequence = self.tx if sequence is None else sequence
        aad = header(kind, 1 - self.role, sequence, len(data))
        nonce = b"\0" * 4 + sequence.to_bytes(8, "big")
        return aad + ChaCha20Poly1305(self.keys[1 - self.role]).encrypt(nonce, data, aad)

    def send(self, kind, data=b"", fragmented=False):
        wire = self.make(kind, data)
        self.write(wire, fragmented)
        self.tx += 1
        return wire

    def frame(self, expected=None):
        while True:
            aad = self.exact(24)
            magic, version, kind, direction, reserved, sequence, length, rest = struct.unpack("!4sBBBBQI4s", aad)
            self.case.assertEqual((magic, version, direction, reserved, rest),
                                  (b"SLF1", 1, self.role, 0, b"\0" * 4))
            self.case.assertLessEqual(length, 4096)
            self.case.assertEqual(sequence, self.rx)
            cipher = self.exact(length + 16)
            data = ChaCha20Poly1305(self.keys[self.role]).decrypt(b"\0" * 4 + sequence.to_bytes(8, "big"), cipher, aad)
            self.rx += 1
            if kind == PING:
                self.send(PONG)
                continue
            if kind == PONG:
                continue
            if expected is not None:
                self.case.assertEqual(kind, expected, self.log())
            return kind, data, sequence

    def exit(self, expected=0):
        self.send(CLOSE)
        self.case.assertEqual(self.process.wait(5), expected, self.log())
        self.case.assertNotIn("runtime error", self.log())

    def cleanup(self):
        self.sock.close()
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait(3)
        self.process.stdin.close()
        self.logfile.close()


class SecureLinkTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory()
        cls.build = Path(cls.temporary.name)
        cls.flags = ["cc", "-std=c99", "-O2", "-g", "-Wall", "-Wextra", "-Werror",
                     "-fsanitize=address,undefined", "-fno-sanitize-recover=all"]
        cls.libs = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "openssl", "bluez"], text=True))
        cls.sources = [str(ROOT / "software/secure_link" / name) for name in
                       ("protocol.c", "transfer.c", "transport.c")] + [str(ROOT / "software/aead_uio_api.c")]
        main = str(ROOT / "software/secure_link/main.c")
        cls.cli = cls.build / "secure_link"
        cls.test_cli = cls.build / "secure_link_test"
        for output, extra in ((cls.cli, []), (cls.test_cli, ["-DSL_TESTING"])):
            subprocess.run(cls.flags + extra + [main] + cls.sources + cls.libs + ["-o", str(output)], check=True)
        cls.short_write_cli = cls.build / "secure_link_short_write"
        subprocess.run(cls.flags + ["-DSL_TESTING", main] + cls.sources +
                       [str(ROOT / "software/tests/test_short_write.c"), "-Wl,--wrap=write"] +
                       cls.libs + ["-o", str(cls.short_write_cli)], check=True)
        serial_cli = cls.build / "secure_link_serial"
        crypto_libs = shlex.split(subprocess.check_output(["pkg-config", "--cflags", "--libs", "openssl"], text=True))
        subprocess.run(cls.flags + ["-DSL_SERIAL_ONLY", main] + cls.sources + crypto_libs +
                       ["-o", str(serial_cli)], check=True)
        msg = "☃".encode()
        aad = header(CHAT, 1, 0, len(msg))
        vector = aad + ChaCha20Poly1305(bytes(range(32))).encrypt(bytes(12), msg, aad)
        (cls.build / "protocol_vector.h").write_text("static const uint8_t protocol_vector[] = {" +
                                                     ",".join(map(str, vector)) + "};\n")
        cls.unit = cls.build / "test_secure_link"
        subprocess.run(cls.flags + ["-I" + str(cls.build), str(ROOT / "software/tests/test_secure_link.c")] +
                       cls.sources + cls.libs + ["-o", str(cls.unit)], check=True)

    @classmethod
    def tearDownClass(cls):
        cls.temporary.cleanup()

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name)
        self.serial = 0

    def endpoint(self, **kwargs):
        self.serial += 1
        executable = kwargs.pop("executable", self.test_cli)
        return Endpoint(self, executable, self.root / f"endpoint{self.serial}", **kwargs)

    def test_wire_vector_private_output_nonce_bounds_and_serial(self):
        result = subprocess.run([str(self.unit)], text=True, capture_output=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("PASS secure link", result.stdout)
        result = subprocess.run([str(self.cli), "--test-fd", "3"], capture_output=True)
        self.assertEqual(result.returncode, 2)  # Test backdoor is absent from production.

    def test_psk_generation_and_permissions(self):
        first, second = self.root / "first.key", self.root / "second.key"
        def run(*args):
            return subprocess.run([str(self.cli), *map(str, args)], capture_output=True)
        self.assertEqual(run("--keygen", first).returncode, 0)
        self.assertEqual(run("--keygen", second).returncode, 0)
        self.assertEqual(len(first.read_bytes()), 32)
        self.assertEqual(first.stat().st_mode & 0o777, 0o600)
        self.assertNotEqual(first.read_bytes(), second.read_bytes())
        self.assertEqual(run("--keygen", first).returncode, 1)
        self.assertEqual(run("--check-key", first).returncode, 0)
        self.assertEqual(run("--check-key=" + str(first)).returncode, 0)
        restricted = self.root / "restricted.psk"
        result = subprocess.run([str(self.cli), "--keygen", str(restricted)], capture_output=True,
                                preexec_fn=lambda: os.umask(0o777))
        self.assertEqual(result.returncode, 1)
        self.assertFalse(restricted.exists())
        result = subprocess.run([str(self.cli), "--keygen", str(restricted)], capture_output=True,
                                preexec_fn=lambda: os.umask(0o200))
        self.assertEqual(result.returncode, 0)
        self.assertEqual(restricted.stat().st_mode & 0o777, 0o400)
        self.assertEqual(run("--check-key", restricted).returncode, 0)
        first.chmod(0o640)
        self.assertEqual(run("--check-key", first).returncode, 1)
        first.chmod(0o600)
        for size in (0, 31, 33):
            first.write_bytes(os.urandom(size))
            self.assertEqual(run("--check-key", first).returncode, 1)
        alias = self.root / "alias"
        alias.symlink_to(second)
        self.assertEqual(run("--check-key", alias).returncode, 1)
        self.assertEqual(run("--keygen", alias).returncode, 1)

    def test_handshake_both_roles_fresh_keys_and_fragmentation(self):
        psk, keys = os.urandom(32), []
        for role in (0, 1, 0):
            e = self.endpoint(role=role, key=psk)
            e.handshake(fragmented=True)
            self.assertIn("SECURE LINK / SPP", e.log())
            self.assertIn("[OK] Session established:", e.log())
            self.assertNotIn(e.psk.hex(), e.log())
            self.assertNotEqual(e.keys[0], e.keys[1])
            keys.append(tuple(e.keys))
            e.send(CHAT, "Xin chào bradah ☃".encode(), fragmented=True)
            self.assertEqual(e.frame(RECEIPT)[1], bytes(8))
            e.command("/msg hello laptop")
            kind, data, seq = e.frame(CHAT)
            self.assertEqual(data, b"hello laptop")
            e.send(RECEIPT, seq.to_bytes(8, "big"))
            e.wait("Chat delivered")
            e.exit()
        self.assertEqual(len(set(keys)), 3)

    def test_handshake_modified_transcript_wrong_psk_and_timeout(self):
        for role in (0, 1):
            e = self.endpoint(role=role, timeout=200)
            e.handshake(corrupt=True)
            self.assertEqual(e.process.wait(3), 1, e.log())
            self.assertNotIn("Session established:", e.log())
        for role, stage in ((1, "finished"),):
            e = self.endpoint(role=role, timeout=200)
            e.handshake(corrupt=stage)
            self.assertEqual(e.process.wait(3), 1, e.log())
            self.assertNotIn("Session established:", e.log())
        e = self.endpoint(timeout=200)
        e.psk = os.urandom(32)  # Independent peer has the wrong PSK.
        client = hs(1, 1) + os.urandom(32)
        e.write(client)
        server = e.exact(72)
        e.write(hs(3, 1) + mac(e.psk, b"client", client + server[:40]))
        self.assertEqual(e.process.wait(3), 1, e.log())
        self.assertNotIn("Session established:", e.log())
        stalled = self.endpoint(timeout=200)
        stalled.write(b"SLH")
        self.assertEqual(stalled.process.wait(3), 1, stalled.log())
        early = self.endpoint(timeout=200)
        early.write(header(CHAT, 1, 0, 1) + b"x" + bytes(16))
        self.assertEqual(early.process.wait(3), 1)
        self.assertNotIn("Peer:", early.log())

    def test_cipher_tag_aad_sequence_and_replay_rejections(self):
        e = self.endpoint()
        e.handshake()
        wire = e.send(CHAT, b"once-only")
        e.frame(RECEIPT)
        bad = []
        for offset in (24, len(wire) - 1, 15):
            changed = bytearray(wire)
            changed[offset] ^= 1
            bad.append(bytes(changed))
        changed = bytearray(wire)
        changed[5] = OFFER  # Well-formed AAD, invalid authentication.
        bad.append(bytes(changed))
        bad.extend((wire, e.make(CHAT, b"future", sequence=e.tx + 1)))
        for i, frame in enumerate(bad, 1):
            e.write(frame, fragmented=True)
            e.wait("REJECTED:", count=i)
        self.assertEqual(e.log().count("[AUTH] REJECTED:"), 4)
        self.assertIn("[REPLAY] REJECTED:", e.log())
        self.assertIn("[ORDER] REJECTED:", e.log())
        self.assertEqual(e.log().count("Peer: once-only"), 1)
        self.assertNotIn("Peer: future", e.log())
        e.send(CHAT, b"still-synchronized")
        e.frame(RECEIPT)
        e.wait("Peer: still-synchronized")
        e.exit()

    def test_verbose_trace_has_status_labels_and_no_keys(self):
        e = self.endpoint(verbose=True)
        e.handshake()
        e.send(CHAT, b"trace demo")
        e.frame(RECEIPT)
        e.wait("[TRACE] tag=")
        log = e.log()
        for label in ("[TRACE] RX result=", "[TRACE] nonce=", "[TRACE] AAD=",
                      "[TRACE] ciphertext=", "[TRACE] tag="):
            self.assertIn(label, log)
        for key in (e.psk, *e.keys):
            self.assertNotIn(key.hex(), log)
        e.exit()

    def test_demo_commands_wait_for_authenticated_receipt(self):
        e = self.endpoint()
        e.handshake()
        for command in ("/tamper harmless", "/replay harmless"):
            e.command(command)
            kind, data, seq = e.frame(CHAT)
            e.send(RECEIPT, seq.to_bytes(8, "big"))
            aad = e.exact(24)
            length = int.from_bytes(aad[16:20], "big")
            cipher = e.exact(length + 16)
            self.assertEqual(int.from_bytes(aad[8:16], "big"), seq)
            if command.startswith("/tamper"):
                with self.assertRaises(Exception):
                    ChaCha20Poly1305(e.keys[e.role]).decrypt(bytes(4) + seq.to_bytes(8, "big"), cipher, aad)
            else:
                self.assertEqual(ChaCha20Poly1305(e.keys[e.role]).decrypt(
                    bytes(4) + seq.to_bytes(8, "big"), cipher, aad), data)
        e.command("/msg next nonce")
        _, _, seq = e.frame(CHAT)
        self.assertEqual(seq, 2)
        e.send(RECEIPT, seq.to_bytes(8, "big"))
        e.wait("Chat delivered", count=3)
        e.exit()

    def test_partial_stdin_utf8_controls_and_oversized_lines(self):
        e = self.endpoint()
        e.handshake()
        e.process.stdin.write(b"/msg par")
        e.process.stdin.flush()
        e.send(CHAT, b"while stdin is incomplete")
        e.frame(RECEIPT)
        e.process.stdin.write(b"tial\n")
        e.process.stdin.flush()
        _, data, seq = e.frame(CHAT)
        self.assertEqual(data, b"partial")
        e.send(RECEIPT, seq.to_bytes(8, "big"))
        e.wait("Chat delivered")
        e.process.stdin.write(b"\xff\n/msg nul\0x\n" + b"x" * 5000 + b"\n\r\n\n")
        e.process.stdin.flush()
        e.wait("Input line discarded", count=2)
        e.wait("Chat requires")
        e.command("/msg after discarded input")
        _, data, seq = e.frame(CHAT)
        self.assertEqual(data, b"after discarded input")
        e.send(RECEIPT, seq.to_bytes(8, "big"))
        e.wait("Chat delivered", count=2)
        e.exit()

    def test_malformed_oversized_headers_and_partial_timeout(self):
        for index, value in ((0, ord("X")), (4, 2), (5, 255), (6, 0), (7, 1), (20, 1), (16, 1)):
            with self.subTest(index=index):
                e = self.endpoint(timeout=200)
                e.handshake()
                bad = bytearray(header(CHAT, 1, 0, 1))
                bad[index] = value
                e.write(bad)
                # Wrong direction is checked after a complete bounded frame.
                if index == 6:
                    e.write(bytes(17))
                self.assertEqual(e.process.wait(3), 1, e.log())
                self.assertNotIn("Peer:", e.log())
        e = self.endpoint(timeout=200)
        e.handshake()
        e.write(header(CHAT, 1, 0, 100) + b"partial")
        self.assertEqual(e.process.wait(3), 1, e.log())
        self.assertIn("timeout", e.log())

    def upload(self, e, name, data):
        e.send(OFFER, len(data).to_bytes(8, "big") + name.encode())
        e.frame(READY)
        self.assertFalse((e.receive / name).exists())
        temporary = list(e.receive.glob(".secure-link-*"))
        self.assertEqual(len(temporary), 1)
        self.assertEqual(temporary[0].stat().st_mode & 0o777, 0o600)
        for offset in range(0, len(data), 4096):
            chunk = data[offset:offset + 4096]
            e.send(CHUNK, chunk)
            self.assertEqual(e.frame(ACK)[1], (offset + len(chunk)).to_bytes(8, "big"))
        self.assertFalse((e.receive / name).exists())
        e.send(END, len(data).to_bytes(8, "big"))
        self.assertEqual(e.frame(COMPLETE)[1], len(data).to_bytes(8, "big"))
        self.assertEqual((e.receive / name).read_bytes(), data)
        self.assertFalse(list(e.receive.glob(".secure-link-*")))

    def download(self, e, path, data):
        path.write_bytes(data)
        e.command(f"/file {path}")
        _, offer, _ = e.frame(OFFER)
        self.assertEqual(offer, len(data).to_bytes(8, "big") + path.name.encode())
        e.send(READY)
        got = bytearray()
        while len(got) < len(data):
            _, chunk, _ = e.frame(CHUNK)
            got.extend(chunk)
            e.send(ACK, len(got).to_bytes(8, "big"))
        self.assertEqual(got, data)
        self.assertEqual(e.frame(END)[1], len(data).to_bytes(8, "big"))
        e.send(COMPLETE, len(data).to_bytes(8, "big"))
        e.wait(f"File delivered: {path.name}")

    def test_files_both_directions_empty_multichunk_and_ten_mib(self):
        e = self.endpoint()
        e.handshake()
        for i, size in enumerate((0, 4099, LIMIT)):
            data = (bytes(range(256)) * ((size + 255) // 256))[:size]
            self.upload(e, f"in{i}.dat", data)
            self.download(e, self.root / f"out{i}.dat", data)
        self.assertIn("[FILE] Receiving in2.dat:", e.log())
        self.assertLessEqual(e.log().count("[FILE] Receiving in2.dat:"), 22)
        self.assertLessEqual(e.log().count("[FILE] Sent out2.dat:"), 22)
        e.exit()

    def test_unsafe_names_duplicates_size_limits_and_offer_collision(self):
        e = self.endpoint()
        e.handshake()
        for name in (b"../escape", b"/absolute", b"a/b", b"a\\b", b".hidden", b"nul\0x", b"x" * 129,
                     "tiếng".encode()):
            # Names over the metadata length bound are malformed, covered separately.
            if len(name) > 128:
                continue
            e.send(OFFER, bytes(8) + name)
            e.frame(REFUSE)
        e.send(OFFER, (LIMIT + 1).to_bytes(8, "big") + b"oversize")
        e.frame(REFUSE)
        (e.receive / "exists").write_bytes(b"original")
        e.send(OFFER, bytes(8) + b"exists")
        e.frame(REFUSE)
        self.assertEqual((e.receive / "exists").read_bytes(), b"original")
        (e.receive / "symlink").symlink_to(self.root / "outside")
        e.send(OFFER, bytes(8) + b"symlink")
        e.frame(REFUSE)
        source = self.root / "source"
        source.write_bytes(b"123")
        e.command(f"/file {source}")
        e.frame(OFFER)
        e.send(OFFER, bytes(8) + b"concurrent")
        e.frame(REFUSE)
        e.send(REFUSE)
        e.wait("Peer refused file")
        e.send(CHAT, b"usable after refusal")
        e.frame(RECEIPT)
        big = self.root / "big"
        with big.open("wb") as f:
            f.truncate(LIMIT + 1)
        e.command(f"/file {big}")
        e.wait("Cannot send file")
        e.exit()

    def test_incomplete_disconnect_bad_tag_replay_and_publish_collision(self):
        for fault in ("disconnect", "bad-tag", "replay", "short-end", "collision", "close", "signal"):
            with self.subTest(fault=fault):
                e = self.endpoint()
                e.handshake()
                e.send(OFFER, (8).to_bytes(8, "big") + b"unfinished")
                e.frame(READY)
                chunk = e.send(CHUNK, b"1234")
                e.frame(ACK)
                if fault == "disconnect":
                    e.sock.close()
                elif fault == "close":
                    e.send(CLOSE)
                elif fault == "signal":
                    e.process.terminate()
                elif fault == "bad-tag":
                    bad = bytearray(e.make(CHUNK, b"5678"))
                    bad[-1] ^= 1
                    e.write(bad)
                elif fault == "replay":
                    e.write(chunk)
                elif fault == "short-end":
                    e.send(END, (8).to_bytes(8, "big"))
                else:
                    e.send(CHUNK, b"5678")
                    e.frame(ACK)
                    (e.receive / "unfinished").write_bytes(b"existing-racer")
                    e.send(END, (8).to_bytes(8, "big"))
                self.assertEqual(e.process.wait(5), 0 if fault in ("close", "signal") else 1, e.log())
                self.assertFalse(list(e.receive.glob(".secure-link-*")))
                if fault == "collision":
                    self.assertEqual((e.receive / "unfinished").read_bytes(), b"existing-racer")
                else:
                    self.assertFalse((e.receive / "unfinished").exists())
                self.assertNotIn("File received:", e.log())

    def test_file_progress_timeout_even_with_authenticated_heartbeats(self):
        e = self.endpoint(timeout=1000)
        e.handshake()
        e.send(OFFER, (4).to_bytes(8, "big") + b"stalled")
        e.frame(READY)
        e.sock.settimeout(7)
        # frame() answers every authenticated PING. An otherwise live peer
        # must not keep an incomplete file alive indefinitely.
        with self.assertRaises(EOFError):
            e.frame()
        self.assertEqual(e.process.wait(3), 1, e.log())
        self.assertIn("Session timeout", e.log())
        self.assertFalse(list(e.receive.iterdir()))

    def test_send_timeout_when_peer_stops_reading(self):
        e = self.endpoint(timeout=200, socket_buffer=512, executable=self.short_write_cli)
        e.handshake()
        e.command("/msg " + "A" * 4091)
        self.assertEqual(e.process.wait(4), 1, e.log())
        self.assertIn("Session timeout (send", e.log())

    def test_simultaneous_large_chats_with_small_socket_buffers(self):
        e = self.endpoint(timeout=1000, socket_buffer=512, executable=self.short_write_cli)
        directory = self.root / "laptop_received"
        directory.mkdir(mode=0o700)
        log = tempfile.TemporaryFile()
        self.addCleanup(log.close)
        p = subprocess.Popen([str(self.short_write_cli), "--role", "laptop", "--test-fd", str(e.sock.fileno()),
                              "--psk", str(e.key_file), "--receive", str(directory), "--timeout-ms", "1000"],
                             pass_fds=(e.sock.fileno(),), stdin=subprocess.PIPE, stdout=log, stderr=log)
        def cleanup():
            if p.poll() is None:
                p.terminate()
                p.wait(5)
            p.stdin.close()
        self.addCleanup(cleanup)
        e.sock.close()
        e.wait("Session established:")
        deadline = time.monotonic() + 5
        while b"Session established:" not in os.pread(log.fileno(), 100000, 0):
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.005)
        e.command("/msg " + "A" * 4091)
        p.stdin.write(b"/msg " + b"B" * 4091 + b"\n")
        p.stdin.flush()
        e.wait("Chat delivered", timeout=4)
        deadline = time.monotonic() + 4
        while b"Chat delivered" not in os.pread(log.fileno(), 100000, 0):
            self.assertIsNone(p.poll(), os.pread(log.fileno(), 100000, 0).decode(errors="replace"))
            self.assertLess(time.monotonic(), deadline)
            time.sleep(0.005)
        p.stdin.write(b"/quit\n")
        p.stdin.flush()
        self.assertEqual(p.wait(5), 0)
        self.assertEqual(e.process.wait(5), 0, e.log())

    def test_two_real_clis_bidirectional_chat_files_and_demos(self):
        # Endpoint's Python peer is replaced by a second real C process.
        e = self.endpoint(timeout=2000)
        laptop_dir = self.root / "laptop_received"
        laptop_dir.mkdir(mode=0o700)
        log = tempfile.TemporaryFile()
        self.addCleanup(log.close)
        p = subprocess.Popen([str(self.test_cli), "--role", "laptop", "--test-fd", str(e.sock.fileno()),
                              "--psk", str(e.key_file), "--receive", str(laptop_dir), "--timeout-ms", "2000"],
                             pass_fds=(e.sock.fileno(),), stdin=subprocess.PIPE, stdout=log, stderr=log)
        def cleanup():
            if p.poll() is None:
                p.terminate()
                p.wait(5)
            p.stdin.close()
        self.addCleanup(cleanup)
        e.sock.close()
        def laptop_command(text):
            p.stdin.write(text.encode() + b"\n")
            p.stdin.flush()
        def wait_laptop(text):
            deadline = time.monotonic() + 5
            while time.monotonic() < deadline:
                content = os.pread(log.fileno(), 1000000, 0).decode(errors="replace")
                if text in content:
                    return
                time.sleep(0.005)
            self.fail(content)
        e.wait("Session established:")
        wait_laptop("Session established:")
        e.command("/msg from KV260 role")
        wait_laptop("Peer: from KV260 role")
        e.wait("Chat delivered")
        laptop_command("/msg from laptop role")
        e.wait("Peer: from laptop role")
        wait_laptop("Chat delivered")
        e.command("/tamper test mutation")
        wait_laptop("REJECTED:")
        e.wait("[DEMO] tamper")
        laptop_command("/replay test replay")
        e.wait("REJECTED:")
        first, second = self.root / "from_kv.dat", self.root / "from_laptop.dat"
        first.write_bytes(os.urandom(9001))
        second.write_bytes(os.urandom(5003))
        e.command(f"/file {first}")
        e.wait("File delivered: from_kv.dat")
        self.assertEqual((laptop_dir / first.name).read_bytes(), first.read_bytes())
        laptop_command(f"/file {second}")
        wait_laptop("File delivered: from_laptop.dat")
        self.assertEqual((e.receive / second.name).read_bytes(), second.read_bytes())
        laptop_command("/quit")
        self.assertEqual(p.wait(5), 0)
        self.assertEqual(e.process.wait(5), 0, e.log())
