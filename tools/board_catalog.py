#!/usr/bin/env python3
"""Check the board catalog (firmware/boards/*/config.json) and regenerate what is built from it:
skills/agent-link-flash/references/boards.json and the board tables in AGENTS.md and AGENTS.zh-CN.md.

usage: python tools/board_catalog.py           check, then regenerate
       python tools/board_catalog.py --check   check only: exit 1 if anything is wrong or out of date
"""
import argparse
import json
import re
import sys
from pathlib import Path
from urllib.parse import urlparse

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT / "skills" / "agent-link-flash" / "scripts"))
import catalog  # noqa: E402

BOARDS = ROOT / "firmware" / "boards"
KCONFIG = ROOT / "firmware" / "main" / "Kconfig.projbuild"
MAIN_CMAKE = ROOT / "firmware" / "main" / "CMakeLists.txt"
DOCS = {"en": ROOT / "AGENTS.md", "zh": ROOT / "AGENTS.zh-CN.md"}
BEGIN, END = "<!-- boards:begin -->", "<!-- boards:end -->"

CHIP = {"esp32s3": "ESP32-S3", "esp32p4": "ESP32-P4"}
LINK = {"BLE": "BLE", "WIFI": "WiFi", "MUSE": "Muse", "NONE": "—"}
STATUS = {
    "en": {"supported": "supported", "legacy": "old revision", "example": "wiring example",
           "not-adapted": "**not adapted, don't flash**"},
    "zh": {"supported": "支持", "legacy": "老版本", "example": "接线示例", "not-adapted": "**未适配，不要烧**"},
}
HEADER = {
    "en": ["Model", "Board", "Vendor", "Where to buy", "What this firmware does", "`BOARD_TYPE_…`", "Chip", "Link", "Status"],
    "zh": ["型号", "板子", "厂家", "购买", "这个固件做什么", "`BOARD_TYPE_…`", "芯片", "连接", "状态"],
}


def kconfig_board_types():
    found = re.findall(r"^\s*config BOARD_TYPE_(\w+)", KCONFIG.read_text(encoding="utf-8"), re.M)
    return list(dict.fromkeys(found))   # a symbol may be defined more than once


def cmake_board_dirs():
    """BOARD_TYPE suffix -> board directory, from the select chain in main/CMakeLists.txt."""
    text = MAIN_CMAKE.read_text(encoding="utf-8")
    out = {}
    for cond, d in re.findall(r"(?:else)?if\(([^)]*)\)\s*\n\s*set\(BOARD_DIR \"([^\"]+)\"\)", text):
        for t in re.findall(r"CONFIG_BOARD_TYPE_(\w+)", cond):
            out[t] = d
    return out


def repo_problems(entries):
    problems = []
    types = [e["board_type"] for e in entries]
    known = kconfig_board_types()
    problems += [f"BOARD_TYPE_{t} appears {types.count(t)} times" for t in sorted(set(types)) if types.count(t) > 1]
    problems += [f"BOARD_TYPE_{t} (main/Kconfig.projbuild) has no catalog entry" for t in known if t not in types]
    problems += [f"BOARD_TYPE_{t} is not in main/Kconfig.projbuild" for t in types if t not in known]
    dirs = cmake_board_dirs()
    for e in entries:
        t = e["board_type"]
        if dirs.get(t) != e["dir"]:
            problems.append(f"BOARD_TYPE_{t}: main/CMakeLists.txt builds {dirs.get(t)!r}, catalog says {e['dir']!r}")
        sources = "".join(p.read_text(encoding="utf-8", errors="replace")
                          for p in (BOARDS / e["dir"]).glob("*") if p.suffix in (".cc", ".cpp", ".c", ".h"))
        if f'"{e["fw_model"]}"' not in sources:
            problems.append(f"BOARD_TYPE_{t}: fw_model {e['fw_model']!r} is not a string in boards/{e['dir']} "
                            "(it must be what Model() or Name() returns)")
    by_model = {}
    for e in entries:
        if e["model"]:
            by_model.setdefault(e["model"], []).append(e)
    for model, es in by_model.items():
        for k in ("vendor", "buy", "docs"):
            if len({json.dumps(x[k], ensure_ascii=False) for x in es}) > 1:
                problems.append(f"model {model}: {k} differs between {', '.join(x['board_type'] for x in es)}")
    return problems


