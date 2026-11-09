"""Resolve board, application and layout manifests into one build/flash contract.

This module has no ESP-IDF dependency. Both CMake and host tools consume its
resolved data; source manifests never contain duplicate flash offsets.
"""
import csv
import json
from pathlib import Path
import re

ROOT = Path(__file__).resolve().parents[1]


def read_json(path):
    return json.loads(Path(path).read_text(encoding="utf-8"))


def contained(root, name):
    path = (root / name).resolve()
    if not path.is_relative_to(root.resolve()):
        raise ValueError(f"Path escapes firmware directory: {name}")
    return path


def number(value):
    value = str(value).strip()
    units = {"k": 1024, "m": 1024 * 1024}
    return int(value[:-1], 0) * units[value[-1].lower()] if value[-1:].lower() in units else int(value, 0)


def partitions(path, table_offset=0x8000):
    """Parse CSV geometry, including IDF's blank-offset alignment convention."""
    result, end = {}, table_offset + 0x1000
    for fields in csv.reader(line.split("#", 1)[0] for line in Path(path).read_text(encoding="utf-8").splitlines()):
        if not fields or not fields[0].strip():
            continue
        fields = [field.strip() for field in fields]
        if len(fields) < 5:
            raise ValueError(f"Invalid partition row in {path}")
        label, kind, subtype, offset, size = fields[:5]
        alignment = 0x10000 if kind in {"app", "0", "0x00"} else 0x1000
        start = number(offset) if offset else (end + alignment - 1) // alignment * alignment
        length = number(size)
        if label in result or start < end or start % alignment or length <= 0 or length % 0x1000:
            raise ValueError(f"Overlapping, duplicate or unaligned partition: {label}")
        result[label] = dict(type=kind, subtype=subtype, offset=start, size=length)
        end = start + length
    return result


def layout_info(layout_id, root=ROOT):
    layouts = read_json(root / "partitions/layouts.json")
    if layout_id not in layouts:
        raise ValueError(f"Unknown partition layout: {layout_id}")
    layout = dict(layouts[layout_id], id=layout_id)
    if layout.get("mode") not in {"single", "managed"} or layout.get("version", 0) < 1:
        raise ValueError(f"Invalid layout: {layout_id}")
    layout["partitions"] = partitions(contained(root, layout["csv"]))
    entries = layout["partitions"]
    slots = layout.get("app_slots", [])
    if not slots or slots != [f"ota_{i}" for i in range(len(slots))] or len(slots) > 16:
        raise ValueError("OTA slots must be contiguous and start at ota_0")
    for slot in slots:
        if entries.get(slot, {}).get("type") != "app" or entries[slot]["subtype"] != slot:
            raise ValueError(f"Missing OTA app partition: {slot}")
    if entries.get("otadata", {}).get("subtype") != "ota" or entries["otadata"]["size"] != 0x2000:
        raise ValueError("Layout needs a two-sector otadata partition")
    if layout["mode"] == "managed":
        if entries.get("factory", {}).get("subtype") != "factory":
            raise ValueError("Managed layout needs a factory partition")
        reserve = layout.get("cover_reserve", 0)
        if not isinstance(reserve, int) or reserve % 0x1000 or any(entries[s]["size"] <= reserve for s in slots):
            raise ValueError("Invalid app-slot reserve")
    return layout


