#!/usr/bin/env python3
"""Read-only Agent Link environment checks. Never install, build or open a serial port."""

import argparse
import concurrent.futures
import importlib.util
import json
import os
from pathlib import Path
import re
import subprocess
import sys
import urllib.request

ROOT = Path(__file__).resolve().parents[1]
GUIDE = "guides/environment.zh-CN.md"
NETWORK = {
    "idf-tools-cn": "https://dl.espressif.cn/github_assets/",
    "idf-installer-cn": "https://dl.espressif.cn/dl/esp-idf/",
    "components": "https://components.espressif.com/",
    "python-packages-cn": "https://pypi.tuna.tsinghua.edu.cn/simple/pip/",
}
ZH = {
    "Checking tools (read-only; up to 15 seconds per SDK check)...": "正在检查工具（只读；每项 SDK 检查最多 15 秒）…",
    "Tools ready.": "工具已就绪，可以继续构建。",
    "Not ready; resolve FAIL items and check again.": "工具尚未就绪，请处理 [FAIL] 项后重新检查。",
    "Tool readiness only; dependency downloads, firmware build and device behavior are not verified.":
        "此结果只验证工具；依赖下载、固件编译和真机运行需要分别验证。",
    "installed but not active in this terminal": "已安装，但当前终端没有激活这套 SDK",
    "not found": "未找到",
    "not selected": "未选择设备方案",
    "active": "已选择该 SDK 路径，继续核对 Python 和工具",
    "available": "可用",
    "satisfied": "依赖齐全",
    "missing or incompatible": "未找到或版本不符合要求",
    "missing in this Python": "当前 Python 中未安装",
    "idf.py starts": "idf.py 可正常启动",
    "idf.py cannot start with this Python": "idf.py 无法使用当前 Python 启动",
    "missing or wrong Python environment": "缺少依赖或使用了错误的 Python 环境",
    "none found": "未发现串口",
    "enumeration unavailable": "暂时无法枚举串口",
    "reachable": "入口可访问",
    "probe failed; check proxy/DNS or try the exact package URL (not a build verdict)":
        "入口探测未通过；检查代理/DNS或具体下载地址，此结果不代表构建失败",
    "Use the Python supplied by your selected ESP-IDF; Agent Link tools require Python 3.10+.":
        "使用所选 ESP-IDF 提供的 Python；本项目工具需要 Python 3.10 或以上。",
    "Use a short ASCII path without spaces. See the environment guide before relocating existing work.":
        "使用短的英文路径，避免空格和中文；已有工作先按指南保留修改再迁移。",
    "Select your board first, then pass --profile NAME; --list shows the available profiles.":
        "先确认板型，再传 --profile 方案名；--list 可列出方案。",
    "Install the selected SDK using the China/offline steps in guides/environment.zh-CN.md":
        "按 guides/environment.zh-CN.md 的国内/离线步骤安装所需 SDK。",
    "S3/C3: 5.5.x (new users: 5.5.4) or 6.1.x. S31: 6.1.x with esp32s31 support.":
        "S3/C3 使用 5.5.x 或 6.1.x，新装优先 5.5.4；S31 需要带 esp32s31 支持的 6.1.x。",
    "Reopen the selected IDF terminal. If still failing, repair only this SDK using its installer.":
        "重新打开对应版本的 IDF 终端；仍失败时用安装器修复这套 SDK，保留其他 SDK。",
    "Use the tools from the selected IDF terminal (CMake 3.22+).":
        "使用对应 IDF 终端内的工具；CMake 需要 3.22 或以上。",
    "Activate ESP-IDF. For prebuilt USB flashing only: python -m pip install esptool pyserial":
        "先激活 ESP-IDF；只烧录现成固件时可运行 python -m pip install esptool pyserial。",
    "Enumeration does not identify the board. Check the data cable, correct connector and driver; no port was opened.":
        "枚举不代表已确认板型；检查数据线、正确接口与驱动。本次没有打开串口。",
    "Check pyserial and OS driver permissions.": "检查 pyserial、系统驱动和访问权限。",
}


