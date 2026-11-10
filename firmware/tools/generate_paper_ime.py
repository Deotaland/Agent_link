"""Generate the offline Paper IME table from locally cached, hash-recorded MIT data."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import unicodedata

ROOT = Path(__file__).resolve().parents[1]
parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('source_directory', type=Path)
args = parser.parse_args()
source = args.source_directory
assets = ROOT / 'components/paper_resources/assets'
licenses = assets / 'ime-data'
licenses.mkdir(parents=True, exist_ok=True)

for project in ('pinyin', 'jieba'):
    record = json.loads((source / (project + '-source.json')).read_text())
    for name, entry in record.items():
        data = (source / (project + '-' + Path(name).name)).read_bytes()
        if hashlib.sha256(data).hexdigest() != entry['sha256']:
            raise SystemExit('Source hash mismatch: ' + project + '/' + name)
    shutil.copyfile(source / (project + '-LICENSE'), licenses / (project + '-LICENSE.txt'))
    shutil.copyfile(source / (project + '-source.json'), licenses / (project + '-source.json'))

hanzi = set()
for hi in range(0xb0, 0xf8):
    for lo in range(0xa1, 0xff):
        try:
            hanzi.add(bytes((hi, lo)).decode('gb2312'))
        except UnicodeDecodeError:
            pass
frequency = {}
for line in (source / 'jieba-dict.txt').read_text(encoding='utf-8').splitlines():
    fields = line.split()
    if len(fields) >= 2 and len(fields[0]) == 1 and fields[1].isdigit():
        frequency[fields[0]] = int(fields[1])

def plain(value):
    value = value.replace('ü', 'v')
    # Decomposed umlauts must become v rather than u.
    value = unicodedata.normalize('NFD', value).replace('u\u0308', 'v')
    return ''.join(c for c in value if 'a' <= c <= 'z')

entries = {}
covered = set()
for line in (source / 'pinyin-pinyin.txt').read_text(encoding='utf-8').splitlines():
    match = re.match(r'U\+([0-9A-Fa-f]+):\s*([^#]+)', line)
    if not match:
        continue
    char = chr(int(match[1], 16))
    if char not in hanzi:
        continue
    for index, reading in enumerate(match[2].strip().split(',')):
        pinyin = plain(reading)
        if pinyin:
            entries[(pinyin, char)] = max(1, frequency.get(char, 5) // (index + 1))
            covered.add(char)

phrases = {
    '你好':'nihao', '谢谢':'xiexie', '再见':'zaijian', '早上好':'zaoshanghao',
    '晚上好':'wanshanghao', '你好世界':'nihaoshijie', '智能体':'zhinengti',
    '墨水屏':'moshuiping', '触摸':'chumo', '输入法':'shurufa', '拼音':'pinyin',
    '消息':'xiaoxi', '发送':'fasong', '请问':'qingwen', '为什么':'weishenme',
    '怎么样':'zenmeyang', '可以':'keyi', '我们':'women', '你们':'nimen',
    '今天':'jintian', '明天':'mingtian', '天气':'tianqi', '时间':'shijian',
    '设置':'shezhi', '闹钟':'naozhong', '音乐':'yinyue', '播放':'bofang',
    '停止':'tingzhi', '打开':'dakai', '关闭':'guanbi', '测试':'ceshi',
}
for text, pinyin in phrases.items():
    if not set(text) <= covered:
        raise SystemExit('Missing phrase glyph: ' + text)
    entries[(pinyin, text)] = 1000000
if len(covered) < 6700:
    raise SystemExit(f'Unexpected character coverage: {len(covered)}')
lines = []
for (pinyin, text), count in sorted(entries.items()):
    lines.append(f'{pinyin}\t{text}\t{count}')
(assets / 'pinyin.tsv').write_text('\n'.join(lines) + '\n', encoding='utf-8', newline='\n')
print(f'Offline dictionary: {len(covered)} GB2312 characters, {len(entries)} readings/phrases')
print('Repack shared resources with: python firmware/tools/generate_paper_resources.py')