def resolve_profiles(root=ROOT):
    """Keep public profile names; chip/hardware/app details come from their owners."""
    result = {}
    for name, selection in read_json(root / "profiles/profiles.json").items():
        if set(selection) != {"board", "board_type", "app", "backend", "defaults"}:
            raise ValueError(f"{name}: profiles select board/app/backend/defaults; hardware belongs to the board")
        profile = dict(selection, name=name)
        catalog = read_json(contained(root, f"boards/{profile['board']}/config.json"))
        entry = next((b for b in catalog["boards"] if b["board_type"] == profile["board_type"]), None)
        if not entry or "build" not in entry:
            raise ValueError(f"{name}: board has no build definition")
        hardware = entry["build"]
        app = read_json(contained(root, f"apps/{profile['app']}/manifest.json"))
        if app.get("schema") != 1 or app.get("backend") != profile["backend"]:
            raise ValueError(f"{name}: incompatible application backend")
        layout = layout_info(hardware["layout"], root)
        if bool(hardware.get("factory")) != (layout["mode"] == "managed"):
            raise ValueError(f"{name}: managed layouts and board-bound factories must be selected together")
        if catalog["target"] not in layout["targets"]:
            raise ValueError(f"{name}: layout does not support board chip")
        profile.update(hardware)
        profile.update(target=catalog["target"], ui=app["ui"], app_shared=app.get("shared_app", ""),
                       ui_component=app.get("ui_component", ""), application=app,
                       model=entry["fw_model"], layout_info=layout, app_requires=app.get("requires", []))
        profile["defaults"] = [*selection["defaults"][:-1], hardware["defaults"],
                               layout["defaults"], selection["defaults"][-1]]
        result[name] = profile
    return result


def system_images(profile, root=ROOT):
    """Compose the board's launcher and selected app without a factory-owned app list."""
    factory = profile.get("factory")
    if not factory:
        return None
    if factory != profile["board"]:
        raise ValueError("A factory must belong to its selected board")
    manifest = read_json(contained(root, f"factory/{factory}/manifest.json"))
    if manifest.get("schema") != 2 or manifest.get("board") != profile["board"]:
        raise ValueError("Invalid board factory identity")
    launcher = dict(manifest["image"], path=f"factory/{factory}")
    app = profile["application"]
    role = app.get("role", "standard")
    if role == "factory" or launcher["app_id"] != "agent.launcher" or app["app_id"] == "agent.launcher":
        raise ValueError("Factory cannot be an installable application")
    layout = profile["layout_info"]
    images = {"factory": dict(launcher, partition="factory"),
              role: dict(app, path=f"apps/{profile['app']}", partition=layout["app_slots"][0])}
    for key, image in images.items():
        directory = contained(root, image["path"])
        if (not re.fullmatch(r"[a-zA-Z0-9._-]{1,31}", image["app_id"]) or
                not 0 < len(image["name"].encode()) < 64 or not any(directory.glob("*.cc"))):
            raise ValueError(f"Invalid image: {key}")
        for field in ("sources", "include_dirs"):
            for relative in image[field]:
                if not contained(directory, relative).exists():
                    raise ValueError(f"Missing image {field}: {relative}")
        image["offset"] = hex(layout["partitions"][image["partition"]]["offset"])
    return dict(schema=2, board=profile["board"], model=profile["model"], layout=layout["version"], images=images)


def flash_plan(profile, role):
    """Offline destinations shared by IDF flash, app-flash and USB packaging."""
    layout = profile["layout_info"]
    managed = layout["mode"] == "managed"
    image = system_images(profile)["images"][role] if managed else profile["application"]
    partition = "factory" if role == "factory" else layout["app_slots"][0]
    return dict(schema=1, profile=profile["name"], board=profile["board"], model=profile["model"],
                chip=profile["target"], layout=layout["id"], layout_version=layout["version"],
                mode=layout["mode"], role=role, app_id=image["app_id"], partition=partition,
                app_offset=layout["partitions"][partition]["offset"],
                app_max_bytes=layout["partitions"][partition]["size"] -
                    (layout.get("cover_reserve", 0) if role != "factory" else 0),
                otadata_offset=layout["partitions"]["otadata"]["offset"],
                partition_table_offset=0x8000, partitions=layout["partitions"],
                app_slots=layout["app_slots"], cover_reserve=layout.get("cover_reserve", 0),
                flash_assets=profile.get("flash_assets", []))