def ordered(entries):
    rank = {"supported": 0, "legacy": 1, "example": 2, "not-adapted": 3}

    def key(e):
        late = e["status"] in ("example", "not-adapted")
        vendor = e["vendor"] or ""
        return (late, vendor != "Deotaland", vendor.lower(), e["model"] or "", rank[e["status"]], e["board_type"])

    return sorted(entries, key=key)


def link_label(url, lang):
    host = urlparse(url).netloc
    host = host[4:] if host.startswith("www.") else host
    if host.endswith("taobao.com"):
        return "淘宝" if lang == "zh" else "Taobao"
    return host


def cell(s):
    return str(s).replace("|", "\\|").replace("\n", " ")


def render_table(entries, lang):
    lines = []
    for n, row in enumerate(table_rows(entries, lang)):
        lines.append("| " + " | ".join(row if n == 1 else [cell(c) for c in row]) + " |")
    return "\n".join(lines)


def table_rows(entries, lang):
    rows = [HEADER[lang], ["---"] * len(HEADER[lang])]
    for e in ordered(entries):
        zh = e["zh"] if lang == "zh" else {}
        if e["buy"]:
            buy = " · ".join(f"[{link_label(u, lang)}]({u})" for u in e["buy"])
        elif e["docs"]:
            buy = " · ".join(f"[{'资料' if lang == 'zh' else 'docs'}]({u})" for u in e["docs"])
        else:
            buy = "—"
        rows.append([
            e["model"] or "—",
            zh.get("product") or e["product"],
            zh.get("vendor") or e["vendor"] or "—",
            buy,
            zh.get("use") or e["use"],
            f"`{e['board_type']}`",
            CHIP.get(e["target"], e["target"]),
            LINK[e["transport"]],
            STATUS[lang][e["status"]],
        ])
    return rows


def expected_outputs(entries):
    out = {}
    bundled = {"source": "firmware/boards/*/config.json, generated by tools/board_catalog.py; do not edit",
               "boards": ordered(entries)}
    out[catalog.BUNDLED] = json.dumps(bundled, ensure_ascii=False, indent=2) + "\n"
    for lang, path in DOCS.items():
        text = path.read_text(encoding="utf-8")
        a, b = text.find(BEGIN), text.find(END)
        if a < 0 or b < a:
            raise SystemExit(f"{path.name}: markers {BEGIN} / {END} not found")
        out[path] = text[:a + len(BEGIN)] + "\n" + render_table(entries, lang) + "\n" + text[b:]
    return out


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", action="store_true", help="check only, change nothing")
    args = ap.parse_args(argv)

    entries, problems = catalog.load_config_dir(BOARDS)
    problems += repo_problems(entries)
    for p in problems:
        print(f"ERROR: {p}", file=sys.stderr)
    if problems:
        return 1

    stale = []
    for path, content in expected_outputs(entries).items():
        current = path.read_text(encoding="utf-8") if path.exists() else None
        if current == content:
            continue
        stale.append(path)
        if not args.check:
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text(content, encoding="utf-8", newline="\n")
    rel = [str(p.relative_to(ROOT)) for p in stale]
    if args.check:
        if rel:
            print("Out of date (run python tools/board_catalog.py): " + ", ".join(rel), file=sys.stderr)
            return 1
        print(f"Catalog OK: {len(entries)} board types, generated files up to date.")
        return 0
    print(f"Catalog OK: {len(entries)} board types. " + (f"Updated: {', '.join(rel)}" if rel else "Nothing to update."))
    return 0


if __name__ == "__main__":
    sys.exit(main())
