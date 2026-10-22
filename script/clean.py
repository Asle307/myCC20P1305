#!/usr/bin/env python3
"""Remove allowlisted generated output without touching tracked or user inputs.

Usage: python3 script/clean.py --mode clean|distclean [--dry-run]
Input: the Git checkout containing this script. Output: relative cleanup paths.
Exit codes: 0 success, 2 invalid checkout/options, 1 filesystem/Git failure.
Uses Python's standard library and Git; does not follow directory symlinks.
"""

import argparse
import fnmatch
import os
from pathlib import Path
import shutil
import subprocess
import sys

PRESERVE = (".git", ".venv", "dv/vectors", "vectors", "artifacts")
TRANSIENT_SUFFIXES = (".cache", ".sim")
DEEP_SUFFIXES = (".runs", ".gen", ".hw", ".ip_user_files")
SESSION_PATTERNS = (
    "vivado*.log", "vivado*.jou", "vivado*.str", "hs_err_pid*.log",
    "xsim*.log", "xsim*.jou", "xelab*.log", "xvlog*.log", "xvhdl*.log",
    "webtalk*.log", "webtalk*.jou", "webtalk*.xml", "clockInfo.txt",
    "*.wdb", "*.wdb.journal", "*.vcd", "*.pb",
)


def tracked_paths(root):
    """Refuse non-checkouts and collect every tracked path/ancestor to protect."""
    git = ["git", "-C", str(root)]
    top = subprocess.check_output(git + ["rev-parse", "--show-toplevel"], stderr=subprocess.PIPE)
    if Path(os.fsdecode(top).strip()).resolve() != root:
        raise ValueError("The script must be located at its checkout's script/clean.py")
    names = subprocess.check_output(git + ["ls-files", "-z"]).split(b"\0")
    protected = set()
    for name in names:
        if name:
            path = Path(os.fsdecode(name))
            protected.add(path.as_posix())
            protected.update(p.as_posix() for p in path.parents if p != Path("."))
    return protected


def preserved(name):
    return any(name == prefix or name.startswith(prefix + "/") for prefix in PRESERVE)


def project_product(root, relative, suffixes):
    """A project output sits beside the XPR that gives it its basename."""
    return any(
        relative.name.endswith(suffix)
        and (root / relative.parent / (relative.name[:-len(suffix)] + ".xpr")).is_file()
        for suffix in suffixes
    ) and relative.parts[0] == "vivado"


def candidates(root, mode, protected):
    """Yield disjoint, allowlisted files/directories; prune protected inputs."""
    def visit(folder):
        with os.scandir(root / folder) as entries:
            items = sorted(entries, key=lambda entry: entry.name)
        for entry in items:
            relative = folder / entry.name
            name = relative.as_posix()
            if preserved(name) or entry.is_symlink():
                continue
            directory = entry.is_dir(follow_symlinks=False)
            selected = False
            if directory:
                selected = entry.name in (".Xil", "__pycache__") or project_product(
                    root, relative, TRANSIENT_SUFFIXES
                )
                if mode == "distclean":
                    selected |= relative == Path("build") or project_product(root, relative, DEEP_SUFFIXES)
                    if relative.name == "utils_1" and relative.parent.name.endswith(".srcs"):
                        parent = relative.parent
                        selected |= project_product(root, parent, (".srcs",))
            else:
                selected = any(fnmatch.fnmatchcase(entry.name, pattern) for pattern in SESSION_PATTERNS)
                if "vivado" == relative.parts[0]:
                    selected |= fnmatch.fnmatchcase(entry.name, "*.xpr.backup*")
            # Never remove a tracked file, or a directory containing one.
            if selected and name not in protected:
                yield relative
            elif directory:
                yield from visit(relative)
    return visit(Path("."))


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--mode", choices=("clean", "distclean"), required=True)
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    root = Path(__file__).resolve().parents[1]
    try:
        protected = tracked_paths(root)
        paths = list(candidates(root, args.mode, protected))
        for relative in paths:
            target = root / relative
            print(("would remove " if args.dry_run else "remove ") + relative.as_posix())
            if not args.dry_run:
                if target.is_dir():
                    shutil.rmtree(target)
                else:
                    target.unlink()
        print(f"{args.mode}: {len(paths)} generated path(s) " + ("selected" if args.dry_run else "removed"))
        return 0
    except ValueError as error:
        print(f"cleanup: {error}", file=sys.stderr)
        return 2
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"cleanup: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
