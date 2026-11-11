"""Image identity and boot-selection encoding; usable without ESP-IDF."""
import binascii
import hashlib
from pathlib import Path
import struct

DESCRIPTOR = struct.Struct("<8sII32s64s48s16s")
DESCRIPTOR_OFFSET = 0x120
# ESP-IDF/esptool image header IDs for the chips with source profiles here.
CHIP_IDS = {"esp32c3": 5, "esp32s3": 9, "esp32s31": 32}


def string(raw):
    if b"\0" not in raw:
        raise ValueError("Unterminated image metadata")
    return raw.split(b"\0", 1)[0].decode("utf-8", errors="strict")


def descriptor(data):
    if len(data) < DESCRIPTOR_OFFSET + DESCRIPTOR.size or data[0] != 0xE9:
        raise ValueError("Not an application image")
    magic, schema, layout, app_id, name, board, sdk = DESCRIPTOR.unpack_from(data, DESCRIPTOR_OFFSET)
    if magic != b"AGSYS001" or schema != 1:
        raise ValueError("Missing Agent System image identity")
    return dict(layout=layout, app_id=string(app_id), name=string(name), board=string(board), sdk=string(sdk),
                chip_id=struct.unpack_from("<H", data, 12)[0], version=string(data[0x30:0x50]))


def image_info(path, model, layout=1, app_max_bytes=None, chip="esp32s3"):
    return image_info_data(Path(path).read_bytes(), model, layout, app_max_bytes, chip)


def image_info_data(data, model, layout=1, app_max_bytes=None, chip="esp32s3"):
    info = descriptor(data)
    if info["board"] != model or info["layout"] != layout or info["sdk"] != "0.1.0" or info["chip_id"] != CHIP_IDS.get(chip):
        raise ValueError("Incompatible board, chip, layout or system SDK")
    if app_max_bytes is not None and len(data) > app_max_bytes:
        raise ValueError("Application image overlaps reserved slot data")
    return dict(info, size=len(data), sha256=hashlib.sha256(data).hexdigest())


def boot_data(slot=None):
    """IDF ota_select_entry_t: new image, seq CRC, redundant erased second sector.

    An erased record selects factory (or the first OTA app on a single-app
    layout). A USB-installed OTA image starts NEW, so normal boot confirmation
    and rollback remain available when enabled in the board's bootloader.
    """
    data = bytearray(b"\xff" * 0x2000)
    if slot is not None:
        if not 0 <= slot < 16:
            raise ValueError("Invalid OTA slot")
        seq = struct.pack("<I", slot + 1)
        data[:4] = seq
        struct.pack_into("<I", data, 24, 0)  # ESP_OTA_IMG_NEW
        struct.pack_into("<I", data, 28, binascii.crc32(seq, 0xFFFFFFFF) & 0xFFFFFFFF)
    return bytes(data)


def partition_records(data):
    """Compare real IDF partition entries, excluding padding/MD5 trailer."""
    records = []
    for offset in range(0, min(len(data), 0xC00), 32):
        row = data[offset:offset + 32]
        magic = struct.unpack_from("<H", row)[0]
        if magic in (0xFFFF, 0xEBEB):
            break
        if magic != 0x50AA:
            raise ValueError("Invalid device partition table")
        records.append(row)
    if not records:
        raise ValueError("No device partition table")
    return records
