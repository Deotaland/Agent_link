#!/usr/bin/env python3
"""Find the Agent Link board type for a board, from whatever the user knows about it: model,
product name, company, shop or link, the name it shows over Bluetooth, or what it looks like.

usage: python find_board.py "<what the user said>"
       python find_board.py --list
       python find_board.py --json "<what the user said>"
"""
import argparse
import json
import re
import sys
import unicodedata
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
import catalog  # noqa: E402

CJK = r"㐀-鿿"


def norm(s):
    s = unicodedata.normalize("NFKC", str(s)).lower()
    return re.sub(r"[\s\-_./:;,()\[\]（）·，、。]+", "", s)


def tokens(query):
    """ASCII words and CJK runs of the query, normalized; single characters are dropped."""
    text = unicodedata.normalize("NFKC", query).lower()
    return [t for t in re.findall(rf"[a-z0-9]+|[{CJK}]+", text) if len(t) >= 2]


def url_pieces(url):
    """Host without www and any long digit runs (shop item ids)."""
    host = re.sub(r"^https?://(www\.)?", "", url).split("/")[0]
    return [norm(host)] + re.findall(r"\d{6,}", url)


def fields(e):
    zh = e.get("zh") or {}
    strong = [e.get("model"), e.get("product"), zh.get("product"), e.get("fw_model"), *e.get("aliases", [])]
    if e.get("ble_name"):
        strong.append(re.sub(r"X{2,}.*$", "", e["ble_name"]))
    medium = [e.get("vendor"), zh.get("vendor")]
    urls = [*e.get("buy", []), *e.get("docs", [])]
    weak = [e.get(k) for k in ("hardware", "identify", "use", "notes")]
    weak += [zh.get(k) for k in ("hardware", "identify", "use", "notes")]
    clean = lambda xs: sorted({norm(x) for x in xs if x})  # noqa: E731
    return clean(strong), clean(medium), urls, clean(weak)


def longest(matches):
    """Drop matches contained in a longer one, so "rorolee" doesn't count again inside "rorolee.com"."""
    return [m for m in matches if not any(m != o and m in o for o in matches)]


def score(e, query):
    q = norm(query)
    strong, medium, urls, weak = fields(e)
    s = 0
    if e.get("model") and norm(e["model"]) == q:
        s += 100
    s += sum(30 + min(len(v), 20) for v in longest([v for v in strong if len(v) >= 2 and v in q]))
    s += sum(20 + min(len(v), 10) for v in longest([v for v in medium if len(v) >= 3 and v in q]))
    s += 25 * len({p for u in urls for p in url_pieces(u) if len(p) >= 4 and p in q})
    for t in tokens(query):
        if any(t in v for v in strong):
            s += 8
        elif any(t in v for v in medium) or any(t in norm(u) for u in urls):
            s += 5
        elif any(t in v for v in weak):
            s += 2
        elif re.fullmatch(rf"[{CJK}]+", t):
            s += sum(1 for i in range(len(t) - 1) if any(t[i:i + 2] in v for v in weak))
    return s


def group_key(e):
    return e["model"] or e["board_type"]


def describe(entries):
    first = entries[0]
    lines = [f"{first['model'] or '(no model)'} · {first['product']} · {first['vendor'] or 'no vendor'}"]
    lines.append(f"  hardware : {first['hardware']}")
    lines.append(f"  looks    : {first['identify']}")
    for k, label in (("buy", "buy"), ("docs", "docs")):
        if first[k]:
            lines.append(f"  {label:<9}: {' '.join(first[k])}")
    lines.append(f"  flash via: {first['flash_port']}")
    if len(entries) > 1:
        lines.append("  Several firmware builds fit this hardware. Ask the user what the device should do:")
    for e in entries:
        lines.append(f"  - {e['board_type']} [{e['status']}] {e['use']}")
        lines.append(f"      firmware/boards/{e['dir']}, target {e['target']}, link {e['transport']},"
                     f" boot log model = {e['fw_model']}")
        lines.append(f"      sdkconfig: {'  '.join(e['sdkconfig'])}")
        if e.get("ble_name"):
            lines.append(f"      shows up as: {e['ble_name']}")
        if e.get("notes"):
            lines.append(f"      note: {e['notes']}")
        if e["status"] == "not-adapted":
            lines.append("      NOT ADAPTED: don't flash this for a user.")
        elif e["status"] == "example":
            lines.append("      Wiring example: only for a board the user wired themselves; check the pins in config.h.")
    return "\n".join(lines)


def main(argv=None):
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("query", nargs="*", help="model, product, company, shop/link, Bluetooth name or description")
    ap.add_argument("--list", action="store_true", help="list every board type")
    ap.add_argument("--json", action="store_true", help="print JSON")
    args = ap.parse_args(argv)
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")

    entries, source, problems = catalog.load()
    for p in problems:
        print(f"warning: {p} (using {source})", file=sys.stderr)

    if args.list or not args.query:
        if args.json:
            print(json.dumps(entries, ensure_ascii=False, indent=2))
            return 0
        late = ("example", "not-adapted")
        for e in sorted(entries, key=lambda e: (e["status"] in late, e["model"] or "", e["board_type"])):
            print(f"{(e['model'] or '-'):<28} {e['board_type']:<22} {e['transport']:<5} {e['status']:<12} {e['product']}")
        print(f"\n({len(entries)} board types from {source})")
        return 0

    query = " ".join(args.query)
    groups = {}
    for e in entries:
        s = score(e, query)
        if s > 0:
            g = groups.setdefault(group_key(e), {"score": 0, "entries": []})
            g["score"] = max(g["score"], s)
            g["entries"].append(e)
    rank = {s: i for i, s in enumerate(catalog.STATUSES)}
    ranked = sorted(groups.items(), key=lambda kv: (-kv[1]["score"],
                                                    min(rank[e["status"]] for e in kv[1]["entries"])))
    if ranked:
        floor = max(20, ranked[0][1]["score"] * 0.3)
        ranked = [ranked[0]] + [kv for kv in ranked[1:3] if kv[1]["score"] >= floor]

    if args.json:
        print(json.dumps([{"score": g["score"], "boards": g["entries"]} for _, g in ranked],
                         ensure_ascii=False, indent=2))
        return 0 if ranked else 1
    if not ranked:
        models = sorted({e["model"] for e in entries if e["model"]})
        print("No match. Ask the user for the model, the shop, the company, a photo or the text printed "
              "on the board.\nKnown models: " + ", ".join(models))
        return 1
    best = ranked[0][1]["score"]
    if best < 25:
        print("Weak match only: confirm with the user (model, shop, photo) before flashing anything.\n")
    for i, (_, g) in enumerate(ranked):
        print(("Best match" if i == 0 else "Also possible") + f" (score {g['score']}):")
        print(describe(g["entries"]))
        print()
    print(f"(catalog: {source})")
    return 0


if __name__ == "__main__":
    sys.exit(main())
