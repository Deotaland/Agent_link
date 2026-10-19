#!/usr/bin/env python3
"""Install this repository's agent skills by copying skills/<name>/ into a skills folder.

usage: python tools/install_skills.py --user          ~/.claude/skills (Claude Code, every project)
       python tools/install_skills.py --project       .claude/skills in this checkout (gitignored)
       python tools/install_skills.py --dest DIR      any folder a tool loads skills from
       add --check to only report what would change, --force to replace folders this script
       didn't install
"""
import argparse
import filecmp
import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
SOURCE = ROOT / "skills"
MARKER = ".installed-by-agent-link"


def skills():
    return sorted(d for d in SOURCE.iterdir() if (d / "SKILL.md").is_file())


def same_tree(a, b):
    """True if b holds exactly a's files (ignoring the marker and Python caches)."""
    skip = {MARKER, "__pycache__"}
    cmp = filecmp.dircmp(a, b, ignore=list(skip))
    if cmp.left_only or cmp.right_only or cmp.diff_files or cmp.funny_files:
        return False
    _, mismatch, errors = filecmp.cmpfiles(a, b, cmp.common_files, shallow=False)
    if mismatch or errors:
        return False
    return all(same_tree(a / d, b / d) for d in cmp.common_dirs)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    where = ap.add_mutually_exclusive_group(required=True)
    where.add_argument("--user", action="store_true", help="install to ~/.claude/skills")
    where.add_argument("--project", action="store_true", help="install to .claude/skills in this checkout")
    where.add_argument("--dest", type=Path, help="install to this folder")
    ap.add_argument("--check", action="store_true", help="report only, change nothing")
    ap.add_argument("--force", action="store_true", help="replace folders this script didn't install")
    args = ap.parse_args(argv)

    dest = Path.home() / ".claude" / "skills" if args.user else ROOT / ".claude" / "skills" if args.project else args.dest
    dest = dest.expanduser().resolve()
    print(f"Skills from {SOURCE}\n        to {dest}")
    blocked = changed = 0
    for src in skills():
        target = dest / src.name
        if not target.exists():
            state = "install"
        elif same_tree(src, target):
            print(f"  up to date   {src.name}")
            continue
        elif (target / MARKER).exists() or args.force:
            state = "update"
        else:
            print(f"  SKIPPED      {src.name}: {target} exists and wasn't installed by this script (use --force)")
            blocked += 1
            continue
        changed += 1
        if args.check:
            print(f"  would {state:<8}{src.name}")
            continue
        if target.is_dir():
            shutil.rmtree(target)
        elif target.exists():
            target.unlink()
        shutil.copytree(src, target, ignore=shutil.ignore_patterns("__pycache__"))
        (target / MARKER).write_text(f"Installed from {src} by tools/install_skills.py\n", encoding="utf-8")
        print(f"  {'installed' if state == 'install' else 'updated':<12} {src.name}")
    if args.check:
        print(f"{changed} to change, {blocked} blocked.")
    return 1 if blocked else 0


if __name__ == "__main__":
    sys.exit(main())
