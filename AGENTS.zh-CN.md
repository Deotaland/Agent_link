# AGENTS.md

[English](AGENTS.md) | **简体中文**

这份文档写给使用本仓库的 AI 助手（ChatGPT、Claude、Codex、Cursor 等）。最常见的任务是帮用户把固件烧进设备、让设备上线；最后几节讲怎么改代码。人类读者请先看 [README.md](README.md)。

## 项目概况

- **Agent Link** 把 ESP32 设备接入 **Deotaland Agent 平台**。设备声明自己有哪些硬件（麦克风、扬声器、屏幕、传感器），连接由 `firmware/components/agent_link` 里的 SDK 负责。
- **平台控制台：** <https://bot.zhaojun.work>，固件中心在 <https://bot.zhaojun.work/#/firmware>。这是临时地址：deotaland.ai 域名备案完成后平台会迁过去。如果上面的地址打不开，问用户现在的地址。
- **硬件：** ESP32-S3，16 MB Flash，八线 PSRAM（除 `esp32p4-waveshare` 外的所有板子）。
- **工具链：** ESP-IDF v5.5（维护者用 v5.5.4 编译）。
- **连接方式（编译时选定）：** BLE 连 Deotaland 手机 App（默认）；WiFi 直连平台(TiDB stack)；Muse（Meta 的 Muse App 和云端）。
- **版本发布：** <https://github.com/Deotaland/Agent_link/releases> 只有发布说明，预编译固件在平台的固件中心，不在 GitHub 上。

## 规则

1. 烧录前先确认用户手里是哪块板子（见第 1 节）。型号、厂家、在哪买的、照片、蓝牙名称，有任何一样就能查。认不出来的板子不要烧。
2. 全片擦除或烧合并包（完整包）之前，先告诉用户这会清空设备上的设置：WiFi、蓝牙和 Muse 配对、设备 ID。之后要重新配网，并在平台上重新绑定。
3. 不要打印、提交或上传任何密钥：Muse SDK token、MiniMax API key、`firmware/sdkconfig` 里的任何内容。用它们编出来的固件里也带着它们，这些固件同样不能外发。
4. 如果你不能执行命令（纯对话的助手），走路径 A，一步一步带用户操作，让用户把屏幕或日志上的内容贴给你。
5. 如果你能执行命令，路径 C 最可控。除了任务需要的改动，不要动用户的代码和配置。

## 技能

`skills/` 下有三个 agent 技能：

| 技能 | 用途 |
|---|---|
| `agent-link-flash` | 认出用户的板子，征得同意后烧录，完成首次设置 |
| `agent-link-build` | 用 ESP-IDF 5.5 为指定板子编译固件，生成合并包 |
| `agent-link-debug` | 根据串口日志和现象排查设备问题 |

开工前自己安装，不要让用户动手：

- Claude Code：执行 `python tools/install_skills.py --user`，复制到 `~/.claude/skills/`；用 `--project` 则装到本仓库的 `.claude/skills/`（已被 gitignore）。手动复制文件夹也可以。
- 其他工具：把每个 `skills/<名字>` 文件夹放到该工具加载技能的位置（见它的文档）。
- 没有仓库：文件夹在 <https://github.com/Deotaland/Agent_link/tree/main/skills>。
- 不支持技能的工具（纯对话助手）：直接读 `skills/<名字>/SKILL.md` 照着做。

工具里确实能列出技能，才能说已经装好。装了技能不代表可以烧录、擦除、提交或推送，这些仍然要用户同意。

## 1. 确认板子

用户知道什么就问什么：型号、厂家、在哪买的（店铺或链接）、蓝牙名称、照片，或者长什么样。然后查：

- 有终端：`python skills/agent-link-flash/scripts/find_board.py "<用户说的话>"`。它会列出匹配的硬件、这块硬件对应的每种固件，以及编译每种固件要用的配置。
- 没有终端：看下表，或者看完整的板卡档案（含外观、烧录口、注意事项）：<https://raw.githubusercontent.com/Deotaland/Agent_link/main/skills/agent-link-flash/references/boards.json>。

