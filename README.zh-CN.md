# Agent Link

通过 BLE 或 WiFi 将 ESP32 设备接入 Deotaland Agent 平台。SDK 负责连接、设备 I/O、
音频流和 OTA，应用负责硬件和界面。仓库也保留已有的 Muse 固件。

[English](README.md) · [固件说明](firmware/README.md) ·
[添加板子](firmware/boards/README.md) · [SDK](firmware/components/agent_link/README.md) ·
[开发者后台 / 固件中心](https://bot.deotaland.cn/#/firmware)

## 第一次使用，从这里开始

| 你要做什么 | 入口 |
| --- | --- |
| 在网页直接烧录已发布固件 | [固件中心](https://bot.deotaland.cn/#/firmware) / [首次使用指南](guides/getting-started.zh-CN.md) |
| 电脑什么都没装，或国内下载很慢 | [Windows / macOS / Linux 环境安装，含国内镜像和离线兜底](guides/environment.zh-CN.md) |
| 不知道设备该按哪个键、连哪个热点 | [按设备查第一次操作](guides/devices.zh-CN.md) |
| 想修改固件或接入新硬件 | [固件说明](firmware/README.md) / [板级接入](firmware/boards/README.md) |

**BLE 需要尚未公开发布的 Deotaland 手机 App，请先确认已取得。**没有 App 时，
查看这款设备是否有 WiFi 方案。WiFi 可独立联网；Muse 使用自己的 App 和服务。
目录的 `supported` 表示存在源码方案，真机验收状态请看对应备注。

## 网页直接烧录（使用现成固件推荐）

[Agent Link 开发者后台的固件中心](https://bot.deotaland.cn/#/firmware)
既可以下载已发布固件，也可以**直接在网页上烧录**。
**不需要下载 GitHub 仓库、不需要安装 Python 或 ESP-IDF，也不需要自己编译。**

### 适用范围与限制

| 项目 | 要求与限制 |
| --- | --- |
| 支持的产品 | 仅限 Agent Link **已经支持、且固件中心已有对应发布包**的产品。必须匹配板型版本和 BLE/WiFi 用途，不能只看 ESP32 芯片型号。 |
| 网页能做什么 | 通过 USB 将选中的、已编译好的固件直接写入设备；网页不提供修改源码、在线编译或新板型适配。 |
| 推荐系统和浏览器 | **Windows 或 macOS 的最新版桌面 Chrome、Microsoft Edge**，需要支持 Web Serial 并允许访问设备串口。“Chrome 内核”本身不能保证可用。 |
| 其他浏览器和设备 | Safari、Firefox、手机/平板浏览器、App 内置浏览器不在本指南的网页烧录支持流程内，请使用上述桌面浏览器。 |
| USB 条件 | 使用数据线和正确接口，设备必须先被系统识别为串口；必要时安装对应 USB 驱动。 |

[Web Serial 官方说明](https://developer.chrome.com/docs/capabilities/serial#browser-support)
也列出 Linux/ChromeOS 桌面 Chrome；这不等于本项目已完成这些系统的网页烧录验收。
当前文档推荐 Windows/macOS，避免把浏览器底层能力当作固件中心的实测支持承诺。

1. 在 Windows 或 macOS 电脑上用 Chrome 或 Edge 打开固件中心，登录后台账号。
2. 使用 USB 数据线连接设备。按“硬件产品”和“芯片型号”筛选，查看更新日志，
   确认准确板型、版本及用途（如 BLE / WiFi）；USB 安装选择匹配的“完整烧录包”。
3. 点击该固件行的“烧录”，在浏览器弹窗中选择设备串口，按页面提示完成写入。
4. 等待设备重启，再按[首次使用指南](guides/getting-started.zh-CN.md)完成 WiFi 配网、
   激活和绑定 Agent，或所选 App 的配对流程。

烧录前查看设置影响：完整包可能覆盖已存 WiFi 和配对信息；无意清空设备时不要勾选整片擦除。
固件中心没有所需版本时，再让能操作本地终端的助手按下面的源码构建流程处理。

用户的 AI 助手也能引导这条流程。即使本地没有本仓库，也可以把链接和设备名称交给它：

> 我有一个 FoloToy AI Passport，想用 WiFi 语音。请引导我在 https://bot.deotaland.cn/#/firmware
> 选择对应的已发布固件，直接网页烧录并完成后续设置。我本地没有源码仓库和编译环境，优先使用网页烧录。

## 让 AI 帮你烧录

使用能操作本地文件和终端的编程助手，打开当前仓库文件夹，告诉它完整板名和用途，例如：

> 我有一个 M5Stack StopWatch，想用 WiFi 独立对话。先按仓库检查设备和环境，缺环境时按国内安装指南准备，再帮我烧录并引导首次使用。

AI 按 [AGENTS.md](AGENTS.md) 识别板子，优先引导固件中心网页烧录；需要源码构建时再准备环境，
并确认串口、验证结果。
同一块硬件有多个固件时，才需要补充选择 BLE、WiFi 或其他用途，无需知道芯片或配置符号。

请使用 USB 数据线。BLE 需要 Deotaland 手机 App；WiFi 通过设备热点配网，再到
[平台](https://bot.deotaland.cn) 输入六位激活码并绑定 Agent。
预编译固件在[固件中心](https://bot.deotaland.cn/#/firmware)。
整包烧录可能覆盖设备设置和配对信息，AI 应在写入前说明影响。

BLE 控制和 OTA 需要完成加密配对；更换手机前须在设备端重置配对。
OTA 请求须携带匹配的固件型号，旧版 App 可能需要更新。
详见[配对和 OTA 兼容性说明](firmware/components/agent_link/README.md#ble-pairing-and-ota-compatibility)。

## 自行构建

没有环境时先看[从零安装](guides/environment.zh-CN.md)。新装 S3/C3 优先 5.5.4；已有 5.5.x / 6.1.x 可复用。
先确认具体方案；下例是 AI Passport WiFi，其他设备需替换方案名。在仓库根目录执行：

```sh
python tools/check_env.py --profile ai-passport-wifi
python firmware/tools/build_firmware.py ai-passport-wifi --jobs 2
```

也可在 `firmware/` 运行 `python tools/build_firmware.py` 从菜单明确选择方案。
环境检查只验证工具，构建成功需要单独确认。构建复用 `firmware/build`，默认两个编译任务，生成应用固件、合并固件及调试文件，
不自动烧录。ESP32-S31 需要带该芯片支持的 ESP-IDF 6.1。凭据放在被 Git 忽略的
`firmware/sdkconfig.private`，含凭据的固件只在本地使用。

## 添加板子

按[板级接入指南](firmware/boards/README.md) 添加硬件、应用、固件方案和目录信息。
也可以直接向 AI 提供原理图、引脚和所需通道：

> 给这块 ESP32-S3 板子接入 WiFi 通道，原理图和引脚如下。

引脚与驱动放在 `firmware/boards/`，界面和业务放在 `firmware/apps/`，
组合由 `firmware/profiles/` 选择。BLE 和 WiFi 共用 SDK 回调及 I/O 模型。

MIT 许可证见 [LICENSE](LICENSE)。
