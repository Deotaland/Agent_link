#!/usr/bin/env python3
"""Download every currently published firmware from https://sdgoods.ai/market.html.

Python 3.10+, standard library only. Run from the repository root:
    python tools/download_sdgoods_firmware.py --list
    python tools/download_sdgoods_firmware.py
    python tools/download_sdgoods_firmware.py --output E:/firmwares/sdgoods

The list is paginated. POST /api/firmwares/{id}/download returns either a
single signed URL or a list of parts (possibly inlineBase64). All returned
parts are saved, together with their addresses and SHA-256 hashes. Identical
shared parts are copied locally after verification. Re-running skips files
that match the current size/hash. Interrupted files are retried
from the beginning. No serial ports are opened and nothing is flashed.
"""

from __future__ import annotations

import argparse
import base64
import hashlib
import http.client
import json
import re
import sys
import time
import urllib.error
import urllib.parse
import urllib.request
from datetime import datetime, timezone
from pathlib import Path


ORIGIN = "https://sdgoods.ai"
DEFAULT_OUTPUT = Path(__file__).resolve().parents[1] / "dist" / "sdgoods-firmware"
CHUNK_SIZE = 1024 * 1024
NETWORK_ERRORS = (urllib.error.URLError, TimeoutError, ConnectionError, http.client.HTTPException)
RETRYABLE_ERRORS = NETWORK_ERRORS + (OSError, ValueError)


def log(message: str) -> None:
    print(message, flush=True)


def error_text(exc: Exception) -> str:
    # Signed download URLs must not leak through exception messages.
    if isinstance(exc, urllib.error.HTTPError):
        return f"HTTP {exc.code} {exc.reason}"
    message = str(exc)
    return re.sub(r"https?://[^\s'\"<>]+", "<URL>", message)


def safe_name(value: object, fallback: str = "firmware", limit: int = 90) -> str:
    name = re.sub(r'[<>:"/\\|?*\x00-\x1f\x7f]', "_", str(value or ""))
    name = name.strip(" .")[:limit].rstrip(" .") or fallback
    if name.split(".")[0].upper() in {
        "CON", "PRN", "AUX", "NUL",
        *(f"COM{i}" for i in range(1, 10)), *(f"LPT{i}" for i in range(1, 10)),
    }:
        name = "_" + name
    return name


def without_temporary_links(value: object) -> object:
    """Store useful metadata, without expiring URL signatures or inline blobs."""
    if isinstance(value, dict):
        return {
            k: without_temporary_links(v)
            for k, v in value.items() if k != "inlineBase64"
        }
    if isinstance(value, list):
        return [without_temporary_links(v) for v in value]
    if isinstance(value, str) and value.startswith(("https://", "http://")):
        u = urllib.parse.urlsplit(value)
        return urllib.parse.urlunsplit((u.scheme, u.netloc, u.path, "", ""))
    return value


