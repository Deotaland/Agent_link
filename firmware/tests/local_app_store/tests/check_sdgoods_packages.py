"""Download real local packages and test their SDK installation with simulated flash.

The HTTP server and all subprocesses are bounded. No serial port or device is used.
"""
import hashlib
import json
from pathlib import Path
import subprocess
import sys
import threading
from http.server import ThreadingHTTPServer
import urllib.error
import urllib.request

STORE = Path(__file__).resolve().parents[1]
FIRMWARE = STORE.parents[1]
sys.path.insert(0, str(STORE))
import server as store
from multi_app_image import target
from sdgoods_image import verify_application


def main():
    profile = 'sdgoods-wifi'
    selected = target(profile)
    apps = store.load_apps(STORE / 'packages', profile=profile)
    if not any(app['image_format']=='agent-link' for app in apps) or not any(app['image_format']=='sdgoods' for app in apps):
        raise ValueError('Place both a native Sugarat application and approved vendor .bin files in packages')
    cache = STORE / '.cache/sdgoods-downloads'
    cache.mkdir(parents=True, exist_ok=True)
    report = dict(profile=profile, layout=selected['layout']['id'],
                  hardware_tested=False, sdk_origin='http://192.168.1.2:8765',
                  pages=[], apps=[], sdk_installation='not-run')
    service = ThreadingHTTPServer(('127.0.0.1', 0), store.make_handler(apps, profile))
    worker = threading.Thread(target=service.serve_forever, daemon=True)
    worker.start()
    origin = 'http://127.0.0.1:'+str(service.server_address[1])
    client = urllib.request.build_opener(urllib.request.ProxyHandler({}))
    try:
        for page in range(1, (len(apps)+7)//8+1):
            route = '/api/apps?page='+str(page)
            with client.open(origin+route, timeout=10) as response:
                catalog = json.load(response)
            assert catalog['board']=='SDGOODS-ESP32S3' and catalog['layout']==3
            assert len(catalog['apps'])<=8
            report['pages'].append(dict(route=route, catalog=catalog))
            for app in catalog['apps']:
                with client.open(origin+app['url'], timeout=20) as response:
                    data = response.read()
                    assert response.status==200
                    assert int(response.headers['Content-Length'])==app['size']==len(data)
                assert hashlib.sha256(data).hexdigest()==app['sha256']
                source = next(item['path'] for item in apps if item['app_id']==app['app_id'])
                assert data==source.read_bytes(), app['app_id']
                downloaded = cache / (app['app_id']+'.bin')
                downloaded.write_bytes(data)
                verified = verify_application(downloaded, selected['maximum'])
                assert verified['sha256']==app['sha256'] and verified['version']==app['version']
                report['apps'].append(dict(app, download_path=str(downloaded), source=str(source),
                                           esp_image_integrity='passed', http_download='passed'))
        assert len(report['apps'])==len(apps)
        for route in ('/../agent_link.bin', '/firmware/SDGOODS_LAUNCHER.bin', '/api/apps?page=0', '/api/apps?page=999'):
            try:
                client.open(origin+route, timeout=5)
                raise AssertionError('Unexpectedly served '+route)
            except urllib.error.HTTPError as error:
                assert error.code==404
    finally:
        service.shutdown()
        service.server_close()
        worker.join(timeout=5)
        assert not worker.is_alive()
    path = STORE / '.cache/sdgoods-http-report.json'
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print('Actual HTTP downloads and ESP image checks passed:', len(apps), 'apps,', len(report['pages']), 'catalogs', flush=True)
    try:
        subprocess.run([sys.executable, str(FIRMWARE/'tests/test_agent_system.py'),
                        '--packages-report', str(path),
                        *(['--dynamic'] if selected['layout'].get('dynamic_slots') else [])], check=True)
    except subprocess.CalledProcessError:
        report['sdk_installation']='failed'
        path.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
        raise
    report['sdk_installation']='passed-with-simulated-flash-and-HTTP-adapters'
    report['simulated_installs']=len(apps)+9 if selected['layout'].get('dynamic_slots') else len(apps)*2
    report['simultaneously_installed']=5 if selected['layout'].get('dynamic_slots') else 2
    path.write_text(json.dumps(report, ensure_ascii=False, indent=2)+'\n', encoding='utf-8')
    print('Report:', path)


if __name__=='__main__':
    main()
