"""Exercise Makefile cleanup on disposable checkouts with real filesystem inputs."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest

ROOT = Path(__file__).resolve().parents[2]


class CleanupTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.addCleanup(self.directory.cleanup)
        self.root = Path(self.directory.name) / "checkout"
        self.root.mkdir()
        subprocess.run(["git", "init", "-q", str(self.root)], check=True)
        self.put("script/clean.py", (ROOT / "script/clean.py").read_text())
        self.put("Makefile", (ROOT / "Makefile").read_text())
        for name in ("rtl/core.v", "dv/waves/test.wcfg", "docs/reference.pdf", "vivado/core/core.xpr"):
            self.put(name)
        subprocess.run(["git", "-C", str(self.root), "add", "."], check=True)
        self.kept = ["rtl/core.v", "dv/waves/test.wcfg", "docs/reference.pdf", "vivado/core/core.xpr",
                     "dv/vectors/custom/actual_stream.hex", "vectors/my.hex", ".venv/marker",
                     "artifacts/export.bit", "artifacts/session.wdb", "notes/dma_peak.log"]
        self.transient = [".Xil/job/cache", "vivado/core/.Xil/job/cache", "nested/deeper/.Xil/cache",
                          "vivado/core/core.cache/cache", "vivado/core/core.sim/behav/test.wdb",
                          "vivado.log", "vivado_123.jou", "hs_err_pid456.log", "script/__pycache__/test.pyc"]
        self.deep = ["vivado/core/core.runs/impl_1/core.bit", "vivado/core/core.gen/core.v",
                     "vivado/core/core.hw/core.lpr", "vivado/core/core.ip_user_files/file",
                     "vivado/core/core.srcs/utils_1/imports/synth_1/core.dcp", "build/host/test"]
        for name in self.kept + self.transient + self.deep:
            if not (self.root / name).exists():
                self.put(name)

    def put(self, name, content="keep exact contents\n"):
        path = self.root / name
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(content)
        return path

    def run_make(self, target, *arguments):
        result = subprocess.run(["make", "--no-print-directory", "-C", str(self.root), target, *arguments],
                                capture_output=True, text=True)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        return result.stdout

    def assert_present(self, names):
        for name in names:
            self.assertTrue((self.root / name).is_file(), name)

    def test_clean_keeps_implementation_inputs_and_unknown_logs(self):
        before = {name: (self.root / name).read_bytes() for name in self.kept + self.deep}
        self.run_make("clean")
        for name, content in before.items():
            self.assertEqual((self.root / name).read_bytes(), content, name)
        for name in self.transient:
            self.assertFalse((self.root / name).exists(), name)
        self.run_make("clean")  # Repeated cleanup is harmless.

    def test_distclean_removes_rebuildable_products(self):
        self.run_make("distclean")
        self.assert_present(self.kept)
        for name in self.transient + self.deep:
            self.assertFalse((self.root / name).exists(), name)

    def test_dry_run_and_default_help_do_not_change_files(self):
        before = {p.relative_to(self.root): p.read_bytes() for p in self.root.rglob("*") if p.is_file()}
        self.assertIn("would remove", self.run_make("distclean", "DRY_RUN=1"))
        self.run_make("help")
        after = {p.relative_to(self.root): p.read_bytes() for p in self.root.rglob("*") if p.is_file()}
        self.assertEqual(before, after)

    def test_tracked_output_name_protects_its_directory(self):
        self.put("vivado/core/core.runs/keep.v", "tracked source\n")
        self.put(".Xil/keep.v", "tracked source\n")
        subprocess.run(["git", "-C", str(self.root), "add", "vivado/core/core.runs/keep.v", ".Xil/keep.v"], check=True)
        self.run_make("distclean")
        self.assertEqual((self.root / ".Xil/keep.v").read_text(), "tracked source\n")
        self.assertEqual((self.root / "vivado/core/core.runs/keep.v").read_text(), "tracked source\n")

    def test_symlinks_are_not_followed_or_removed(self):
        outside = Path(self.directory.name) / "outside"
        outside.mkdir()
        (outside / "data").write_text("outside\n")
        (self.root / "nested/link").symlink_to(outside, target_is_directory=True)
        (self.root / "linked").mkdir()
        (self.root / "linked/.Xil").symlink_to(outside, target_is_directory=True)
        self.run_make("distclean")
        self.assertEqual((outside / "data").read_text(), "outside\n")
        self.assertTrue((self.root / "linked/.Xil").is_symlink())

    def test_matching_suffix_without_project_is_not_deleted(self):
        self.put("notes/research.runs/keep.txt")
        self.put("vivado/unknown/unknown.gen/keep.v")
        self.run_make("distclean")
        self.assert_present(["notes/research.runs/keep.txt", "vivado/unknown/unknown.gen/keep.v"])


if __name__ == "__main__":
    unittest.main()
