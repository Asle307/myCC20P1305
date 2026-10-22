"""Reference, export and post-simulation verification contracts."""

import contextlib
import io
import json
from pathlib import Path
import random
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import gen_vectors as gen


class VectorTests(unittest.TestCase):
    def command(self, arguments):
        with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(io.StringIO()):
            return gen.main(arguments)

    def test_rfc_and_packing(self):
        self.assertEqual(self.command(["selftest"]), 0)
        vector = gen.rfc_vector()
        case = gen.golden(vector, "encrypt")
        self.assertEqual(case.ciphertext, gen.RFC_CIPHERTEXT)
        self.assertEqual(case.tag, gen.RFC_TAG)
        for width in (4, 16):
            for order in ("little", "big"):
                for length in range(2 * width + 1):
                    data = bytes(range(length))
                    packed = gen.pack_words(data, width, order)
                    self.assertEqual(b"".join(word.to_bytes(width, order) for word in packed)[:length], data)

    def test_full_suite_coverage_without_random(self):
        args = gen.build_parser().parse_args(["generate", "--random-count", "0"])
        vectors = gen.make_vectors(args)
        self.assertEqual({(len(v.aad) - 1) % 16 + 1 for v in vectors if v.aad}, set(range(1, 17)))
        self.assertEqual({(len(v.plaintext) - 1) % 64 + 1 for v in vectors if v.plaintext}, set(range(1, 65)))
        self.assertEqual(max(len(v.plaintext) for v in vectors), 4096)
        case = gen.golden(gen.rfc_vector(), "decrypt")
        for bit in range(128):
            changed = gen.mutate(case, "tag", bit)
            self.assertFalse(changed.auth_ok)
            self.assertEqual(changed.output_payload, case.output_payload)
            self.assertEqual(int.from_bytes(changed.received_tag, "little") ^
                             int.from_bytes(case.tag, "little"), 1 << bit)
        for field in ("aad", "ciphertext"):
            changed = gen.mutate(case, field, 0)
            self.assertFalse(changed.auth_ok)
            self.assertEqual(changed.received_tag, case.tag)
            self.assertNotEqual(changed.tag, case.tag)

    def test_empty_sections_and_inline_tag_adapter(self):
        vector = gen.Vector("empty", bytes(32), bytes(12), b"", b"", "test")
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for mode in ("encrypt", "decrypt"):
                gen.emit_case(root, gen.golden(vector, mode), 32, "little")
            enc, dec = root / "empty_enc", root / "empty_dec"
            self.assertEqual((enc / "aad.hex").read_text(), "")
            self.assertEqual(gen.read_word_file(enc / "rtl_input_control.hex"), [0x300, 0x200])
            self.assertEqual(len(gen.read_word_file(enc / "rtl_expected.hex")), 4)
            self.assertEqual(gen.read_word_file(dec / "rtl_input_control.hex"),
                             [0x300, 0x200, 4, 4, 4, 0x204])
            self.assertEqual((dec / "expected_stream.hex").read_text(), "")
            self.assertEqual((dec / "rtl_expected.hex").read_text(), "")

    def test_native_adapter_independent_of_generic_packing(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            for width, order in ((32, "little"), (128, "big")):
                out = root / f"{width}_{order}"
                self.assertEqual(self.command(["generate", "--profile", "rfc", "--word-bits", str(width),
                                               "--byteorder", order, "--out", str(out)]), 0)
            for name in ("rtl_config.hex", "rtl_input.hex", "rtl_expected.hex", "rtl_expected_control.hex"):
                self.assertEqual((root / "32_little/rfc8439_enc" / name).read_bytes(),
                                 (root / "128_big/rfc8439_enc" / name).read_bytes())
            case = root / "128_big/rfc8439_enc"
            (case / "actual_stream.hex").write_bytes((case / "rtl_expected.hex").read_bytes())
            (case / "actual_status.hex").write_text("00000000\n")
            self.assertEqual(self.command(["verify", "--case", str(case)]), 0)
            # Changing payload must fail even when an AUTH_OK capture exists.
            (case / "actual_stream.hex").write_text("00000000\n")
            self.assertEqual(self.command(["verify", "--case", str(case)]), 1)

    def test_authentication_status_and_bad_tag_exports(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            case = gen.mutate(gen.golden(gen.rfc_vector(), "decrypt"), "tag", 127)
            gen.emit_case(root, case, 32, "little")
            folder = root / case.slug
            self.assertNotEqual((folder / "input_tag.hex").read_text(), (folder / "expected_tag.hex").read_text())
            (folder / "actual_stream.hex").write_bytes((folder / "rtl_expected.hex").read_bytes())
            (folder / "actual_status.hex").write_text("00000000\n")
            self.assertEqual(self.command(["verify", "--case", str(folder)]), 0)
            (folder / "actual_status.hex").write_text("00000001\n")
            self.assertEqual(self.command(["verify", "--case", str(folder)]), 1)
            (folder / "actual_stream.hex").write_text("xxxxxxxx\n")
            self.assertEqual(self.command(["verify", "--case", str(folder)]), 2)

    def test_custom_binary_input_and_seed_replay(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            data = b"\x00\xff\x03" * 7
            (root / "data.bin").write_bytes(data)
            output = root / "custom"
            args = ["generate", "--profile", "custom", "--plaintext-file", str(root / "data.bin"),
                    "--aad-hex", "0x01:02_03-04", "--seed", "123", "--out", str(output)]
            self.assertEqual(self.command(args), 0)
            info = json.loads((output / "custom_enc/case.json").read_text())
            self.assertEqual(bytes.fromhex(info["plaintext_hex_bytes"]), data)
            self.assertEqual(info["aad_hex_bytes"], "01020304")
            fixture = (output / "custom_enc/rtl_input.hex").read_bytes()
            self.assertEqual(self.command(args + ["--force"]), 0)
            self.assertEqual((output / "custom_enc/rtl_input.hex").read_bytes(), fixture)
            self.assertEqual(self.command(["inspect", str(output)]), 0)
            self.assertEqual(self.command(["inspect", str(output / "custom_enc")]), 0)

    def test_force_ownership_and_failed_generation_preserve_old_suite(self):
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory)
            sentinel = output / "keep.txt"
            sentinel.write_text("keep")
            args = ["generate", "--profile", "rfc", "--out", str(output), "--force"]
            self.assertEqual(self.command(args), 2)
            self.assertEqual(sentinel.read_text(), "keep")
            (output / gen.MAGIC_FILE).write_text("cc20p1305-vectorgen v1\n")
            with patch.object(gen, "emit_case", side_effect=OSError("test write failure")):
                self.assertEqual(self.command(args), 2)
            self.assertEqual(sentinel.read_text(), "keep")
            self.assertEqual(self.command(args), 0)
            self.assertFalse(sentinel.exists())

    def test_random_profile_has_no_fixed_cases(self):
        args = gen.build_parser().parse_args(["generate", "--profile", "random", "--random-count", "3"])
        first = gen.make_vectors(args)
        self.assertEqual(len(first), 3)
        self.assertEqual(first, gen.make_vectors(args))
        self.assertTrue(all(v.name.startswith("random_") for v in first))

    def test_bad_options_do_not_write_output(self):
        invalid = [
            ["--random-count", "-1"], ["--max-random-bytes", "65537"],
            ["--profile", "suite", "--mode", "encrypt"],
            ["--profile", "random", "--random-count", "0"],
            ["--profile", "rfc", "--aad-hex", "00"],
            ["--profile", "custom", "--key-hex", "00"],
            ["--profile", "custom", "--nonce-hex", "gg"],
            ["--profile", "custom", "--plaintext-hex", "0"],
            ["--profile", "custom", "--aad-hex", "00", "--aad-file", "nonexistent"],
        ]
        with tempfile.TemporaryDirectory() as directory:
            output = Path(directory) / "absent"
            for arguments in invalid:
                with self.subTest(arguments=arguments):
                    self.assertEqual(self.command(["generate", "--out", str(output)] + arguments), 2)
                    self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
