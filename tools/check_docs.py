#!/usr/bin/env python3
"""Check Markdown links and English documentation/JSON metadata, excluding ignored files."""

import json
import re
import subprocess
import sys
from pathlib import Path
from urllib.parse import unquote, urlsplit

ROOT = Path(__file__).resolve().parents[1]
CJK = re.compile(r"[\u3400-\u9fff\uf900-\ufaff]")
LINK = re.compile(r"!?\[[^\]\n]*\]\((<[^>]+>|[^\s)]+)(?:\s+\"[^\"]*\")?\)")


def repository_files(root):
    result = subprocess.run(
        ["git", "ls-files", "--cached", "--others", "--exclude-standard", "-z"],
        cwd=root, check=True, capture_output=True,
    )
    return {name for name in result.stdout.decode("utf-8").split("\0")
            if name and (root / name).is_file()}


def prose_lines(text):
    fence = None
    for number, line in enumerate(text.splitlines(), 1):
        marker = re.match(r"^\s*(`{3,}|~{3,})", line)
        if marker:
            token = marker[1]
            if fence is None:
                fence = token
            elif token[0] == fence[0] and len(token) >= len(fence):
                fence = None
            continue
        if fence is None:
            yield number, line


def anchors(text):
    found, counts = set(), {}
    for _, line in prose_lines(text):
        heading = re.match(r"^#{1,6}\s+(.+?)(?:\s+#+)?$", line)
        if not heading:
            continue
        slug = re.sub(r"[^\w\- ]", "", heading[1].lower()).replace(" ", "-")
        count = counts.get(slug, 0)
        counts[slug] = count + 1
        found.add(f"{slug}-{count}" if count else slug)
    return found


def check_document(root, name, files):
    text = (root / name).read_text(encoding="utf-8")
    problems = []
    if name != "README.zh-CN.md":
        for number, line in enumerate(text.splitlines(), 1):
            if CJK.search(line):
                problems.append(f"{name}:{number}: use English; translations belong in README.zh-CN.md")
    for number, line in prose_lines(text):
        for match in LINK.finditer(line):
            link = match[1].strip("<>")
            parts = urlsplit(link)
            if parts.scheme or parts.netloc:
                continue
            target = (root / name).parent / unquote(parts.path) if parts.path else root / name
            if parts.path.startswith("/"):
                target = root / unquote(parts.path).lstrip("/")
            target = target.resolve()
            if not target.is_relative_to(root):
                problems.append(f"{name}:{number}: link leaves repository: {link}")
                continue
            relative = target.relative_to(root).as_posix()
            included = (relative in files or
                        (target.is_dir() and any(f.startswith(relative + "/") for f in files)))
            if not included:
                problems.append(f"{name}:{number}: missing or ignored link target: {link}")
            elif parts.fragment and target.suffix.lower() == ".md":
                if unquote(parts.fragment) not in anchors(target.read_text(encoding="utf-8")):
                    problems.append(f"{name}:{number}: missing heading: {link}")
    return problems


def check_metadata(root, name):
    try:
        data = json.loads((root / name).read_text(encoding="utf-8"))
    except (UnicodeError, ValueError) as error:
        return [f"{name}: invalid UTF-8 JSON: {error}"]
    if CJK.search(json.dumps(data, ensure_ascii=False)):
        return [f"{name}: JSON text and aliases must use English"]
    return []


def main():
    files = repository_files(ROOT)
    docs = sorted(name for name in files if name.lower().endswith((".md", ".mdx")))
    metadata = sorted(name for name in files if name.lower().endswith(".json"))
    problems = [problem for name in docs for problem in check_document(ROOT, name, files)]
    problems += [problem for name in metadata for problem in check_metadata(ROOT, name)]
    for problem in problems:
        print(problem, file=sys.stderr)
    if problems:
        return 1
    print(f"Documentation OK: {len(docs)} Markdown files and {len(metadata)} JSON files; "
          "English policy and local links checked.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