def translate(text, language):
    if language == "en":
        return text
    if text.startswith("Use the registered activation script: "):
        return text.replace("Use the registered activation script: ", "使用登记的激活脚本：", 1)
    if text.startswith("Source the registered activation script: "):
        return text.replace("Source the registered activation script: ", "在当前终端加载登记的激活脚本：", 1)
    if text.startswith("Open the installed ESP-IDF PowerShell, or run: "):
        return text.replace("Open the installed ESP-IDF PowerShell, or run: ", "打开已安装的 ESP-IDF PowerShell，或执行：", 1)
    if text.startswith("Run in this terminal: "):
        return text.replace("Run in this terminal: ", "在当前终端执行：", 1)
    if text.startswith("Install tools for "):
        return "在所选 SDK 中安装 " + text.split()[3] + " 的工具，只安装本次芯片。"
    return ZH.get(text, text)


def run(command, timeout=15):
    try:
        proc = subprocess.run(command, capture_output=True, text=True, errors="replace", timeout=timeout)
        return proc.returncode == 0, proc.stdout + proc.stderr
    except (OSError, subprocess.TimeoutExpired):
        return False, ""


def idf_version(path):
    try:
        content = (path / "tools/cmake/version.cmake").read_text(encoding="utf-8")
        return tuple(int(re.search(r"set\(IDF_VERSION_" + part + r"\s+(\d+)\)", content)[1])
                     for part in ("MAJOR", "MINOR", "PATCH"))
    except (OSError, TypeError, ValueError):
        return ()


def supports(path, target):
    return idf_version(path)[:2] in ((5, 5), (6, 1)) and (path / "components/soc" / target).is_dir()


def registry_entries(roots):
    entries = []
    for root in roots:
        for name in ("esp_idf.json", "eim_idf.json", "tools/eim_idf.json"):
            try:
                data = json.loads((root / name).read_text(encoding="utf-8"))
                installed = data.get("idfInstalled", {})
                entries.extend(installed.values() if isinstance(installed, dict) else installed)
            except (OSError, ValueError, AttributeError):
                pass
    return [entry for entry in entries if isinstance(entry, dict) and entry.get("path")]


def install_roots(extra=(), environ=None, home=None):
    env = os.environ if environ is None else environ
    home = Path.home() if home is None else Path(home)
    roots = [home / "esp", home / ".espressif", Path("/opt"), *map(Path, extra)]
    if env.get("IDF_TOOLS_PATH"):
        roots.append(Path(env["IDF_TOOLS_PATH"]))
    if os.name == "nt":
        roots += [Path("C:/Espressif"), Path("C:/esp")]
    return roots


def discover(extra=(), environ=None, home=None):
    """Bounded lookup: registered, previously used and conventional SDK folders only."""
    env = os.environ if environ is None else environ
    home = Path.home() if home is None else Path(home)
    candidates = [Path(env["IDF_PATH"])] if env.get("IDF_PATH") else []
    for file in (ROOT / "firmware/.firmware-profile.json", ROOT / "firmware/build/project_description.json"):
        try:
            data = json.loads(file.read_text(encoding="utf-8"))
            value = data.get("idf_path") or data.get("idf", {}).get("path")
            if value:
                candidates.append(Path(value))
        except (OSError, ValueError, AttributeError):
            pass
    roots = install_roots(extra, env, home)
    for root in roots:
        candidates += [root, root / "esp-idf"]
        for pattern in ("v*/esp-idf", "esp-idf*", "frameworks/esp-idf*"):
            candidates.extend(root.glob(pattern))
    candidates += [Path(entry["path"]) for entry in registry_entries(roots)]
    return sorted({p.resolve() for p in candidates if (p / "tools/idf.py").is_file()}, key=str)


