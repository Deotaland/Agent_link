#!/usr/bin/env python3
"""Local development app catalog and firmware downloads; no platform account required."""
import argparse
import hashlib
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import socket
import sys
import urllib.request
from urllib.parse import urlsplit

from paths import BASE_DIR, local_path
from multi_app_image import BOARD, image_info
from make_app_cover import convert_cover, validate_cover, FORMAT, BLOCK_BYTES


def prepare_cover(cover, app_id, cache, source_root=BASE_DIR):
    source = None
    if cover.get('source_file'):
        path = Path(source_root) / cover['source_file']
        if path.is_file(): source = path.read_bytes()
    if source is None and cover.get('source_url'):
        request = urllib.request.Request(cover['source_url'], headers={'User-Agent':'Agent-Link-Development/1.0'})
        try:
            with urllib.request.build_opener(urllib.request.ProxyHandler({})).open(request, timeout=20) as response:
                source = response.read(8 * 1024 * 1024 + 1)
        except Exception as error:
            raise ValueError('Cover source fetch failed: ' + type(error).__name__) from None
    if source is None:
        raise ValueError('Configure a source_file or source_url for backend cover conversion')
    if len(source)>8*1024*1024 or ('source_size' in cover and len(source)!=cover['source_size']) or \
            ('source_sha256' in cover and hashlib.sha256(source).hexdigest()!=cover['source_sha256']):
        raise ValueError('Cover source size/hash mismatch')
    data = convert_cover(source,app_id)
    info = validate_cover(data,app_id)
    directory = Path(cache); directory.mkdir(parents=True, exist_ok=True)
    path = directory/(app_id+'.'+info['sha256']+'.raw');path.write_bytes(data)
    return data, info


def load_apps(bundle, covers=None, source_root=BASE_DIR, cover_cache=None):
    bundle = local_path(bundle)
    if not bundle.is_dir():
        raise ValueError(f"Firmware directory not found: {bundle}. Create it and copy application .bin files into it.")
    cover_cache = Path(cover_cache) if cover_cache is not None else BASE_DIR / '.cache/covers'
    apps = []
    for entry in sorted(bundle.iterdir(), key=lambda item: item.name.lower()):
        if not entry.is_file() or entry.suffix.lower() != '.bin':
            continue
        path = entry.resolve()
        if path.parent != bundle:
            raise ValueError("App files must be directly inside the bundle")
        try:
            info = image_info(path)
        except ValueError as error:
            print(f"Skipped {path.name}: {error}", file=sys.stderr)
            continue
        if info["app_id"] == "agent.launcher":
            print(f"Skipped {path.name}: factory is not an installable application", file=sys.stderr)
            continue
        if any(app["app_id"] == info["app_id"] for app in apps):
            raise ValueError(f"Duplicate app ID: {info['app_id']}. Keep one .bin per app in {bundle}.")
        app=dict(info,path=path)
        cover=(covers or {}).get(info['app_id'])
        if cover:
            if cover.get('source_file') or cover.get('source_url'):
                data, meta = prepare_cover(cover,info['app_id'],cover_cache,source_root)
                cover=dict(url='/covers/'+info['app_id']+'/'+meta['sha256']+'.raw',**meta)
                app['cover_data']=data
            address=cover.get('url','')
            valid_url=isinstance(address,str) and (address.startswith(('https://','http://')) or ('cover_data' in app and address.startswith('/covers/')))
            if not valid_url or \
                not isinstance(cover.get('size'),int) or cover['size']!=BLOCK_BYTES or cover.get('format')!=FORMAT or \
                len(cover.get('sha256',''))!=64 or any(c not in '0123456789abcdef' for c in cover['sha256']):
                raise ValueError('Invalid cover URL, format, size or SHA-256')
            app.update(cover_url=cover['url'],cover_size=cover['size'],cover_sha256=cover['sha256'],cover_format=cover['format'])
        apps.append(app)
    return apps


def make_handler(apps):
    allowed = {"/firmware/" + app["app_id"] + ".bin": app for app in apps}
    covers = {app['cover_url']:app['cover_data'] for app in apps if 'cover_data' in app}
    catalog = dict(schema=1, layout=1, board=BOARD, apps=[
        {**{key: app[key] for key in ("app_id", "name", "version", "size", "sha256", "cover_url", "cover_size", "cover_sha256", "cover_format") if key in app},
         "url": "/firmware/" + app["app_id"] + ".bin"} for app in apps])
    class Handler(BaseHTTPRequestHandler):
        def do_GET(self):
            route = urlsplit(self.path).path
            if route in ("/api/apps", "/catalog.json"):
                body = json.dumps(catalog, ensure_ascii=False).encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "application/json; charset=utf-8")
                self.send_header("Content-Length", str(len(body)))
                self.send_header("Cache-Control", "no-store")
                self.end_headers(); self.wfile.write(body)
            elif route in covers:
                data=covers[route]
                self.send_response(200)
                self.send_header('Content-Type','application/octet-stream')
                self.send_header('Content-Length',str(len(data)))
                self.send_header('Cache-Control','public,max-age=31536000,immutable')
                self.end_headers();self.wfile.write(data)
            elif route in allowed:
                app = allowed[route]
                # Refuse changed artifacts rather than silently serving the wrong checksum.
                try:
                    data = app["path"].read_bytes()
                except OSError:
                    self.send_error(409, "App image unavailable; restart the test store"); return
                if hashlib.sha256(data).hexdigest() != app["sha256"]:
                    self.send_error(409, "App image changed; restart the test store"); return
                self.send_response(200)
                self.send_header("Content-Type", "application/octet-stream")
                self.send_header("Content-Length", str(len(data)))
                self.end_headers(); self.wfile.write(data)
            else:
                self.send_error(404)
        def log_message(self, message, *args):
            # Requests do not contain credentials. Keep query strings out of logs.
            print(f"{self.client_address[0]}: {self.command} {urlsplit(self.path).path}", flush=True)
    return Handler


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bundle", type=local_path, default="packages",
                        help="application .bin directory, relative to this service (default: packages)")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--covers-config",type=local_path,default="covers.json")
    args = parser.parse_args()
    try:
        covers=json.loads(args.covers_config.read_text(encoding='utf-8')) if args.covers_config.is_file() else {}
        apps = load_apps(args.bundle,covers,source_root=args.covers_config.parent)
    except (OSError, ValueError) as error:
        parser.error(str(error))
    server = ThreadingHTTPServer((args.host, args.port), make_handler(apps))
    print(f"Firmware directory: {args.bundle} ({len(apps)} installable applications)", flush=True)
    if not apps:
        print("Copy application .bin files here, then restart the server. No package.json is required.", flush=True)
    addresses = sorted(set(socket.gethostbyname_ex(socket.gethostname())[2]))
    print("Local test app store. Use the PC address on the same WiFi as the device:", flush=True)
    for address in addresses:
        if not address.startswith("127."):
            print(f"  http://{address}:{args.port}/api/apps", flush=True)
    print("Set this URL at http://DEVICE_IP:8081, then open Online apps. Ctrl+C stops the server.", flush=True)
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        server.server_close()


if __name__ == "__main__":
    main()
