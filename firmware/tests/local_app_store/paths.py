"""Paths belonging to this test service, independent of the caller's directory."""
from pathlib import Path

BASE_DIR = Path(__file__).resolve().parent


def local_path(value):
    path = Path(value).expanduser()
    return (path if path.is_absolute() else BASE_DIR / path).resolve()