def activation(path, extra=()):
    for entry in registry_entries(install_roots(extra)):
        script = entry.get("activationScript")
        if Path(entry["path"]).resolve() == path and script and Path(script).is_file():
            if os.name == "nt":
                return "Use the registered activation script: & '" + script.replace("'", "''") + "'"
            return "Source the registered activation script: . '" + script.replace("'", "'\"'\"'") + "'"
    if os.name == "nt":
        return 'Open the installed ESP-IDF PowerShell, or run: & "' + str(path / "export.ps1") + '"'
    return "Run in this terminal: . " + "'" + str(path / "export.sh").replace("'", "'\"'\"'") + "'"


def probe(url):
    try:
        req = urllib.request.Request(url, headers={"User-Agent": "Agent-Link-environment-check"})
        with urllib.request.urlopen(req, timeout=6) as response:
            response.read(1)
        return "OK", "reachable"
    except Exception:
        # An index/edge can refuse probes while individual packages remain downloadable.
        # Do not expose proxy credentials or private URLs from exception strings.
        return "WARN", "probe failed; check proxy/DNS or try the exact package URL (not a build verdict)"


def inspect(args):
    checks = []

    def add(name, status, detail, hint=""):
        checks.append(dict(name=name, status=status, detail=detail, hint=hint))

    add("python", "OK" if sys.version_info >= (3, 10) else "FAIL", sys.version.split()[0],
        "Use the Python supplied by your selected ESP-IDF; Agent Link tools require Python 3.10+.")
    add("project-path", "FAIL" if re.search(r"\s|[^\x00-\x7f]", str(ROOT)) else "OK", str(ROOT),
        "Use a short ASCII path without spaces. See the environment guide before relocating existing work.")
    profiles = json.loads((ROOT / "firmware/profiles/profiles.json").read_text(encoding="utf-8"))
    target = profiles[args.profile]["target"] if args.profile else None
    if args.mode == "build":
        add("profile", "OK" if target else "FAIL", args.profile or "not selected",
            "Select your board first, then pass --profile NAME; --list shows the available profiles.")
        found = discover(args.search_dir)
        active = os.environ.get("IDF_PATH")
        selected = Path(args.idf_path).resolve() if args.idf_path else Path(active).resolve() if active else None
        if selected is None:
            matching = [p for p in found if target and supports(p, target)]
            selected = max(matching, key=idf_version) if matching else (found[0] if found else None)
        if selected is None:
            add("esp-idf", "FAIL", "not found", "Install the selected SDK using the China/offline steps in " + GUIDE)
        else:
            version = idf_version(selected)
            compatible = bool(target and supports(selected, target))
            add("esp-idf", "OK" if compatible else "FAIL", str(selected) + " / " + ".".join(map(str, version)),
                "S3/C3: 5.5.x (new users: 5.5.4) or 6.1.x. S31: 6.1.x with esp32s31 support.")
            activated = bool(active and Path(active).resolve() == selected)
            add("activation", "OK" if activated else "FAIL", "active" if activated else "installed but not active in this terminal",
                activation(selected, args.search_dir))
            if compatible and activated:
                ok, _ = run([sys.executable, str(selected / "tools/idf.py"), "--version"])
                add("idf-python", "OK" if ok else "FAIL", "idf.py starts" if ok else "idf.py cannot start with this Python",
                    activation(selected, args.search_dir))
                ok, _ = run([sys.executable, str(selected / "tools/idf_tools.py"),
                             "--idf-path", str(selected), "check-python-dependencies"])
                add("python-dependencies", "OK" if ok else "FAIL", "satisfied" if ok else "missing or wrong Python environment",
                    "Reopen the selected IDF terminal. If still failing, repair only this SDK using its installer.")
                compiler = "xtensa-esp-elf-gcc" if target == "esp32s3" else "riscv32-esp-elf-gcc"
                ok, _ = run([compiler, "--version"])
                add("compiler", "OK" if ok else "FAIL", compiler,
                    "Install tools for " + target + " in the selected SDK; do not install all targets.")
        for name, command in (("git", ["git", "--version"]), ("cmake", ["cmake", "--version"]),
                              ("ninja", ["ninja", "--version"])):
            ok, out = run(command)
            if name == "cmake" and ok:
                match = re.search(r"(\d+)\.(\d+)", out)
                ok = bool(match and tuple(map(int, match.groups())) >= (3, 22))
            add(name, "OK" if ok else "FAIL", "available" if ok else "missing or incompatible",
                "Use the tools from the selected IDF terminal (CMake 3.22+).")
    for module in ("esptool", "serial"):
        try:
            available = importlib.util.find_spec(module) is not None
        except (ImportError, ValueError):
            available = False
        add(module, "OK" if available else "FAIL", "available" if available else "missing in this Python",
            "Activate ESP-IDF. For prebuilt USB flashing only: python -m pip install esptool pyserial")
    if args.ports:
        try:
            from serial.tools import list_ports
            ports = [p.device for p in list_ports.comports()]
            add("serial-ports", "INFO", ", ".join(ports) if ports else "none found",
                "Enumeration does not identify the board. Check the data cable, correct connector and driver; no port was opened.")
        except (ImportError, OSError):
            add("serial-ports", "WARN", "enumeration unavailable", "Check pyserial and OS driver permissions.")
    if args.network:
        with concurrent.futures.ThreadPoolExecutor(max_workers=4) as pool:
            results = list(pool.map(probe, NETWORK.values()))
        for (name, url), (status, detail) in zip(NETWORK.items(), results):
            add("network:" + name, status, detail, url)
    return dict(mode=args.mode, profile=args.profile, target=target,
                ready=not any(c["status"] == "FAIL" for c in checks),
                scope="Tool readiness only; dependency downloads, firmware build and device behavior are not verified.",
                installed_idf=[str(p) for p in discover(args.search_dir)] if args.mode == "build" else [],
                guide=GUIDE, checks=checks)