<!-- boards:begin -->
| 型号 | 板子 | 厂家 | 购买 | 这个固件做什么 | `BOARD_TYPE_…` | 芯片 | 连接 | 状态 |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| RRL-01 | RoRoLee | Deotaland（德奥塔） | [rorolee.com](https://www.rorolee.com) | 电子工牌：通过 BLE 由 App 填写的名片 | `ESP32S3_WORK_BADGE` | ESP32-S3 | BLE | 支持 |
| RRL-01 | RoRoLee | Deotaland（德奥塔） | [rorolee.com](https://www.rorolee.com) | Muse 语音助手：按住 BOOT 说话，回答以文字显示 | `ROROLEE_MUSE` | ESP32-S3 | Muse | 支持 |
| RRL-01 | RoRoLee | Deotaland（德奥塔） | [rorolee.com](https://www.rorolee.com) | Agent Link 参考固件：通过 BLE 连 Deotaland App，按键和智能体对话 | `ROROLEE_S3` | ESP32-S3 | BLE | 支持 |
| WWY-01 | WOWNNY | Deotaland（德奥塔） | [rorolee.com](https://www.rorolee.com) | Muse 语音助手：按住 BOOT 说话；固件里填了 MiniMax key 时会念出回答 | `WOWNNY_MUSE` | ESP32-S3 | Muse | 支持 |
| WWY-01-old | WOWNNY（老板子） | Deotaland（德奥塔） | [rorolee.com](https://www.rorolee.com) | Muse 语音助手，和 WWY-01 相同 | `WOWNNY_OLD_MUSE` | ESP32-S3 | Muse | 老版本 |
| ESP32-S3 AIoT Basic V2 | ESP32-S3 AIoT Basic V2 开发板 | OpenJumper | [资料](https://www.openjumper.com/doc/esp32aiot-basicv2) | 摄像头画面显示在屏幕上；按键（或 App）拍照，照片通过 BLE 发给 App | `ESP32S3_GC2145_CAMERA` | ESP32-S3 | BLE | 支持 |
| Korvo 2 V3 | ESP32S3 Korvo 2 V3 开发板 | 酷世DIY（Kevincoooool） | [淘宝](https://item.taobao.com/item.htm?id=681702043224) | 触摸主屏，三个应用：摄像头预览、语音发给智能体、录音存到 TF 卡；通过 BLE 连 Deotaland App | `ESP32S3_KORVO` | ESP32-S3 | BLE | 支持 |
| Korvo 2 V3 | ESP32S3 Korvo 2 V3 开发板 | 酷世DIY（Kevincoooool） | [淘宝](https://item.taobao.com/item.htm?id=681702043224) | WiFi 直连 Deotaland 平台：热点配网，然后在控制台输入 6 位激活码；第二页可以打开摄像头 | `ESP32S3_KORVO_CLOUD` | ESP32-S3 | WiFi | 支持 |
| — | 接线示例：ESP32-S3 + ES8311（只用麦克风） | — | — | 最小实时语音转文字示例：按住按键，麦克风音频通过 BLE 发给 App | `ESP32S3_ES8311_ASR` | ESP32-S3 | BLE | 接线示例 |
| — | 接线示例：ESP32-S3 + ES8311（扬声器和麦克风） | — | — | 最小语音示例：按住 BOOT 通过 BLE 和智能体对话，回答从扬声器播放 | `ESP32S3_ES8311_VOICE` | ESP32-S3 | BLE | 接线示例 |
| ESP32-P4-WIFI6-Touch-LCD-7B | 微雪 ESP32-P4-WIFI6-Touch-LCD-7B | Waveshare（微雪） | [waveshare.com](https://www.waveshare.com/esp32-p4-wifi6-touch-lcd-7b.htm) | 只点亮屏幕，没有连接功能 | `ESP32P4_WAVESHARE` | ESP32-P4 | — | **未适配，不要烧** |
<!-- boards:end -->

- 型号对应的是硬件。几行型号相同时（RRL-01、Korvo 2 V3），先问用户想让设备做什么，再选对应那一行。
- 标着"未适配"的板子不要烧。"接线示例"不是成品，只有用户自己接线时才用，烧之前先按该板子的 `config.h` 核对接线。
- Muse 板子在 Muse App 里显示为 `MuseGadget-XXXXXX`（MAC 地址最后几个字节），不是它自己的名字。
- 两个 WOWNNY 固件的屏幕引脚不同。烧了一个屏幕不亮，就换另一个。
- 走 WiFi 需要板子里编进平台地址（`Board::Platform()`），目前只有 `korvo-cloud` 有。
- 开机日志能确认跑的是什么：`board = …, model = …, fw = …`（即档案里的 `fw_model`）和 `init: … transport=ble|wifi|muse`。
- Kconfig 符号是 `CONFIG_BOARD_TYPE_<值>`，在 menuconfig 里的位置是 `Agent Link Device → Board Type`。
- 板卡档案在 `firmware/boards/<目录>/config.json`，每种板子一条。上表和技能自带的 `boards.json` 都由 `python tools/board_catalog.py` 从它生成。

## 2. 选一条路

| 路径 | 需要 | 适用场景 |
|---|---|---|
| A. 固件中心的网页烧录 | 电脑上的 Chrome、Edge 或 Opera；平台账号 | 用户不是开发者，或者你不能执行命令 |
| B. 预编译固件 + esptool | Python | 手上有 `.bin`，也有终端 |
| C. 从源码编译 | ESP-IDF v5.5 | 这块板没有预编译固件，或者要改配置、改代码 |

固件中心里这块板显示"未找到可用固件版本"时，走路径 C。

## 路径 A：网页烧录

1. 用电脑上的 Chrome、Edge 或 Opera 打开 <https://bot.zhaojun.work/#/firmware>。网页烧录依赖 Web Serial，Safari、Firefox 和手机都不行。登录。
2. 按产品或芯片筛选，选一个版本。要选**完整烧录包**。**OTA 差分升级包**是给空中升级用的，不能通过 USB 烧。
3. 点**烧录**。在烧录窗口里点**拉取云端固件**（或者选择本地 .bin 文件）。
4. 用 USB 连上设备，点**连接设备串口**，选它的串口。波特率默认 921600，失败就换 115200。烧录器会核对芯片型号，不符会自动中止。
5. **烧录起始地址**会按文件名自动填好：文件名带 `merged`、`all` 或 `full` 的合并包是 `0x0`，只有 App 的是 `0x10000`。保持不变即可。
6. 除非用户就是要一台干净的设备，否则不要勾**烧录前全片擦除**（见规则 2）。自动复位保持勾选。点**开始写入设备**。
7. 找不到串口或一直卡在连接：按住 BOOT 键再插 USB（或按住 BOOT 时点一下 RESET），松开 BOOT，重试。

只烧 App 的固件在做过 OTA 升级的设备上不会生效，见"常见问题"。烧完接着看"烧录之后"。

## 路径 B：用 esptool 烧录

安装：`pip install esptool`（ESP-IDF 环境里已经带了）。下面带下划线的命令在 esptool v4 和 v5 都能用，v5 也认 `write-flash` / `erase-flash`。

串口：Windows 是 `COM3` 这类（设备管理器 → 端口）；macOS 是 `/dev/cu.usbmodem*`；Linux 是 `/dev/ttyACM0`（芯片自带的 USB）或 `/dev/ttyUSB0`（USB 转串口芯片），用户要在 `dialout` 组里（Arch 上是 `uucp`）。

```bash
# 合并包，从 0x0 写入。会清空设置和配对（规则 2）。
python -m esptool --chip esp32s3 -p PORT -b 460800 write_flash 0x0 firmware-merged.bin

# 保留设置：写入编译产物里的四个文件，和 idf.py flash 写的一样。
python -m esptool --chip esp32s3 -p PORT -b 460800 write_flash --flash_mode dio --flash_freq 80m --flash_size 16MB \
    0x0 bootloader.bin 0x8000 partition-table.bin 0xe000 ota_data_initial.bin 0x10000 agent_link.bin

# 整片擦除，rorolee-muse 的中文字库也会被擦掉。
python -m esptool --chip esp32s3 -p PORT erase_flash
```

Flash 分区（`firmware/partitions.csv`，16 MB）：

| 地址 | 大小 | 内容 |
|---|---|---|
| `0x0` | | bootloader |
| `0x8000` | | 分区表 |
| `0x9000` | 20 KB | `nvs`：WiFi、配对、设备 ID |
| `0xE000` | 8 KB | `otadata`：从哪个 App 分区启动 |
| `0x10000` | 2 MB | `ota_0`：App |
| `0x210000` | 2 MB | `ota_1`：第二个 App 分区，OTA 升级用 |
| `0x410000` | 8 KB | `nvs_keys`、`phy_init` |
| `0x412000` | 11.4 MB | `anim_pack`：rorolee-muse 的中文字库放在这里 |
| `0xF84000` | 496 KB | `model` |

## 路径 C：从源码编译

安装 ESP-IDF v5.5：<https://docs.espressif.com/projects/esp-idf/zh_CN/v5.5.4/esp32s3/get-started/index.html>。Windows 用 ESP-IDF 安装器，之后从它的"ESP-IDF PowerShell"快捷方式打开终端。macOS 和 Linux 先运行 `./install.sh esp32s3`，以后每开一个新终端都要先执行 `. ./export.sh`。

```bash
git clone https://github.com/Deotaland/Agent_link.git
cd Agent_link/firmware
```

交互式选择板子和连接方式：

```bash
idf.py set-target esp32s3        # esp32p4-waveshare 用 esp32p4
idf.py menuconfig                # Agent Link Device → Board Type、Transport backend
idf.py reconfigure
idf.py build
```

不用 menuconfig（适合 agent）：把选择写进一个**放在仓库外面**的文件，例如：

```
CONFIG_BOARD_TYPE_WOWNNY_MUSE=y
CONFIG_AGENT_LINK_TRANSPORT_MUSE=y
```

然后：

```bash
idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;/绝对路径/board.defaults" set-target esp32s3
idf.py reconfigure
idf.py build
```

- 连接方式的取值：`AGENT_LINK_TRANSPORT_BLE`、`AGENT_LINK_TRANSPORT_WIFI`、`AGENT_LINK_TRANSPORT_MUSE`。
- **选好板子后一定要执行一次 `idf.py reconfigure`。** 顶层 `CMakeLists.txt` 只有在配置时发现板子已经选好，才会把这块板专属的设置（PSRAM、RTC 时钟、内存放置、TCP 缓冲）补进 `sdkconfig`，日志里会出现 `agent_link: patched sdkconfig for BOARD_TYPE_…`。漏了这一步，Muse 板子会内存不够。
- `set-target` 会重新生成 `sdkconfig`，旧的存为 `sdkconfig.old`，里面的密钥要重新填。密钥也可以写进上面那个文件，所以它必须放在仓库外面。
- Muse 板子要设置 `CONFIG_GADGET_SDK_TOKEN`（`Component config → Muse gadget → Muse Gadgets SDK token`，在 <https://gadgets.muse.ai/settings/sdk-tokens> 申请）。不设编译会警告，等 Muse 开始强制校验 token 后就配不上对了。可选：`CONFIG_MUSE_TTS_MINIMAX_KEY`，填 MiniMax API key 后设备会把回答念出来。

烧录并看日志：

```bash
idf.py -p PORT flash monitor     # 按 Ctrl+] 退出监视器
```

`idf.py flash` 写入 bootloader、分区表、otadata 和 App，NVS 里的设置和配对都会保留。要生成一个单文件固件发给别人：`cd build && python -m esptool --chip esp32s3 merge_bin -o merged.bin @flash_args`。

## 烧录之后：设置设备

### BLE 板子

设备用自己的名称广播（档案里的 `ble_name`），用 Deotaland 手机 App 配对。App 还没有公开发布，先问用户手上有没有。

### WiFi（korvo-cloud）

1. 屏幕显示 "Set up WiFi"。用手机连开放热点 `KorvoCloud-XXXX`，会自动弹出配网页面（没弹出就打开 <http://192.168.4.1>），选择 WiFi、输入密码。
2. 屏幕显示 "Activation code" 和 6 位数字。这个码 10 分钟内有效，只能用一次。
3. 在平台上：**设备管理 → 添加设备 → 6位码快速绑定**，输入绑定码，可以填设备名，选择智能体。
4. 屏幕显示 "Online" 就好了。显示 "Already bound" 说明设备已经被绑定过，要先在平台上解绑；显示 "No agent" 说明还没给它分配智能体。

平台地址是编进固件的（`boards/korvo-cloud/config.h` 里的 `CLOUD_BASE_URL`、`CLOUD_PRODUCT_ID`）。平台迁到 deotaland.ai 后，这个固件要重新编译。

### Muse 板子（WOWNNY、RoRoLee Muse）

1. 在 Muse App 里：Settings → Devices → Developer mode，然后 Add Device。设备显示为 `MuseGadget-XXXXXX`。
2. 设备要求确认时，按一下 BOOT。
3. App 把 WiFi 信息发给设备，设备重启后连上用户的 Muse。
4. 按住 BOOT 说话，松开发送。RoRoLee Muse 用文字显示回答；WOWNNY 屏幕上没有文字，只有形象动画。固件里填了 MiniMax key 的话，两者都会把回答念出来。

在中国大陆，不给整个网络加代理就连不上 Muse 的服务器（在路由器上开代理，或者电脑代理开 TUN 模式再共享热点给设备）。只在电脑上开代理对设备没用。

按键：

- WOWNNY：按住 BOOT 说话；VOL+ / VOL- 调音量；按住任一音量键约一秒打开设置菜单（Volume、Speaker、Wi-Fi、Reset、Exit），BOOT 确认。Reset 会清除 Muse 配对。
- RoRoLee Muse：按住 BOOT 说话；VOL+ / VOL- 调音量；按住 VOL- 6 秒清除配对。
- RoRoLee Muse 要显示中文，需要往 `anim_pack` 烧一个字库，见 `firmware/boards/rorolee-muse/font/README.md`。

## 常见问题

| 现象 | 处理 |
|---|---|
| 找不到串口 | 换一根数据线（很多线只能充电），换个 USB 口。用档案里 `flash_port` 写的那个口（Korvo 用左边的 USB-C）。板子上是 CH340、CH9102 或 CP210x 转串口芯片的，Windows 上要装对应驱动。 |
| `Failed to connect`，或一直卡在连接 | 进下载模式：按住 BOOT 插 USB（或点一下 RESET），再松开 BOOT。换 115200 波特率。关掉其他占着串口的程序，比如串口监视器。 |
| 芯片型号不符 | 固件是给别的芯片的（ESP32-S3 和 ESP32-P4）。 |
| 烧完还是旧固件 | 设备做过 OTA 升级，从 `ota_1` 启动。把 `ota_data_initial.bin` 也写到 `0xe000`（`idf.py flash` 会写），或者擦掉 otadata：`python -m esptool -p PORT erase_region 0xe000 0x2000`。 |
| 反复重启，或屏幕不亮 | 板子选错了。WOWNNY 换另一个硬件版本的固件试试。看日志：`idf.py -p PORT monitor`，或者任意串口工具，115200 波特率。 |
| WOWNNY（WWY-01）屏幕亮起后日志就断了 | 正常。GPIO43（UART0 TX）用作屏幕复位，之后日志只从芯片自带的 USB 口输出。 |
| WOWNNY 老板子开机时屏幕黑一秒 | 正常，固件在给屏幕重新上电。 |
| WiFi："Reconnecting" 或 "Platform error" | 检查网络，以及这个网络能不能访问平台地址。 |
| Muse：一直连不上 | 访问 Meta 服务器的网络问题，见上文。 |
| 编译：`sdkconfig.h` 里没有某个 `CONFIG_…` | 执行 `idf.py reconfigure`，新加的 Kconfig 选项需要这一步。 |
| 编译：App 太大 | App 分区 2 MB，Muse 固件已经用了约 92%。 |

## 仓库结构

```
Agent_link/
├── README.md、AGENTS.md、CLAUDE.md
├── skills/                    agent 技能；agent-link-flash/references/boards.json 是生成的板卡档案
├── tools/                     board_catalog.py（校验并重新生成板卡档案）、install_skills.py
├── app/                       手机 App（尚未发布）
└── firmware/                  ESP-IDF 工程，idf.py 在这里执行
    ├── CMakeLists.txt         按板子修补 sdkconfig、PROJECT_VER
    ├── partitions.csv
    ├── sdkconfig.defaults     以及 .esp32s3 / .esp32p4
    ├── main/                  app_main.cpp、board.h（Board 接口）、Kconfig.projbuild（板子和连接方式菜单）
    ├── boards/<board>/        每块板一个目录：config.h、config.json（含板卡档案）、<board>.cc
    ├── boards/common/         多块板共用的代码：屏幕、音频编解码、电量计、触摸、摄像头
    └── components/
        ├── agent_link/        SDK：核心、BLE / WiFi / Muse 三种传输、OTA、平台客户端
        ├── muse_gadget/       Meta 的 Muse Home Link；它和 noise_core 只在选了 Muse 时才参与编译
        ├── minimp3/           Muse 语音播报用的 MP3 解码器
        └── esp_lcd_gc9d01/    GC9D01 圆屏驱动
```

改代码前先读：`firmware/README.md`、`firmware/boards/README.md`（Board 接口、新增板子）、`firmware/components/muse_gadget/README.md`。

## 改代码的规矩

- 改完先编译通过再交付。改了 `components/` 或 `boards/common/`，所有用到它的板子都要编译一遍。
- 板子不知道自己走的是哪种连接。板子代码里不要按 BLE / WiFi / Muse 分支，连接状态统一通过 `agent_link_status_t` 交给板子。
- SDK 回调跑在传输层的任务里（NimBLE host 栈只有 6 KB），回调里少做事。
- SDK 的公开函数在 `agent_link_init()` 之前也必须能安全调用，因为板子对象先于它构造。
- 新增 Kconfig 选项后要执行 `idf.py reconfigure`。组件的 `REQUIRES` 不能随 Kconfig 变化，用"组件注册为空"的方式处理。
- 注释简短直白，代码注释里不放链接，也不引用文档。
- 密钥只放在 `sdkconfig` 里（已被 gitignore），绝不放进 `sdkconfig.defaults`。
- 多块板共用的代码放在 `firmware/boards/common/`，不要放进某块板的目录。
- 新增板子：按 `firmware/boards/README.md` 里的 "Adding a board" 做，包括在 `config.json` 里写好板卡档案。然后执行 `python tools/board_catalog.py`，它也会检查 Kconfig 里的每种板子是否都有档案。
- 提交前，`python tools/board_catalog.py --check` 必须通过。
