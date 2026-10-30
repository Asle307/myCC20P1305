"""Compile the host and test MMIO sequencing against generator-produced fixtures."""

import json
import re
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "script"))
import gen_vectors as gen


class HostTests(unittest.TestCase):
    def test_builtin_rfc_literals(self):
        source = (ROOT / "software/aead_uio.c").read_text()
        cipher = re.search(r"static const char ciphertext_hex\[\] =\s*(.*?);", source, re.S)
        plain = re.search(r"static const uint8_t plaintext\[\] =\s*(.*?);", source, re.S)
        self.assertEqual(bytes.fromhex("".join(re.findall(r'"([0-9a-f]+)"', cipher[1]))), gen.RFC_CIPHERTEXT)
        self.assertEqual("".join(re.findall(r'"([^\"]*)"', plain[1])).encode("ascii"), gen.rfc_vector().plaintext)
        for literal in (gen.rfc_vector().nonce.hex(), gen.rfc_vector().aad.hex(), gen.RFC_TAG.hex()):
            self.assertIn('decode_hex("' + literal + '"', source)

    def test_host_and_slot_model(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            cases = [gen.golden(gen.rfc_vector(), mode) for mode in ("encrypt", "decrypt")]
            cases.append(gen.mutate(cases[1], "tag", 0))
            for name, aad_bytes, payload_bytes in (("empty", 0, 0), ("partial", 3, 3),
                                                  ("boundary", 17, 65), ("benchmark", 12, 16384)):
                vector = gen.Vector(name, bytes(range(32)), bytes(range(12)), bytes(range(aad_bytes)),
                                    bytes(i & 255 for i in range(payload_bytes)), "host model")
                cases.extend(gen.golden(vector, mode) for mode in ("encrypt", "decrypt"))
            declarations, entries = [], []

            def array(name, values, kind):
                declarations.append(f"static const {kind} {name}[] = {{" +
                                    ",".join(hex(v) for v in values or [0]) + "};")
                return name

            for index, case in enumerate(cases):
                gen.emit_case(root, case, 32, "little")
                folder = root / case.slug
                prefix = f"v{index}_"
                fields = []
                for label, data in (("key", case.vector.key), ("nonce", case.vector.nonce),
                                    ("aad", case.vector.aad), ("input", case.input_payload),
                                    ("expected", case.output_payload),
                                    ("tag", case.tag if case.mode == "encrypt" else case.received_tag)):
                    fields.append(array(prefix + label, list(data), "uint8_t"))
                test = "{" + json.dumps(case.slug) + "," + ",".join(fields) + "," + ",".join(map(str, (
                    len(case.vector.aad), len(case.input_payload), int(case.mode == "encrypt"), int(case.auth_ok)))) + "}"
                refs = []
                for label in ("input", "input_control", "expected", "expected_control"):
                    refs.append(array(prefix + "words_" + label,
                                      gen.read_word_file(folder / f"rtl_{label}.hex"), "uint32_t"))
                config = gen.pack_words(case.vector.key + case.vector.nonce, 4, "little")
                refs.append(array(prefix + "config", config, "uint32_t"))
                metadata = json.loads((folder / "case.json").read_text())["rtl"]
                entries.append("{" + test + "," + ",".join(refs) + "," +
                               str(metadata["input_beats"]) + "," + str(metadata["expected_beats"]) + "}")
            (root / "vectors.h").write_text("\n".join(declarations) +
                                            "\nstatic const struct fixture fixtures[] = {\n" +
                                            ",\n".join(entries) + "\n};\n")
            flags = ["cc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror"]
            host, model = root / "aead_uio", root / "test_uio_model"
            subprocess.run(flags + [str(ROOT / "software/aead_uio.c"), str(ROOT / "software/aead_uio_api.c"), "-o", str(host)], check=True)
            subprocess.run(flags + ["-fsanitize=address,undefined", "-I" + str(root),
                                    str(ROOT / "software/tests/test_uio_model.c"), str(ROOT / "software/aead_uio_api.c"), "-o", str(model)], check=True)
            guard = root / "test_uio_guard"
            subprocess.run(flags + ["-fsanitize=address,undefined",
                                    str(ROOT / "software/tests/test_uio_guard.c"),
                                    str(ROOT / "software/aead_uio_api.c"),
                                    "-Wl,--wrap=fopen,--wrap=open", "-o", str(guard)], check=True)
            checked = subprocess.run([str(guard)], capture_output=True, text=True)
            self.assertEqual(checked.returncode, 0, checked.stdout + checked.stderr)
            self.assertIn("PASS FPGA state guard", checked.stdout)
            result = subprocess.run([str(model)], capture_output=True, text=True)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            self.assertIn("PASS UIO host", result.stdout)
            self.assertEqual(result.stdout.count("BENCH encrypt"), 5)
            self.assertEqual(result.stdout.count("BENCH decrypt"), 5)
            rows = re.findall(r"BENCH (encrypt|decrypt)\s*: bytes=(\d+), messages=(\d+), elapsed=([\d.]+) s\n"
                              r"  ([\d.]+) MB/s \| ([\d.]+) MiB/s \| ([\d.]+) Mbit/s \| "
                              r"([\d.]+) us/message \| ([\d.]+) messages/s", result.stdout)
            self.assertEqual(len(rows), 10)
            for _, byte_count, messages, _, mbps, mibps, mbit, latency, rate in rows:
                self.assertEqual(int(messages), 2)
                self.assertGreater(float(latency), 0)
                self.assertGreater(float(rate), 0)
                self.assertAlmostEqual(float(mbps) * 8, float(mbit), delta=0.005)
                self.assertAlmostEqual(float(mibps) * 1048576 / 1e6, float(mbps), delta=0.002)
                self.assertAlmostEqual(float(mbps), int(byte_count) / 2 * float(rate) / 1e6, delta=0.002)
            self.assertNotIn("runtime error", result.stderr)
            self.assertNotRegex(result.stderr, r"AEAD API:.*data [0-9a-f]{8}")
            self.assertIn("AEAD API: output metadata", result.stderr)
            self.assertIn("timeout", result.stderr)
            self.assertIn("Benchmark failed at encrypt iteration 1; no rate for this mode.", result.stderr)
            self.assertEqual(subprocess.run([str(host), "--help"], capture_output=True).returncode, 0)
            for options in (("--timeout-ms", "0"), ("--test", "invalid"), ("--timeout-ms", "junk"),
                            ("--bytes", "1024"), ("--bench", "--pause"), ("--bench", "--test", "bad-tag"),
                            ("--bench", "--bytes", "-1"), ("--bench", "--bytes", "16777217"),
                            ("--bench", "--bytes", "1K"), ("--bench", "--bytes", "+1"),
                            ("--bench", "--iterations", "0"), ("--bench", "--warmup", "1000001"),
                            ("--bench", "--aad-bytes", "1048577")):
                self.assertEqual(subprocess.run([str(host), *options], capture_output=True).returncode, 2)


if __name__ == "__main__":
    unittest.main()
