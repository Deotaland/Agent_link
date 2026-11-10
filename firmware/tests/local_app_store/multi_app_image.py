"""Local-store adapter to the production image ABI and board layout."""
from pathlib import Path
from functools import lru_cache
import sys

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / "tools"))
from firmware_image import DESCRIPTOR, image_info as inspect_image
from firmware_manifest import resolve_profiles
from sdgoods_image import image_info as inspect_sdgoods

DEFAULT_PROFILE = "waveshare-amoled-175-wifi"
PROFILES = resolve_profiles(ROOT)
PROFILE = PROFILES[DEFAULT_PROFILE]
BOARD = PROFILE["model"]
LAYOUT = PROFILE["layout_info"]
APP_IMAGE_MAX_BYTES = min(LAYOUT["partitions"][slot]["size"] for slot in LAYOUT["app_slots"]) - LAYOUT["cover_reserve"]


@lru_cache(maxsize=None)
def target(profile=DEFAULT_PROFILE):
    value = PROFILES[profile]
    layout = value['layout_info']
    if layout['mode'] != 'managed':
        raise ValueError('The test store requires a managed Factory profile')
    maximum = min(layout['partitions'][slot]['size'] for slot in layout['app_slots']) - layout['cover_reserve']
    return dict(board=value['model'], layout=layout, maximum=maximum,
                chip=value['target'], vendor=value['board']=='sdgoods-esp32s3')


def image_info(path, profile=DEFAULT_PROFILE):
    selected = target(profile)
    layout = selected['layout']
    with Path(path).open('rb') as stream:
        stream.seek(0x120)
        native = stream.read(5) == b'AGSYS'
    if selected['vendor'] and not native:
        return inspect_sdgoods(path, selected['maximum'])
    value = inspect_image(path, selected['board'], layout['version'], chip=selected['chip'])
    value['image_format'] = 'agent-link'
    maximum = layout['partitions']['factory']['size'] if value['app_id']=='agent.launcher' else selected['maximum']
    if value["size"] > maximum:
        raise ValueError(f"Application image overlaps the reserved {layout['cover_reserve'] // 1024} KiB slot cover")
    return value
