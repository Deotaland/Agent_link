"""Check real language defaults, saved choices and rendered WiFi portal states."""
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
from html.parser import HTMLParser

root = Path(__file__).resolve().parents[1]
build = root / 'build/language-tests'
build.mkdir(parents=True, exist_ok=True)
source = build / 'language.cc'
source.write_text(r'''
#include "amoled_ui_language.h"
#include "amoled_ui_guides.h"
#include "portal_language.h"
#include "ui_text.h"
#include <cassert>
#include <fstream>
#include <iterator>
int main(int argc, char** argv) {
    using namespace amoled_ui;
    assert(argc==3);
    assert(kDefaultLanguage==(AGENT_LINK_UI_DEFAULT_ENGLISH ? Language::kEnglish : Language::kChinese));
    assert(language.load()==kDefaultLanguage);
    assert(SavedLanguage(0)==Language::kChinese && SavedLanguage(1)==Language::kEnglish);
    assert(SavedLanguage(42)==kDefaultLanguage);
    assert(std::string(Tr("Chinese", "English"))==(AGENT_LINK_UI_DEFAULT_ENGLISH ? "English" : "Chinese"));
    language=SavedLanguage(0); assert(std::string(Tr("Chinese", "English"))=="Chinese");
    language=SavedLanguage(1); assert(std::string(Tr("Chinese", "English"))=="English");
    assert(std::string(DeviceBindingGuide()).find("https://bot.deotaland.cn/#/devices")!=std::string::npos);
    assert(std::string(DownloadCodeGuide()).find("https://sugarat.vercel.app/")!=std::string::npos);
    assert(std::string(agent_system::DefaultUiText("Chinese", "English"))==
           (AGENT_LINK_UI_DEFAULT_ENGLISH ? "English" : "Chinese"));
    std::ifstream input(argv[1],std::ios::binary);
    assert(input);
    std::string page(std::istreambuf_iterator<char>(input),{});
    ApplyPortalLanguage(page);
    assert(page.find("{{LANG_CHECKED}}")==std::string::npos && page.find("{{LANGUAGE}}")==std::string::npos);
    std::ofstream(argv[2],std::ios::binary)<<page;
}
''', encoding='utf-8')
env = os.environ.copy()
if os.name == 'nt':
    locator = Path(os.environ['ProgramFiles(x86)']) / 'Microsoft Visual Studio/Installer/vswhere.exe'
    vs = subprocess.check_output([str(locator), '-latest', '-products', '*', '-requires',
                                 'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                                 '-property', 'installationPath'], text=True).strip()
    for line in subprocess.check_output(f'"{vs}/VC/Auxiliary/Build/vcvarsall.bat" x64 >nul && set',
                                        shell=True, text=True).splitlines():
        if '=' in line and not line.startswith('='):
            key, value = line.split('=', 1)
            env[key] = value
    compiler = shutil.which('cl.exe', path=env.get('Path') or env.get('PATH'))
else:
    compiler = 'c++'
includes = [root/'components/amoled_ui/include', root/'components/agent_system',
            root/'components/agent_link/src/transports/wifi']

class Page(HTMLParser):
    def __init__(self, text):
        super().__init__()
        self.elements = []
        self.feed(text)

    def handle_starttag(self, tag, attrs):
        self.elements.append((tag, dict(attrs)))

for english in (0, 1):
    output = build / ('en' if english else 'zh')
    output.mkdir(exist_ok=True)
    binary = output / ('language.exe' if os.name == 'nt' else 'language')
    if os.name == 'nt':
        command = [compiler, '/nologo', '/std:c++17', '/EHsc', '/utf-8',
                   f'/DAGENT_LINK_UI_DEFAULT_ENGLISH={english}', *['/I'+str(p) for p in includes],
                   str(source), '/Fo'+str(output/'language.obj'), '/Fe'+str(binary)]
    else:
        command = [compiler, '-std=c++17', f'-DAGENT_LINK_UI_DEFAULT_ENGLISH={english}',
                   *['-I'+str(p) for p in includes], str(source), '-o', str(binary)]
    subprocess.run(command, env=env, cwd=output, check=True)
    for name in ('portal.html', 'connecting.html'):
        target = output/name
        subprocess.run([str(binary), str(root/'components/agent_link/portal'/name), str(target)], check=True)
        page = Page(target.read_text(encoding='utf-8'))
        locale = next(attrs for tag, attrs in page.elements if tag=='input' and attrs.get('id')=='lang')
        assert ('checked' in locale)==bool(english)
        assert next(attrs['lang'] for tag, attrs in page.elements if tag=='html')==('en' if english else 'zh-CN')
        if name=='portal.html':
            assert any(tag=='form' and attrs.get('action')=='/provision' for tag, attrs in page.elements)
            assert {'ssid','password'} <= {attrs.get('name') for tag, attrs in page.elements if tag=='input'}

# Execute the actual progress-page script with a minimal browser boundary.
script = re.search(r'<script>(.*?)</script>', (build/'en/connecting.html').read_text(encoding='utf-8'), re.S)[1]
javascript = build/'progress-test.cjs'
javascript.write_text('''
const vm=require('node:vm'),assert=require('node:assert/strict');
const elements={};
const context={document:{getElementById(id){return elements[id] ||= {checked:id==='lang',style:{},textContent:''};}},
    setTimeout(){return 1;},clearTimeout(){},fetch(){return new Promise(()=>{});}};
vm.createContext(context);
vm.runInContext(SCRIPT,context);
for(const [state,expected] of [[{state:'connecting'},'Connecting'],[{state:'connected',ssid:'Test WiFi',ip:'192.168.1.2'},'WiFi connected'],
    [{state:'failed',reason:'badpass'},'Connection failed'],[{state:'unknown'},'Check the device']]) {
    context.last=state;context.render();assert.ok(elements.t.textContent.includes(expected));
    assert.ok(!/[\\u4e00-\\u9fff]/.test(elements.t.textContent+elements.m.textContent));
}
elements.lang.checked=false;context.render();assert.ok(/[\\u4e00-\\u9fff]/.test(elements.t.textContent));
'''.replace('SCRIPT', json.dumps(script)), encoding='utf-8')
subprocess.run([shutil.which('node') or 'node', str(javascript)], check=True)
print('UI languages: English defaults, saved Chinese/English, unchanged other-board defaults, both WiFi pages and live results passed.')
