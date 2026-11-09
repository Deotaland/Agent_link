"""Exercise the LAN catalog/download contract, including changed artifacts and hidden files."""
import io
import json
from pathlib import Path
import struct
import tempfile
import threading
import unittest
import urllib.error
import urllib.request
import hashlib
import zlib
import subprocess
import sys
from http.server import ThreadingHTTPServer

ROOT = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(ROOT))
import server as store
CACHE = ROOT / ".cache/tests"
CACHE.mkdir(parents=True, exist_ok=True)


class TestLocalStore(unittest.TestCase):
    def test_user_cover_contract(self):
        source=ROOT/'assets/voice.png'
        if not source.is_file(): self.skipTest('No user-provided image available locally')
        data=store.convert_cover(source.read_bytes(),'agent.voice')
        actual=store.validate_cover(data)
        self.assertEqual(actual['size'],52336)
        self.assertEqual(actual['format'],store.FORMAT)
        self.assertEqual(actual['sha256'],hashlib.sha256(data).hexdigest())
        invalid=bytearray(data);invalid[10]=1
        with self.assertRaises(ValueError):store.validate_cover(bytes(invalid))
        invalid=bytearray(data);invalid[64]^=1
        with self.assertRaises(ValueError):store.validate_cover(bytes(invalid))
        with self.assertRaises(ValueError):store.validate_cover(data[:100])
        with self.assertRaises(ValueError):store.validate_cover(data,'agent.other')

    def test_color_order_and_alpha(self):
        from PIL import Image
        from make_app_cover import HEADER, EDGE
        # Protocol fixtures: RGB565LE must not inherit the other board's swapped bytes.
        for rgb, expected in (((255,0,0),b'\x00\xf8'),((0,255,0),b'\xe0\x07'),((0,0,255),b'\x1f\x00')):
            source=io.BytesIO();Image.new('RGBA',(132,132),(*rgb,255)).save(source,format='PNG')
            data=store.convert_cover(source.getvalue(),'agent.example')
            center=66*132+66
            self.assertEqual(data[64+center*2:64+center*2+2],expected)
            self.assertEqual(data[64+132*132*2],0)
            self.assertEqual(data[64+132*132*2+center],255)
            self.assertEqual(HEADER.unpack_from(data)[-2],zlib.crc32(data[64:])&0xffffffff)

    def test_catalog_and_download(self):
        with tempfile.TemporaryDirectory(dir=CACHE) as temporary:
            assert Path(temporary).resolve().is_relative_to(CACHE)
            bundle = Path(temporary)
            data = bytearray(1024); data[0] = 0xE9
            struct.pack_into("<H", data, 12, 9); data[0x30:0x36] = b"2.0.0\0"
            struct.pack_into("<8sII32s64s48s16s", data, 0x120, b"AGSYS001", 1, 1,
                            b"agent.example", b"Example", store.BOARD.encode(), b"0.1.0")
            image = bundle / "example.bin"; image.write_bytes(data)
            # No manifest is needed; even an unrelated broken JSON is ignored.
            (bundle / "package.json").write_text('not a manifest', encoding="utf-8")
            (bundle / "merged.bin").write_bytes(b"must not be served")
            from PIL import Image
            artwork=bundle/'source.png';Image.new('RGB',(150,100),(255,0,0)).save(artwork)
            apps = store.load_apps(bundle,{'agent.example':{'source_file':'source.png'}},
                source_root=bundle,cover_cache=bundle/'.cache/covers')
            server = ThreadingHTTPServer(("127.0.0.1", 0), store.make_handler(apps))
            thread = threading.Thread(target=server.serve_forever, daemon=True); thread.start()
            origin = f"http://127.0.0.1:{server.server_address[1]}"
            try:
                with urllib.request.urlopen(origin + "/api/apps", timeout=5) as response:
                    catalog = json.load(response)
                self.assertEqual(catalog["board"], store.BOARD)
                app = catalog["apps"][0]
                self.assertTrue(app['cover_url'].startswith('/covers/agent.example/'))
                self.assertEqual(app['cover_size'],52336)
                self.assertEqual(app['cover_format'],store.FORMAT)
                with urllib.request.urlopen(origin+app['cover_url'],timeout=5) as response:
                    raw=response.read()
                    self.assertEqual(response.headers['Content-Type'],'application/octet-stream')
                    self.assertEqual(int(response.headers['Content-Length']),52336)
                    self.assertEqual(hashlib.sha256(raw).hexdigest(),app['cover_sha256'])
                    store.validate_cover(raw,'agent.example')
                self.assertNotIn("path", app)
                with urllib.request.urlopen(origin + app["url"], timeout=5) as response:
                    self.assertEqual(int(response.headers["Content-Length"]), len(data))
                    self.assertEqual(response.read(), data)
                for route in ("/merged.bin", "/package.json", "/../merged.bin", "/firmware/agent.launcher.bin"):
                    with self.assertRaises(urllib.error.HTTPError) as error:
                        urllib.request.urlopen(origin + route, timeout=5)
                    self.assertEqual(error.exception.code, 404)
                image.write_bytes(bytes(data) + b"changed")
                with self.assertRaises(urllib.error.HTTPError) as error:
                    urllib.request.urlopen(origin + app["url"], timeout=5)
                self.assertEqual(error.exception.code, 409)
                restarted = store.load_apps(bundle)
                self.assertEqual(restarted[0]['sha256'], hashlib.sha256(bytes(data)+b'changed').hexdigest())
            finally:
                server.shutdown(); server.server_close(); thread.join(timeout=5)
                self.assertFalse(thread.is_alive())

    def test_other_board_is_rejected(self):
        with tempfile.TemporaryDirectory(dir=CACHE) as temporary:
            assert Path(temporary).resolve().is_relative_to(CACHE)
            image = Path(temporary) / "other.bin"
            data = bytearray(1024); data[0] = 0xE9
            struct.pack_into("<H", data, 12, 9)
            struct.pack_into("<8sII32s64s48s16s", data, 0x120, b"AGSYS001", 1, 1,
                            b"agent.example", b"Example", b"OtherBoard", b"0.1.0")
            image.write_bytes(data)
            with self.assertRaises(ValueError):
                store.image_info(image)

    def test_cover_reserve_is_excluded_from_app_images(self):
        from multi_app_image import APP_IMAGE_MAX_BYTES
        with tempfile.TemporaryDirectory(dir=CACHE) as temporary:
            image=Path(temporary)/'too-large.bin'
            data=bytearray(APP_IMAGE_MAX_BYTES+1);data[0]=0xE9
            struct.pack_into('<H',data,12,9);data[0x30:0x36]=b'2.0.0\0'
            struct.pack_into('<8sII32s64s48s16s',data,0x120,b'AGSYS001',1,1,b'agent.example',b'Example',store.BOARD.encode(),b'0.1.0')
            image.write_bytes(data)
            with self.assertRaisesRegex(ValueError,'reserved 56 KiB'):store.image_info(image)

    def test_manual_directory_scan(self):
        with tempfile.TemporaryDirectory(dir=CACHE) as temporary:
            bundle=Path(temporary)
            self.assertEqual(store.load_apps(bundle),[])
            data=bytearray(1024);data[0]=0xE9
            struct.pack_into('<H',data,12,9);data[0x30:0x36]=b'2.0.0\0'
            def write_image(filename,app_id):
                struct.pack_into('<8sII32s64s48s16s',data,0x120,b'AGSYS001',1,1,
                    app_id,b'Example',store.BOARD.encode(),b'0.1.0')
                (bundle/filename).write_bytes(data)
            write_image('renamed-by-user.BIN',b'agent.example')
            write_image('factory.bin',b'agent.launcher')
            (bundle/'merged.bin').write_bytes(b'not an application image')
            apps=store.load_apps(bundle)
            self.assertEqual([a['app_id'] for a in apps],['agent.example'])
            self.assertEqual(apps[0]['path'].name,'renamed-by-user.BIN')
            write_image('same-app-another-version.bin',b'agent.example')
            with self.assertRaisesRegex(ValueError,'Duplicate app ID: agent.example'):
                store.load_apps(bundle)

    def test_start_from_another_directory(self):
        self.assertEqual(store.local_path('packages'),ROOT/'packages')
        with tempfile.TemporaryDirectory(dir=CACHE) as temporary:
            result=subprocess.run([sys.executable,str(ROOT/'server.py'),'--bundle','does-not-exist',
                '--covers-config','does-not-exist.json'],cwd=temporary,capture_output=True,text=True)
            self.assertEqual(result.returncode,2)
            self.assertNotIn('Traceback',result.stderr)
            self.assertIn(str(ROOT/'does-not-exist'),result.stderr)
            result=subprocess.run([sys.executable,'-c',
                'import sys;sys.path.insert(0,sys.argv[1]);import server;print(len(server.load_apps(sys.argv[2])))',
                str(ROOT),temporary],cwd=temporary,capture_output=True,text=True,check=True)
            self.assertEqual(result.stdout.strip(),'0')


if __name__ == "__main__":
    unittest.main()
