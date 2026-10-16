# Agent Link 固件

[English](README.md) | **简体中文**

Agent Link 把嵌入式设备接入 Deotaland Agent 平台。可复用的 SDK 在
[`components/agent_link`](components/agent_link/README.md)，本工程提供板级驱动、应用和固件方案。

## 构建

使用 ESP-IDF 5.5.x，当前维护环境为 5.5.4。激活环境后，在本目录运行：

首次拉取不需要已有的 `build/`、`sdkconfig`、私有配置或测试文件。工具自动生成所选方案的配置，
ESP-IDF 按仓库中的组件清单和 `dependencies.lock` 下载依赖，首次编译需联网。

```powershell
python tools/build_firmware.py
```

从终端列表按序号选择即可，不用输入方案名。SDK 配置编辑器 / `menuconfig` 中也可以直接打开
**Agent Link Device → Firmware profile** 下拉选择，保存后构建。可选项按当前芯片限制，
跨芯片切换请用终端列表。自动化仍可指定 `python tools/build_firmware.py korvo-ble`。

每个方案固定板型、应用、目标芯片和接入后端。Korvo 的手机 App 固件是 `korvo-ble`，
平台触屏和实时语音固件是 `korvo-wifi`。仅提供已登记的组合；ESP32-P4 尚未适配。

默认复用 `build/`，并行度为 2，生成 `agent_link.bin`、`merged.bin` 和对应调试文件，
不会自动烧录。首次配置后，可以继续通过 IDE 编译同一方案。

```powershell
python tools/build_firmware.py korvo-ble --configure-only
python tools/build_firmware.py korvo-ble --reset-config
python tools/build_firmware.py --check
```

配置按公共、芯片、后端、专用方案、本地私有配置的顺序加载。首次迁移将原配置备份为
`sdkconfig.before-profile`，凭据及关联选项保留到 `sdkconfig.private`。后续修改凭据请编辑
后者；不要提交这些文件，也不要分发含凭据的固件。

保存新方案后，下一次构建会提示切换并重新生成 `sdkconfig`；跨芯片时由工具清理原构建缓存。
同一方案增量构建。CMake 调用配置工具处理方案切换，再校验结果，不再追加设置。
详见[固件方案说明](profiles/README.md)。

## 接入后端

| 后端 | 连接方式                                             |
| ---- | ---------------------------------------------------- |
| BLE  | 经 Rorolee 手机 App，GATT 传命令，L2CAP 传音频和图像 |
| WiFi | 热点配网、平台激活、MQTT 命令、实时语音              |
| Muse | Muse App 通过 BLE 配对，再通过 WiFi 连接 Muse 服务   |

SDK 通过 `agent_link_status_t` 提供连接状态，硬件回调接收 PCM 音频、文字或外设命令。
SDK 支持多种后端，不代表每个应用都已经适配所有后端。

## 目录

```text
firmware/
  profiles/                  固件组合和短配置
  tools/build_firmware.py    单个方案的配置、校验、编译和合并
  cmake/                    构建集成
  main/                     app_main 和方案选择菜单
  apps/<app>/               UI、会话、录音及 SDK 回调
  apps/common/              Application 应用接口
  boards/<board>/           引脚、硬件初始化和板型目录
  boards/common/            共用屏幕、音频、相机及外设驱动
  components/board_support/ 只编译所选硬件驱动
  components/ui_lvgl/       按需接入 LVGL
  components/agent_link/    核心、音频、OTA 及所选接入后端
  components/muse_gadget/   Muse SDK 及移植层
```

`profiles/profiles.json` 统一选择应用、硬件、驱动、UI 和后端。Korvo BLE/WiFi 共用
`boards/korvo/`，各自的界面、音频流程放在 `apps/korvo-ble` 和 `apps/korvo-wifi`。
SDK 仅编译所选后端，未启用功能保留接口并返回不支持。公共 SDK 接口、设备型号和分区布局保持兼容。
新增硬件见 [boards/README.md](boards/README.md)，同时登记固件方案。多个板子共用的代码放在
`boards/common/`。

## 单独使用 SDK

将 `components/agent_link/` 放入 ESP-IDF 工程的 `components/`，添加组件依赖，包含
`agent_link.h` 并注册硬件能力和回调。麦克风数据通过 `agent_link_stream_open/write/close`
上传，硬件输出通过回调接收。回调可能在接入任务执行，耗时操作请排队处理。

接口与授权说明见[组件文档](components/agent_link/README.md)和
[实时语音文档](components/agent_link/REALTIME.md)。

## 许可证

MIT，见 [LICENSE](../LICENSE)。第三方组件保留各自许可证。
