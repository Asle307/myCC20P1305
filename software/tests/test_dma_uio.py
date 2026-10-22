"""Compile the simple-mode host and verify packing/sequencing using golden fixtures."""
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "script"))
import gen_vectors as gen


class DmaTests(unittest.TestCase):
    def test_dma_host(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            flags = ["cc", "-std=c99", "-O2", "-Wall", "-Wextra", "-Werror"]
            host, model = root / "host", root / "model"
            subprocess.run(flags + [str(ROOT / "software/aead_dma_uio.c"), "-o", str(host)], check=True)
            subprocess.run(flags + ["-fsanitize=undefined", str(ROOT / "software/tests/test_dma_model.c"),
                                    "-o", str(model)], check=True)
            cases = [gen.golden(gen.rfc_vector(), mode) for mode in ("encrypt", "decrypt")]
            cases.append(gen.mutate(cases[1], "tag", 0))
            for name, aad, payload in (("empty", 0, 0), ("aad_only", 3, 0),
                                       ("partial", 3, 3), ("boundary", 17, 65), ("benchmark", 12, 16384)):
                vector = gen.Vector(name, bytes(range(32)), bytes(range(12)), bytes(range(aad)),
                                    bytes(i & 255 for i in range(payload)), "DMA host test")
                cases.extend(gen.golden(vector, mode) for mode in ("encrypt", "decrypt"))
            for case in cases:
                gen.emit_case(root, case, 32, "little")
                folder = root / case.slug
                inspect = subprocess.run([str(host), "--case", str(folder), "--inspect"], capture_output=True, text=True)
                self.assertEqual(inspect.returncode, 0, inspect.stderr)
                tx_bytes = ((len(case.vector.aad) + 3) // 4 * 4 +
                            (len(case.input_payload) + 3) // 4 * 4 + (16 if case.mode == "decrypt" else 0))
                self.assertIn(f"DMA TX={tx_bytes} bytes", inspect.stdout)
                result = subprocess.run([str(model), str(folder)], capture_output=True, text=True)
                self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
                self.assertIn("PASS DMA host", result.stdout)
                self.assertNotIn("runtime error", result.stderr)
            for args in (("--iterations", "0"), ("--warmup", "-1"), ("--clock-hz", "0"),
                         ("--timeout-ms", "junk"), ()):
                self.assertEqual(subprocess.run([str(host), *args], capture_output=True).returncode, 2)
            self.assertEqual(subprocess.run([str(host), "--help"], capture_output=True).returncode, 0)


if __name__ == "__main__":
    unittest.main()
