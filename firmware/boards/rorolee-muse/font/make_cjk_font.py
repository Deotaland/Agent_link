#!/usr/bin/env python3
"""Compatibility entry point; the shared font generator lives under boards/common/fonts."""
from pathlib import Path
import runpy

if __name__ == "__main__":
    runpy.run_path(str(Path(__file__).resolve().parents[2] / "common/fonts/make_cjk_font.py"),
                  run_name="__main__")
