# 安装与检查开发环境

[English](environment.md) · [第一次使用](getting-started.zh-CN.md) · [设备操作](devices.zh-CN.md)

## 先决定是否需要安装

| 你要做什么 | 需要准备什么 |
| --- | --- |
| 使用现成固件 | 优先用桌面 Chrome / Edge 打开[固件中心](https://bot.deotaland.cn/#/firmware)，不需要安装 ESP-IDF |
| 用命令行烧录现成固件 | Python、esptool、pyserial；不需要完整 SDK |
| 编译或修改固件 | 按下面流程准备 ESP-IDF 和当前芯片的工具 |

新手可以把下面的话交给能操作本地终端的 AI 助手：

> 在这个 Agent_link 仓库中，先按 guides/environment.zh-CN.md 检查我的开发环境。
> 我在国内，设备是【填写板子名称】。先找已有的 SDK，能复用就激活；缺少时使用乐鑫官方国内源或离线安装包，只安装这块板子的工具。
> 每一步简要说明，保留我的其他 SDK 和配置，最后重新检查并报告结果。此次只准备环境，不烧录。

浏览器里的聊天助手未必能操作本地文件或 USB。如果助手只能给出说明，按图形安装流程自行操作即可。

## 1. 从零准备与选版本

Windows 没有 Python/Git 时，先使用下面的官方图形安装器；无需先运行 Python 脚本。
已存在仓库时继续用当前目录，不要另建源码副本。第一次获取源码可在
[GitHub 仓库](https://github.com/Deotaland/Agent_link)点击 Code → Download ZIP，解压到短的英文目录；
有 Git 时也可 `git clone https://github.com/Deotaland/Agent_link.git`。
GitHub 无法访问时先解决该下载，不要把第三方未知镜像当作本项目官方源码。

| 设备 | 新安装建议 | 已有环境 |
| --- | --- | --- |
| ESP32-S3 / ESP32-C3 | ESP-IDF **5.5.4** | 可复用 5.5.x 或 6.1.x |
| paper V2 / ESP32-S31 | 带 **esp32s31** 支持的 **6.1.x** | 必须检查 SDK 的 `components/soc/esp32s31` 存在 |
| ESP32-P4 | 本仓库尚未适配 | 不要选择相近方案烧录 |

不要让安装器默认的“最新版本”替代上表，也不要勾选所有芯片。忘记芯片时先按[设备指南](devices.zh-CN.md)查型号。
SDK、Python 和仓库尽量使用无空格、无中文的短路径，例如 `C:\esp\idf-5.5.4` 和 `C:\projects\Agent_link`。
已有工作目录需要迁移时先保留未提交修改，由用户选择位置。

## 2. 已经装过：先检查与激活

在仓库根目录运行；macOS/Linux 的 `python` 若不存在，用 `python3`：

```sh
python tools/check_env.py --list
python tools/check_env.py --profile ai-passport-wifi
```

上例是 AI Passport WiFi；换成你的方案。检查器只读取信息，不安装、不编译、不开串口。
它会寻找当前 SDK、安装器登记和常见目录，以及本仓库以前使用的 SDK；找不到自定义位置可加：

```sh
python tools/check_env.py --profile ai-passport-wifi --search-dir D:/esp_idf
```

看到“已安装，但当前终端没有激活”（英文为 **installed but not active**）时，不用重新安装。Windows 打开安装器创建的对应版本
ESP-IDF PowerShell / Command Prompt；EIM 用户使用 Dashboard 给出的激活命令。
传统安装也可在 PowerShell 中运行（路径改成实际位置）：

```powershell
& 'C:\esp\idf-5.5.4\export.ps1'
idf.py --version
```

macOS/Linux 传统安装：

```sh
. "$HOME/esp/esp-idf-v5.5.4/export.sh"
idf.py --version
```

每次新开终端都要激活。EIM 的激活脚本位置由其安装记录决定，不要猜 Python 虚拟环境名称。
不要为了激活关闭系统安全检查；脚本被系统策略拦截时使用官方专用终端或让管理员处理。

## 3. Windows 国内安装：推荐图形流程

1. 打开[乐鑫官方安装入口](https://dl.espressif.com/dl/eim/)，获取 Windows x64 的 EIM 图形安装器。
   下载慢时，S3/C3 可改用[乐鑫国内 Windows 5.x 安装页](https://dl.espressif.cn/dl/esp-idf/)的 **5.5.4 离线包**；
   该旧安装页不提供 6.x，S31 不要用它降级。
2. EIM 选择 **Expert Installation**，芯片只选本次目标，取消 `all`，版本按上表选。
3. 在 **Download Mirrors** 分别选择安装器提供的国内/非 GitHub 的 SDK 源码源和工具源。
   两项要分别检查；工具包下载加速不会自动加速源码和子模块。
4. 选择独立的版本目录，保留工具缓存，安装缺失的 Python、Git、编译工具。
   电脑上有别的 SDK 时不要卸载或切换其源码分支。
5. 安装完成，从 Dashboard/开始菜单打开对应版本环境，回到 Agent_link 根目录，运行第 6 节检查。

找不到 `esp32s31` 或所需 6.1 版本时先停止，确认提供的 SDK 是否包含该芯片；
不能用 S3 工具或 5.5 环境代替。不要默认改用 WSL，原生 Windows 即可完成本项目构建。
安装器按钮可能随版本变化，以[官方图形流程](https://docs.espressif.com/projects/idf-im-ui/en/latest/gui_configuration.html)为准。

## 4. macOS / Linux 国内安装

使用相同的[官方 EIM 下载入口](https://dl.espressif.com/dl/eim/)，选择匹配的系统和 CPU 架构，
按 Expert Installation 选择目标、SDK 版本、国内源码源和工具源。
已有 Homebrew 的 macOS 用户也可按[官方安装说明](https://docs.espressif.com/projects/idf-im-ui/en/latest/)安装 EIM；
不要求新手先安装 Homebrew。Linux 使用官方提供的对应发行版包。

EIM 在 macOS/Linux 不会替你安装全部系统前置工具。Debian/Ubuntu 可先安装：

```sh
sudo apt-get update
sudo apt-get install git wget flex bison gperf python3 python3-pip python3-venv cmake ninja-build ccache libffi-dev libssl-dev dfu-util libusb-1.0-0
```

macOS 检查 `git --version`、`python3 --version`；系统提示时完成 Xcode Command Line Tools 安装。
使用符合所选 IDF 要求的 Python；本项目工具至少需要 Python 3.10。
其他发行版与系统依赖按[乐鑫 Linux/macOS 指南](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/get-started/linux-macos-setup.html)处理。
安装好后使用 EIM 给出的激活命令，再检查当前方案。

### 已有完整 SDK 源码，只缺工具

先确认目录版本和目标支持。以下设置只影响当前终端，不改全局 Git、pip 或系统代理。
PowerShell 示例中路径和目标需换成实际值；C3 用 `esp32c3`，S31 用 `esp32s31`：

```powershell
$env:IDF_GITHUB_ASSETS = 'dl.espressif.cn/github_assets'
$env:PIP_INDEX_URL = 'https://pypi.tuna.tsinghua.edu.cn/simple'
Set-Location 'C:\esp\idf-5.5.4'
& '.\install.ps1' esp32s3
& '.\export.ps1'
```

macOS/Linux（示例目录必须已是完整 SDK）：

```sh
export IDF_GITHUB_ASSETS=dl.espressif.cn/github_assets
export PIP_INDEX_URL=https://pypi.tuna.tsinghua.edu.cn/simple
cd "$HOME/esp/esp-idf-v5.5.4"
./install.sh esp32s3
. ./export.sh
```

`IDF_GITHUB_ASSETS` **不能带 `https://`**，它仅替换工具下载中的 GitHub Release 资源。
它不代理 Git clone、子模块或项目组件。源码未下载完整时回到 EIM 的源码镜像选择，
不要反复运行安装脚本，也不要忽略子模块失败。
国内工具镜像机制见[乐鑫官方说明](https://docs.espressif.com/projects/esp-idf/en/v5.5.4/esp32s3/api-guides/tools/idf-tools.html#github-assets-mirror)。
关闭该终端即可结束本次镜像设置；后续构建用新开的、已激活的 SDK 终端。

## 5. 下载失败与离线兜底

| 失败位置 | 处理办法 |
| --- | --- |
| SDK 源码或子模块 | EIM 中切换它提供的源码镜像；确认版本和完整子模块，不修改全局 Git URL 替换 |
| 编译器等压缩包 | 使用官方国内工具镜像；核对系统/架构；保留已下载缓存，使用安装器校验，不关闭 TLS 或哈希检查 |
| Python 包 | 当前终端设置 `PIP_INDEX_URL`；镜像缺包时在新终端尝试官方 PyPI，不降低 IDF 依赖版本 |
| 项目组件 | 区别于工具链，首次构建仍可能访问组件服务或组件声明的 Git 地址；查看失败的具体依赖 |
| 代理连接被拒绝 | 检查当前终端代理是否指向已关闭的软件；不要擅自清除用户全局代理，也不要在报告中打印带密码的代理 URL |
| 下载中断 | 重试一次；仍失败则按所在层切换官方镜像/离线方式，不无限等待 |

EIM 离线包需匹配**操作系统、CPU 架构和 IDF 版本**。从官方入口下载后解压外层 ZIP，
保留安装器旁的 `.zst` 原样，打开 Offline Installation 选择它；不要手动解开 `.zst`。
Windows 可补装缺失的 Python/Git；macOS/Linux 要先满足系统依赖。
EIM 离线包当前的 Python 范围比普通安装更严格，安装前核对
[官方离线要求](https://docs.espressif.com/projects/idf-im-ui/en/latest/offline_installation.html)。

**SDK 离线安装包不等于本项目离线可编译。**本项目依赖随方案、IDF 和系统不同。
没有本项目完整、校验一致的组件缓存时，首次构建仍需要联网。
不要下载对方项目的 LVGL 包来覆盖本仓库。离线部署应在联网机器上先验证相同提交、方案和 SDK，
保存对应 lock 文件与组件缓存，按组件管理器要求恢复；不要改 lock 文件绕过校验。

可选网络检查（默认不联网）：

```sh
python tools/check_env.py --profile ai-passport-wifi --network
```

每个站点请求有超时，只测试公开下载入口。`WARN` 可能是索引或边缘节点拒绝探测，
不能代表具体包一定下载失败；也不会把网络警告当作缺少编译器。

## 6. 安装后的验收

回到**同一个已激活终端**的仓库根目录：

```sh
idf.py --version
python tools/check_env.py --profile ai-passport-wifi
python tools/check_env.py --profile ai-passport-wifi --json
```

把示例方案换成实际方案。没有 `FAIL`、显示“工具已就绪”且退出码为 0，代表工具就绪；
它不代表组件下载、构建或真机已经通过。JSON 的 `ready` 含义相同。
编译是下一步独立验收，用户要求构建时运行：

```sh
python firmware/tools/build_firmware.py ai-passport-wifi --jobs 2
```

看到 `Built ... No device flashed` 才是该次构建通过。复用 `firmware/build`，不需要全板型构建。
`python firmware/tools/build_firmware.py --check` 只查方案元数据，不能代替环境检查。
本次没有明确要求烧录时，不连接串口监视器、不写设备。

## 7. 串口与常见问题

| 问题 | 下一步 |
| --- | --- |
| 找不到 `python` | Windows 使用安装器的专用终端；macOS/Linux 试 `python3`，仍无则先装 Python |
| 找不到 `idf.py` / CMake / 编译器 | 激活对应 SDK，不要把多套工具的 PATH 手动拼在一起 |
| 已设置 IDF_PATH 仍失败 | 仅设置路径不能代替激活；当前 Python、工具和 SDK 必须匹配 |
| 激活脚本失败，提示缺少 esp-clangd 等工具 | 使用安装器修复对应 SDK 的缺项；如果已有 IDE 登记的可用构建环境，可让助手先核对并复用，不能据此宣称通用终端已激活 |
| 检查器找不到已有 SDK | 传 `--search-dir` 或 `--idf-path`，后者只检查，不会自动激活 |
| 没有端口 | 换数据线、检查本板型 USB 接口和供电；确认驱动，而不是直接擦除 |
| Linux 权限不足 | 按发行版串口组规则添加用户并重新登录，勿用全局放宽设备权限的方式解决 |
| Windows 驱动 | 区分原生 USB Serial/JTAG 与 CP210x/CH34x 桥接，按实际设备使用官方驱动，不要盲装所有驱动 |
| 端口被占用 | 让用户退出相关烧录页/监视器；不擅自关闭其他任务的程序 |

只枚举端口可运行 `python tools/check_env.py --mode flash --ports`，不会打开端口或复位设备。
检查通过后按[第一次使用](getting-started.zh-CN.md)继续。