def main(argv=None):
    for stream in (sys.stdout, sys.stderr):
        if hasattr(stream, "reconfigure"):
            stream.reconfigure(encoding="utf-8")
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile")
    parser.add_argument("--mode", choices=("build", "flash"), default="build")
    parser.add_argument("--idf-path", help="inspect this existing SDK; does not activate or install it")
    parser.add_argument("--search-dir", action="append", default=[], help="additional SDK installation parent folder")
    parser.add_argument("--network", action="store_true", help="optional bounded public download-site probes")
    parser.add_argument("--ports", action="store_true", help="enumerate ports without opening them")
    parser.add_argument("--json", action="store_true")
    parser.add_argument("--lang", choices=("zh", "en"), default="zh", help="human-readable report language; JSON stays stable")
    parser.add_argument("--list", action="store_true")
    args = parser.parse_args(argv)
    profiles = json.loads((ROOT / "firmware/profiles/profiles.json").read_text(encoding="utf-8"))
    if args.list:
        print("\n".join(profiles))
        return 0
    if args.profile and args.profile not in profiles:
        parser.error("Unknown profile. Run python tools/check_env.py --list")
    if not args.json:
        print(translate("Checking tools (read-only; up to 15 seconds per SDK check)...", args.lang), flush=True)
    result = inspect(args)
    if args.json:
        print(json.dumps(result, ensure_ascii=False, indent=2))
    else:
        for check in result["checks"]:
            print("[{status}] {name}: {detail}".format(**dict(check, detail=translate(check["detail"], args.lang))))
            if check["hint"] and check["status"] != "OK":
                print("  " + translate(check["hint"], args.lang))
        print(translate("Tools ready." if result["ready"] else "Not ready; resolve FAIL items and check again.", args.lang))
        print(translate(result["scope"], args.lang) + (" Guide: " if args.lang == "en" else " 指南：") + GUIDE)
        for path in result["installed_idf"]:
            print(("Installed SDK: " if args.lang == "en" else "已有 SDK：") + path)
    return 0 if result["ready"] else 1


if __name__ == "__main__":
    sys.exit(main())
