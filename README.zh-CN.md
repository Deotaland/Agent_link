# Agent Link

通过 BLE 或 WiFi 将 ESP32 设备接入 Deotaland Agent 平台。SDK 负责连接、设备 I/O、
音频流和 OTA，应用负责硬件和界面。仓库也保留已有的 Muse 固件。

[English](README.md) · [固件说明](firmware/README.md) ·
[添加板子](firmware/boards/README.md) · [SDK](firmware/components/agent_link/README.md)

## 让 AI 帮你烧录

用编程助手打开仓库，告诉它板子名称即可，例如：

> 我有一个 M5Stack StopWatch，帮我烧录 Agent Link 固件。

AI 按 [AGENTS.md](AGENTS.md) 识别板子、选择固件方案、构建或查找固件、确认串口并检查启动日志。
同一块硬件有多个固件时，才需要补充选择 BLE、WiFi 或其他用途，无需知道芯片或配置符号。

请使用 USB 数据线。BLE 需要 Deotaland 手机 App；WiFi 通过设备热点配网，再到
[平台](https://bot.deotaland.cn) 输入六位激活码并绑定 Agent。
预编译固件在[固件中心](https://bot.deotaland.cn/#/firmware)。
整包烧录可能覆盖设备设置和配对信息，AI 应在写入前说明影响。

## 自行构建

激活 ESP-IDF 5.5.x 或 6.1.x 后执行：

```sh
git clone https://github.com/Deotaland/Agent_link.git
cd Agent_link/firmware
python tools/build_firmware.py
idf.py -p PORT flash monitor
```

从菜单选择方案。构建复用 `firmware/build`，默认两个编译任务，生成应用固件、合并固件及调试文件，
不自动烧录。ESP32-S31 需要带该芯片支持的 ESP-IDF 6.1。凭据放在被 Git 忽略的
`firmware/sdkconfig.private`，含凭据的固件只在本地使用。

## 添加板子

按[板级接入指南](firmware/boards/README.md) 添加硬件、应用、固件方案和目录信息。
也可以直接向 AI 提供原理图、引脚和所需通道：

> 给这块 ESP32-S3 板子接入 WiFi 通道，原理图和引脚如下。

引脚与驱动放在 `firmware/boards/`，界面和业务放在 `firmware/apps/`，
组合由 `firmware/profiles/` 选择。BLE 和 WiFi 共用 SDK 回调及 I/O 模型。

MIT 许可证见 [LICENSE](LICENSE)。
