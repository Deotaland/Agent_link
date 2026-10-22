"""Check the local ESP-IDF dynamic RX implementation with a fragmented-heap fixture."""
import argparse
import os
from pathlib import Path
import shutil
import subprocess

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('--idf-path', type=Path, required=True)
args = parser.parse_args()
root = Path(__file__).resolve().parents[1]
directory = args.idf_path / 'components/mbedtls/port/dynamic'
implementation = (directory / 'esp_mbedtls_dynamic_impl.c').read_text(encoding='utf-8')
header = (directory / 'esp_mbedtls_dynamic_impl.h').read_text(encoding='utf-8')
fixture = (root / 'tests/tls_receive_memory_test.cc').read_text(encoding='utf-8')
types = header[header.index('typedef enum {'):header.index('void esp_mbedtls_free_buf')]
guard = implementation[implementation.index('#define ESP_MBEDTLS_RETURN_IF_RX_BUF_STATIC'):implementation.index('static const char *TAG')]
functions = []
for name in ['static void esp_mbedtls_set_buf_state', 'static esp_mbedtls_ssl_buf_states esp_mbedtls_get_buf_state',
             'void esp_mbedtls_free_buf', 'static void esp_mbedtls_init_ssl_buf',
             'static void esp_mbedtls_parse_record_header', 'static int tx_buffer_len',
             'static void init_rx_buffer', 'void esp_mbedtls_reset_free_rx_buffer',
             'esp_err_t esp_mbedtls_dynamic_set_rx_buf_static', 'int esp_mbedtls_add_rx_buffer',
             'int esp_mbedtls_free_rx_buffer']:
    begin = implementation.index(name + '(')
    functions.append(implementation[begin:implementation.index('\n}', begin) + 2])
fixture = fixture.replace('// INSERT_IDF_BUFFER_TYPES', types).replace('// INSERT_IDF_STATIC_GUARD', guard)
fixture = fixture.replace('// INSERT_IDF_RX_FUNCTIONS', '\n\n'.join(functions))
build = root / '.cache/tls-receive-memory-tests'
build.mkdir(parents=True, exist_ok=True)
source = build / 'tls_receive_memory_test.cc'
source.write_text(fixture, encoding='utf-8')
environment = os.environ.copy()
if os.name == 'nt':
    vswhere = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)')) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    installation = subprocess.check_output([str(vswhere), '-latest', '-products', '*',
        '-requires', 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64', '-property', 'installationPath'], text=True).strip()
    if not installation:
        raise SystemExit('An installed Visual C++ toolchain is required')
    setup = Path(installation) / 'VC/Auxiliary/Build/vcvarsall.bat'
    result = subprocess.check_output(f'"{setup}" x86 >nul && set', shell=True, text=True, errors='replace')
    for line in result.splitlines():
        if '=' in line and not line.startswith('='):
            name, value = line.split('=', 1)
            environment[name] = value
    compiler = shutil.which('cl.exe', path=environment.get('Path') or environment.get('PATH'))
    binary = build / 'tls_receive_memory_test.exe'
    subprocess.run([compiler, '/nologo', '/std:c++17', '/EHsc', '/W4', '/D_CRT_SECURE_NO_WARNINGS',
        str(source), '/Fo' + str(build) + '/', '/Fe' + str(binary)], env=environment, cwd=build, check=True)
else:
    binary = build / 'tls_receive_memory_test'
    subprocess.run(['c++', '-std=c++17', '-fpermissive', '-Wall', '-Wextra', str(source), '-o', str(binary)], cwd=build, check=True)
subprocess.run([str(binary)], check=True, timeout=30)
