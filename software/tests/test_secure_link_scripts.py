"""Exercise shell helpers without a board, radio, real SSH or real secrets."""
import os
from pathlib import Path
import shutil
import subprocess
import tarfile
import tempfile
import time
import unittest

ROOT = Path(__file__).resolve().parents[2]


class ScriptTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.project = self.root / "project with spaces"
        self.software = self.project / "software"
        self.source = self.software / "secure_link"
        self.source.mkdir(parents=True)
        for name in ("aead_uio.c", "aead_uio_api.c", "aead_uio_api.h", "aead_uio_internal.h"):
            shutil.copy2(ROOT / "software" / name, self.software / name)
        for name in ("main.c", "protocol.c", "protocol.h", "transfer.c", "transfer.h",
                     "transport.c", "transport.h", "laptop.sh", "board.sh", "README.md"):
            shutil.copy2(ROOT / "software/secure_link" / name, self.source / name)
        self.fake_bin = self.root / "tools"
        self.fake_bin.mkdir()
        self.argv = self.root / "arguments"
        self.capture = self.root / "ssh-input"
        self.psk = self.root / "private directory" / "link.psk"
        self.psk.parent.mkdir(mode=0o700)
        self.psk.write_bytes(bytes(range(32)))
        self.psk.chmod(0o600)
        self.endpoint = self.root / "endpoint stub"
        self.endpoint.write_text("#!/bin/sh\nprintf 'CALL\\0' >> \"$SL_TEST_ARGV\"\n"
                                 "printf '%s\\0' \"$@\" >> \"$SL_TEST_ARGV\"\n"
                                 "printf '%s\\n' '[OK] Stub endpoint'\n")
        self.endpoint.chmod(0o700)
        future = time.time() + 10
        os.utime(self.endpoint, (future, future))
        self.env = os.environ.copy()
        self.env.update({"PATH": str(self.fake_bin) + os.pathsep + self.env.get("PATH", ""),
                         "SL_BINARY": str(self.endpoint), "SL_PSK": str(self.psk),
                         "SL_RECEIVE": str(self.root / "received files"),
                         "SL_TEST_ARGV": str(self.argv), "SL_TEST_CAPTURE": str(self.capture),
                         "SL_AUTO_NIX": "0"})

    def run_script(self, script, *args, **env):
        return subprocess.run([str(self.source / script), *args], env={**self.env, **env},
                              text=True, capture_output=True, timeout=15)

    def fake(self, name, content):
        path = self.fake_bin / name
        path.write_text("#!/bin/sh\n" + content)
        path.chmod(0o700)
        return path

    def test_laptop_start_preserves_paths_and_config(self):
        result = self.run_script("laptop.sh", "start", SL_BLUETOOTH_ADDR="01:02:03:04:05:06",
                                 SL_CHANNEL="3", SL_TIMEOUT_MS="12345", SL_VERBOSE="1")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue((self.root / "received files").is_dir())
        self.assertIn("[INFO] Connecting", result.stdout)
        self.assertNotIn(self.psk.read_bytes().hex(), result.stdout + result.stderr)
        args = self.argv.read_bytes().decode().split("\0")
        self.assertEqual(args, ["CALL", "--check-key", str(self.psk), "CALL", "--role", "laptop",
                                "--bluetooth", "01:02:03:04:05:06", "--channel", "3", "--psk", str(self.psk),
                                "--receive", str(self.root / "received files"), "--timeout-ms", "12345",
                                "--verbose", ""])

    def test_deploy_archive_excludes_keys_received_files_and_binary(self):
        (self.source / "private.psk").write_bytes(b"secret key never deployed")
        (self.source / "received").mkdir()
        (self.source / "received" / "private.txt").write_text("private file")
        (self.source / "build.bin").write_bytes(b"not a source file")
        self.fake("ssh", "printf '%s\\n' \"$@\" > \"$SL_TEST_ARGV\"\ncat > \"$SL_TEST_CAPTURE\"\n")
        result = self.run_script("laptop.sh", "deploy", SL_BOARD_HOST="debian@example")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("[OK] Source copied", result.stdout)
        call = self.argv.read_text()
        self.assertIn("debian@example", call)
        self.assertIn("~/Asle/secure-link", call)
        self.assertNotIn("private", result.stdout + result.stderr)
        with tarfile.open(self.capture) as archive:
            names = archive.getnames()
            self.assertIn("aead_uio_api.c", names)
            self.assertIn("secure_link/board.sh", names)
            self.assertIn("secure_link/laptop.sh", names)
            self.assertFalse(any("psk" in n.lower() or "received" in n or n.endswith(".bin") for n in names))
            self.assertTrue(archive.getmember("secure_link/board.sh").mode & 0o111)

    def test_copy_key_stream_is_private_and_destination_is_no_clobber(self):
        self.fake("ssh", "printf '%s\\n' \"$@\" > \"$SL_TEST_ARGV.ssh\"\ncat > \"$SL_TEST_CAPTURE\"\n")
        result = self.run_script("laptop.sh", "copy-key")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.capture.read_bytes(), self.psk.read_bytes())
        command = (self.root / "arguments.ssh").read_text().splitlines()[-1]
        self.assertIn("ln -T", command)
        self.assertIn("wc -c", command)
        self.assertEqual(subprocess.run(["sh", "-n", "-c", command]).returncode, 0)
        self.assertNotIn(self.psk.read_bytes().hex(), result.stdout + result.stderr)
        self.fake("ssh", "exit 1\n")
        failure = self.run_script("laptop.sh", "copy-key")
        self.assertNotEqual(failure.returncode, 0)
        self.assertNotIn("[OK] PSK copied", failure.stdout)
        self.assertEqual(self.psk.read_bytes(), bytes(range(32)))

    def test_copy_key_remote_command_rejects_existing_and_partial_keys(self):
        board_dir = self.root / "board area" / "secure-link"
        board_dir.mkdir(parents=True)
        ssh = self.fake_bin / "ssh"
        ssh.write_text("#!/usr/bin/env python3\n"
                       "import os, shlex, subprocess, sys\n"
                       "command = sys.argv[-1].replace('~/Asle/secure-link', "
                       "shlex.quote(os.environ['SL_TEST_BOARD_DIR']))\n"
                       "sys.exit(subprocess.run(['sh', '-c', command]).returncode)\n")
        ssh.chmod(0o700)
        env = {"SL_TEST_BOARD_DIR": str(board_dir)}
        result = self.run_script("laptop.sh", "copy-key", **env)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        destination = board_dir / "link.psk"
        self.assertEqual(destination.read_bytes(), bytes(range(32)))
        self.assertEqual(destination.stat().st_mode & 0o777, 0o600)
        self.psk.write_bytes(b"x" * 32)
        result = self.run_script("laptop.sh", "copy-key", **env)
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(destination.read_bytes(), bytes(range(32)))
        self.assertFalse(list(board_dir.glob(".psk.*")))
        destination.unlink()
        self.psk.write_bytes(b"x" * 31)
        result = self.run_script("laptop.sh", "copy-key", **env)
        self.assertNotEqual(result.returncode, 0)
        self.assertFalse(destination.exists())
        self.assertFalse(list(board_dir.glob(".psk.*")))

    def test_missing_source_never_runs_stale_binary(self):
        (self.source / "transport.c").unlink()
        result = self.run_script("laptop.sh", "start")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("[ERROR] Missing source:", result.stderr)
        self.assertFalse(self.argv.exists())

    def test_board_refuses_unknown_fpga_before_running_binary(self):
        self.fake("cat", "printf 'unknown\\n'\n")
        result = self.run_script("board.sh", "start")
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("[ERROR] FPGA manager is not operating", result.stderr)
        self.assertFalse(self.argv.exists())

    def test_board_build_is_atomic_and_does_not_probe_hardware(self):
        output = self.root / "board build" / "secure_link_kv260"
        self.fake("cat", "printf '%s\\n' 'FPGA should not be checked during build' >&2; exit 1\n")
        cc = self.fake("cc-stub", "printf '%s\\n' \"$@\" > \"$SL_TEST_ARGV\"\n"
                       "while [ $# -gt 0 ]; do if [ \"$1\" = -o ]; then shift; out=$1; fi; shift; done\n"
                       "printf '#!/bin/sh\\nexit 0\\n' > \"$out\"\nchmod 700 \"$out\"\n")
        result = self.run_script("board.sh", "build", SL_BINARY=str(output), CC=str(cc))
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("[OK] KV260 CLI built", result.stdout)
        self.assertNotIn("should not be checked", result.stderr)
        self.assertIn("-DSL_SERIAL_ONLY", self.argv.read_text())
        original = output.read_bytes()
        self.fake("cc-stub", "exit 7\n")
        result = self.run_script("board.sh", "build", SL_BINARY=str(output), CC=str(cc))
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(output.read_bytes(), original)
        self.assertFalse(list(output.parent.glob(".secure-link-build.*")))

    def test_board_start_rebuilds_even_when_binary_timestamp_is_newer(self):
        self.fake("cat", "printf 'operating\\n'\n")
        cc = self.fake("cc-stub", "printf '%s\\n' \"$@\" > \"$SL_TEST_CC_LOG\"\n"
                       "while [ $# -gt 0 ]; do if [ \"$1\" = -o ]; then shift; out=$1; fi; shift; done\n"
                       "cp \"$SL_BINARY\" \"$out\"\n"
                       "chmod 700 \"$out\"\n")
        cc_log = self.root / "compiler arguments"
        result = self.run_script("board.sh", "start", CC=str(cc), SL_TEST_CC_LOG=str(cc_log),
                                 SL_SERIAL="/dev/null", SL_UIO="/dev/null")
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertIn("-DSL_SERIAL_ONLY", cc_log.read_text())
        args = self.argv.read_bytes().decode().split("\0")
        self.assertIn("--check-key", args)
        self.assertIn("--role", args)
        self.assertIn("kv260", args)

    def test_usage_and_argument_validation(self):
        for name in ("board.sh", "laptop.sh"):
            result = self.run_script(name, "help")
            self.assertEqual(result.returncode, 0)
            self.assertTrue(all(line.startswith("[HELP]") for line in result.stdout.splitlines()))
            self.assertEqual(self.run_script(name, "invalid").returncode, 2)
            self.assertEqual(self.run_script(name, "start", "extra").returncode, 2)


if __name__ == "__main__":
    unittest.main()
