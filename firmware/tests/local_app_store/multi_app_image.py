"""Local-store adapter to the production image ABI and board layout."""
from pathlib import Path
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from firmware_image import DESCRIPTOR, image_info as inspect_image
from firmware_manifest import resolve_profiles

PROFILE = resolve_profiles(ROOT)["waveshare-amoled-175-wifi"]
BOARD = PROFILE["model"]
LAYOUT = PROFILE["layout_info"]
APP_IMAGE_MAX_BYTES = min(LAYOUT["partitions"][slot]["size"] for slot in LAYOUT["app_slots"]) - LAYOUT["cover_reserve"]


def image_info(path):
    value = inspect_image(path, BOARD, LAYOUT["version"])
    maximum = LAYOUT["partitions"]["factory"]["size"] if value["app_id"] == "agent.launcher" else APP_IMAGE_MAX_BYTES
    if value["size"] > maximum:
        raise ValueError(f"Application image overlaps the reserved {LAYOUT['cover_reserve'] // 1024} KiB slot cover")
    return value