def save_json(path: Path, value: object) -> None:
    temporary = path.with_name(path.name + ".tmp")
    temporary.write_text(json.dumps(value, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(path)


class Client:
    def __init__(self, timeout: float, attempts: int):
        self.timeout = timeout
        self.attempts = attempts
        self.opener = urllib.request.build_opener()
        self.verified_files: dict[str, Path] = {}

    def json(self, path: str, *, post: bool = False) -> dict:
        headers = {
            "User-Agent": "AgentLink-SDGoods-Downloader/1.0",
            "Accept": "application/json",
            "Origin": ORIGIN,
            "Referer": ORIGIN + "/market.html",
        }
        if post:
            headers["Content-Type"] = "application/json"
        request = urllib.request.Request(
            ORIGIN + path, data=b"{}" if post else None,
            headers=headers, method="POST" if post else "GET",
        )
        with self.opener.open(request, timeout=self.timeout) as response:
            data = json.load(response)
        if not isinstance(data, dict):
            raise ValueError("API response is not a JSON object")
        return data

    def catalog(self) -> list[dict]:
        items: dict[str, dict] = {}
        page = 1
        while True:
            query = urllib.parse.urlencode({"scope": "market", "pageSize": 60, "page": page})
            for attempt in range(self.attempts):
                try:
                    data = self.json("/api/firmwares?" + query)
                    break
                except RETRYABLE_ERRORS as exc:
                    if attempt + 1 == self.attempts:
                        raise
                    log(f"  Retry catalog page {page}: {error_text(exc)}")
                    time.sleep(min(2 ** attempt, 8))
            batch = data.get("items")
            total = data.get("total")
            if not isinstance(batch, list) or not isinstance(total, int) or total < 0:
                raise ValueError("API list must contain items[] and total")
            before = len(items)
            for item in batch:
                if not isinstance(item, dict) or not isinstance(item.get("id"), str) or not item["id"]:
                    raise ValueError("Invalid firmware catalog entry")
                items[item["id"]] = item
            if len(items) >= total:
                return list(items.values())
            if len(items) == before:
                raise ValueError(f"Pagination stopped at {len(items)} of {total} firmwares")
            page += 1


def normalize_parts(manifest: dict, firmware: dict) -> list[dict]:
    parts = manifest.get("parts")
    if not parts and manifest.get("url"):
        parts = [{
            "fileName": firmware.get("fileName") or "firmware.bin",
            "url": manifest["url"], "sha256": manifest.get("sha256"),
            "sizeBytes": manifest.get("sizeBytes"), "address": None,
        }]
    if not isinstance(parts, list) or not parts:
        raise ValueError("Download API returned no files")
    result = []
    names: set[str] = set()
    for i, source in enumerate(parts):
        if not isinstance(source, dict):
            raise ValueError("Invalid download part")
        part = dict(source)
        if part.get("inlineBase64") is None and not part.get("url"):
            raise ValueError("Download part has neither URL nor inlineBase64")
        if part.get("url"):
            url = urllib.parse.urljoin(ORIGIN + "/", part["url"])
            if urllib.parse.urlsplit(url).scheme not in ("http", "https"):
                raise ValueError("Download URL must use HTTP(S)")
            part["url"] = url
        address = part.get("address")
        if isinstance(address, str):
            address = int(address, 16 if address.lower().startswith("0x") else 10)
        if address is not None and (not isinstance(address, int) or address < 0):
            raise ValueError("Invalid part address")
        part["address"] = address
        size = part.get("sizeBytes")
        if size is not None and (not isinstance(size, int) or size < 0):
            raise ValueError("Invalid part size")
        digest = part.get("sha256")
        if digest and not re.fullmatch(r"[0-9a-fA-F]{64}", str(digest)):
            raise ValueError("Invalid SHA-256 in download manifest")
        filename = safe_name(part.get("fileName"), "firmware.bin")
        if address is not None:
            filename = f"0x{address:08x}_{filename}"
        if filename.casefold() in names:
            filename = f"{i:02d}_{filename}"
        names.add(filename.casefold())
        part["localFile"] = filename
        result.append(part)
    return result


def file_hash(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for chunk in iter(lambda: stream.read(CHUNK_SIZE), b""):
            digest.update(chunk)
    return digest.hexdigest()


def valid_existing(path: Path, part: dict) -> bool:
    # Without a server hash, download again rather than trusting size alone.
    if not path.is_file() or not part.get("sha256"):
        return False
    if part.get("sizeBytes") and path.stat().st_size != part["sizeBytes"]:
        return False
    return file_hash(path).lower() == part["sha256"].lower()


def download_part(client: Client, directory: Path, part: dict) -> dict:
    target = directory / part["localFile"]
    if valid_existing(target, part):
        client.verified_files[part["sha256"].lower()] = target
        log(f"  Skip verified: {target.name}")
        return {"localFile": target.name, "sizeBytes": target.stat().st_size,
                "sha256": part["sha256"].lower(), "address": part["address"]}
    temporary = target.with_name(target.name + ".part")
    digest = hashlib.sha256()
    size = 0
    try:
        with temporary.open("wb") as output:
            def write(chunk: bytes) -> None:
                nonlocal size
                output.write(chunk)
                digest.update(chunk)
                size += len(chunk)

            cached = client.verified_files.get((part.get("sha256") or "").lower())
            if cached and valid_existing(cached, part):
                with cached.open("rb") as source:
                    for chunk in iter(lambda: source.read(CHUNK_SIZE), b""):
                        write(chunk)
                log(f"  Reuse identical: {target.name}")
            elif part.get("inlineBase64") is not None:
                write(base64.b64decode(part["inlineBase64"], validate=True))
            else:
                # Only the public download URL is sent to the file host.
                request = urllib.request.Request(part["url"], headers={
                    "User-Agent": "AgentLink-SDGoods-Downloader/1.0",
                    "Accept-Encoding": "identity",
                })
                with client.opener.open(request, timeout=client.timeout) as response:
                    for chunk in iter(lambda: response.read(CHUNK_SIZE), b""):
                        write(chunk)
        if part.get("sizeBytes") and size != part["sizeBytes"]:
            raise ValueError(f"Size mismatch for {target.name}: {size} != {part['sizeBytes']}")
        actual_hash = digest.hexdigest()
        if part.get("sha256") and actual_hash.lower() != part["sha256"].lower():
            raise ValueError(f"SHA-256 mismatch for {target.name}")
        temporary.replace(target)
        client.verified_files[actual_hash] = target
    finally:
        temporary.unlink(missing_ok=True)
    log(f"  Saved: {target.name} ({size:,} bytes)")
    return {"localFile": target.name, "sizeBytes": size,
            "sha256": actual_hash, "address": part["address"]}


def download_firmware(client: Client, root: Path, firmware: dict, mode: str | None) -> dict:
    folder = "__".join((safe_name(firmware.get("name"), limit=55),
                        safe_name(firmware.get("version"), "unversioned", 25),
                        safe_name(firmware["id"], limit=40)))
    directory = root / folder
    directory.mkdir(parents=True, exist_ok=True)
    endpoint = "/api/firmwares/" + urllib.parse.quote(firmware["id"], safe="") + "/download"
    if mode:
        endpoint += "?" + urllib.parse.urlencode({"mode": mode})
    for attempt in range(client.attempts):
        try:
            # A retry obtains fresh signed links; successful parts are reused.
            manifest = client.json(endpoint, post=True)
            parts = normalize_parts(manifest, firmware)
            files = [download_part(client, directory, part) for part in parts]
            save_json(directory / "manifest.json", {
                "firmware": without_temporary_links(firmware),
                "download": without_temporary_links(manifest),
                "files": files, "requestedMode": mode or "server-default",
                "downloadedAt": datetime.now(timezone.utc).isoformat(),
            })
            return {"id": firmware["id"], "name": firmware.get("name"),
                    "version": firmware.get("version"), "directory": folder,
                    "fileCount": len(files), "sizeBytes": sum(f["sizeBytes"] for f in files)}
        except RETRYABLE_ERRORS as exc:
            if isinstance(exc, urllib.error.HTTPError) and exc.code in (401, 404):
                raise
            if attempt + 1 == client.attempts:
                raise
            log(f"  Retry {attempt + 2}/{client.attempts}: {error_text(exc)}")
            time.sleep(min(2 ** attempt, 8))
    raise AssertionError("unreachable")


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--output", type=Path, default=DEFAULT_OUTPUT, help="Destination directory")
    parser.add_argument("--list", action="store_true", help="List all published firmwares without downloading")
    parser.add_argument("--id", action="append", dest="ids", help="Only download this firmware ID (repeatable)")
    parser.add_argument("--mode", choices=("single", "multi"), help="App package mode; default: server default")
    parser.add_argument("--attempts", type=int, default=3, help="Maximum attempts per firmware/page (default: 3)")
    parser.add_argument("--timeout", type=float, default=45, help="Network inactivity timeout, seconds (default: 45)")
    args = parser.parse_args(argv)
    if args.attempts < 1 or not 0 < args.timeout < float("inf"):
        parser.error("--attempts and --timeout must be positive")
    client = Client(args.timeout, args.attempts)
    try:
        firmwares = client.catalog()
        log(f"Market: {len(firmwares)} firmwares")
        if args.ids:
            missing = set(args.ids) - {f["id"] for f in firmwares}
            if missing:
                raise ValueError("IDs not in market: " + ", ".join(sorted(missing)))
            firmwares = [f for f in firmwares if f["id"] in args.ids]
        if args.list:
            for firmware in firmwares:
                log(f"{firmware['id']}  {firmware.get('name')}  {firmware.get('version')}  "
                    f"{firmware.get('sizeBytes', '?')} bytes  {firmware.get('packageForm', '?')}")
            return 0
        root = args.output.expanduser().resolve()
        root.mkdir(parents=True, exist_ok=True)
        log(f"Output: {root}")
        save_json(root / "catalog.json", without_temporary_links(firmwares))
        report: dict = {"source": ORIGIN + "/market.html", "completed": [], "failed": []}
        for index, firmware in enumerate(firmwares, 1):
            log(f"[{index}/{len(firmwares)}] {firmware.get('name')} {firmware.get('version')}")
            try:
                report["completed"].append(download_firmware(client, root, firmware, args.mode))
            except RETRYABLE_ERRORS as exc:
                message = error_text(exc)
                log(f"  FAILED: {message}")
                report["failed"].append({"id": firmware["id"], "name": firmware.get("name"), "error": message})
            save_json(root / "download-report.json", report)
        # Also produce a report when the market is empty.
        save_json(root / "download-report.json", report)
        log(f"Done: {len(report['completed'])} succeeded, {len(report['failed'])} failed")
        return 1 if report["failed"] else 0
    except RETRYABLE_ERRORS as exc:
        log("ERROR: " + error_text(exc))
        return 1


if __name__ == "__main__":
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(errors="backslashreplace")
    try:
        raise SystemExit(main())
    except KeyboardInterrupt:
        log("Interrupted. Re-run the same command to reuse verified files.")
        raise SystemExit(130)
