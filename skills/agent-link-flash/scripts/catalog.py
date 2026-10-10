"""Board catalog: one entry per board type, from firmware/boards/*/config.json.

Used by find_board.py next to this file and by tools/board_catalog.py in the repository. Outside a
checkout (a skill installed on its own) the copy in ../references/boards.json is used instead.
"""
import json
from pathlib import Path

TRANSPORTS = ("BLE", "WIFI", "MUSE", "NONE")
STATUSES = ("supported", "legacy", "example", "not-adapted")
REQUIRED = ("board_type", "model", "product", "vendor", "buy", "docs", "aliases", "hardware",
            "identify", "flash_port", "use", "transport", "ble_name", "fw_model", "status",
            "notes", "zh")
ZH_REQUIRED = ("product", "hardware", "identify", "flash_port", "use")
BUNDLED = Path(__file__).resolve().parent.parent / "references" / "boards.json"


def find_boards_dir(start):
    """firmware/boards of the checkout containing `start`, or None."""
    start = Path(start).resolve()
    for d in (start, *start.parents):
        boards = d / "firmware" / "boards"
        if boards.is_dir() and any(boards.glob("*/config.json")):
            return boards
    return None


def _check_entry(b, where):
    problems = [f"{where}: missing {k}" for k in REQUIRED if k not in b]
    if problems:
        return problems
    if b["transport"] not in TRANSPORTS:
        problems.append(f"{where}: transport must be one of {', '.join(TRANSPORTS)}")
    if b["status"] not in STATUSES:
        problems.append(f"{where}: status must be one of {', '.join(STATUSES)}")
    if not b["model"] and b["status"] != "example":
        problems.append(f"{where}: model is required unless status is example")
    for k in ("buy", "docs", "aliases"):
        if not isinstance(b[k], list):
            problems.append(f"{where}: {k} must be a list")
    if not isinstance(b["zh"], dict):
        problems.append(f"{where}: zh must be an object")
    else:
        problems += [f"{where}: zh.{k} missing" for k in ZH_REQUIRED if not b["zh"].get(k)]
    return problems


def load_config_dir(boards_dir):
    """(entries, problems) read from every boards_dir/*/config.json."""
    entries, problems = [], []
    for cfg in sorted(Path(boards_dir).glob("*/config.json")):
        name = cfg.parent.name
        try:
            data = json.loads(cfg.read_text(encoding="utf-8"))
        except (OSError, ValueError) as e:
            problems.append(f"{name}/config.json: {e}")
            continue
        if data.get("type") != name:
            problems.append(f"{name}/config.json: type is {data.get('type')!r}, expected {name!r}")
        if not isinstance(data.get("target"), str) or not data["target"]:
            problems.append(f"{name}/config.json: target missing")
        boards = data.get("boards")
        if not isinstance(boards, list) or not boards:
            problems.append(f"{name}/config.json: needs a non-empty \"boards\" list")
            continue
        for i, b in enumerate(boards):
            where = f"{name}/config.json boards[{i}]"
            found = _check_entry(b, where) if isinstance(b, dict) else [f"{where}: not an object"]
            if found:
                problems += found
                continue
            sdkconfig = [f"CONFIG_BOARD_TYPE_{b['board_type']}=y"]
            if b["transport"] != "NONE":
                sdkconfig.append(f"CONFIG_AGENT_LINK_TRANSPORT_{b['transport']}=y")
            entries.append({"board_type": b["board_type"], "dir": name, "target": data.get("target"),
                            **{k: v for k, v in b.items() if k != "board_type"},
                            "sdkconfig": sdkconfig})
    return entries, problems


def load(start=None):
    """(entries, source, problems): the checkout's config.json files when inside a checkout and
    they are valid, else the bundled copy (problems then says why the checkout was not used)."""
    problems = []
    boards = find_boards_dir(start or Path(__file__).parent)
    if boards:
        entries, problems = load_config_dir(boards)
        if not problems:
            return entries, str(boards), []
    data = json.loads(BUNDLED.read_text(encoding="utf-8"))
    return data["boards"], str(BUNDLED), problems
