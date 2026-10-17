# 固件方案

日常切换：在 SDK 配置编辑器 / `menuconfig` 中打开 **Agent Link Device → Firmware profile**，从列表选择，保存后直接构建。可选项按当前目标芯片限制。

需要切换芯片时，在 ESP-IDF 5.5 终端从 `firmware` 目录运行，按列表序号选择：

```powershell
python tools/build_firmware.py
```

每个方案固定板型、应用和接入后端。Korvo 的两种应用共用硬件，但分别构建：

| 方案 | 应用 | 接入后端 |
| --- | --- | --- |
| `korvo-ble` | 摄像头、语音、TF 卡录音 | Deotaland 手机 App |
| `korvo-wifi` | 平台激活、会话管理、实时语音 | WiFi 直连平台 |
| `m5stack-stopwatch-ble` | 黄键按住对讲、圆屏状态 | Deotaland 手机 App |
| `m5stack-stopwatch-wifi` | 配网激活、触摸管理会话、实时语音 | WiFi 直连平台 |
| `ai-passport-ble` | OK 键对讲、屏幕状态 | Deotaland 手机 App |
| `rorolee-muse` / `wownny-muse` / `wownny-old-muse` | Muse 语音助手 | Muse App 和云端 |

自动化构建仍可使用 `python tools/build_firmware.py korvo-ble`，查看全部方案用 `--list`。ESP32-P4 尚未适配，不提供构建方案。Muse 会同时使用 BLE 配对和 WiFi 联网。

## 配置

加载顺序：公共 `sdkconfig.defaults` → 芯片配置 → 后端配置 → 专用方案配置 → 本地 `sdkconfig.private`。

- `profiles.json` 声明组合，构建工具与板型目录的 `config.json` 交叉校验。
- 每种方案有自己的短配置；PSRAM、RTC、缓冲区等要求可直接查看。
- 首次迁移备份原配置为 `sdkconfig.before-profile`，自动保留凭据及关联选项到 `sdkconfig.private`。二者均被 Git 忽略。备份只创建一次。
- 修改凭据请编辑 `sdkconfig.private`。它只接受令牌、密钥、测试会话及相关选项，不接受板型或内存配置。
- `sdkconfig` 是生成结果。同一方案继续增量构建；保存新的方案后，下一次构建会明确提示切换并重新生成配置，不继承其他方案的参数。凭据来自 `sdkconfig.private`，其他自定义选项需重新设置。
- 仅切换芯片时由构建工具清理原 `build` 缓存。请用交互列表完成跨芯片切换。
- `menuconfig` 可调整其他选项，板型和后端由所选方案派生。单独修改方案规定的内存等设置，构建会报错。

```powershell
python tools/build_firmware.py korvo-ble --configure-only
python tools/build_firmware.py korvo-ble --reset-config
python tools/build_firmware.py --check
```

默认复用 `firmware/build`，并行度为 2。成功后生成 `build/agent_link.bin`、`build/merged.bin` 和调试文件；不会自动烧录。带凭据的产物仅供本地使用。

支持 ESP-IDF 5.5.x 和 6.1.x。激活所需环境后使用同一命令；切换 SDK 路径或版本时自动清理
原构建缓存，并根据新 SDK 重新生成配置。两套环境不会共用生成的 CMake 配置和目标文件。

## 内存

S3 方案使用 Octal PSRAM。BLE 主机缓冲放在 PSRAM，控制器和 DMA 仍由驱动分配内部 RAM；保留原缓冲数量。AI Passport 没有 PSRAM，继续使用 C3 的内部 RAM 和小缓冲配置。

网络方案另将 LVGL 静态堆放入 PSRAM，保留 WiFi/TLS 的专用配置。这些设置不再由 CMake 追加进 `sdkconfig`。

## 新增方案

1. 按 `boards/README.md` 添加板型及目录。
2. 在 `main/Kconfig.profiles` 添加方案选项，新建专用 `.defaults` 设置 `CONFIG_AGENT_LINK_PROFILE_<方案>=y`、RTC 等要求；板型和后端由 Kconfig 根据方案派生。
3. 在 `profiles.json` 登记芯片、板型和配置文件，运行 `--check`。
4. 构建并实机验收后再将组合标为支持。

Korvo 的应用目录为 `apps/korvo-ble` 和 `apps/korvo-wifi`，硬件共用 `boards/korvo`。设备型号仍为 `KORVO` 和 `KORVO-CLOUD`，保持 OTA 和平台兼容。

StopWatch 的应用分别在 `apps/m5stack-stopwatch-ble` 和 `apps/m5stack-stopwatch-wifi`，共用 `boards/m5stack-stopwatch` 的硬件初始化，型号均为 `M5-STOPWATCH`。
WiFi 版连接 `M5StopWatch-XXXX` 热点配网并用激活码认领设备，在平台上绑定 Agent。触摸主页的 Start/End call、Sessions、Settings 操作通话和会话；回复区可上下滑动。会话每页显示三条，Prev/Next 翻页，点选后返回主页；Settings 可新建或删除当前会话。删除和 Reset device activation 均需再次确认，重置保留 WiFi，重新激活前需在平台解绑。黄键减音量、蓝键加音量，每次 5%，范围 0–100，长按连续调节；独立电源键保持原有功能。通话时采用与 Korvo 相同的扬声器回声静音保护，不支持语音打断。空闲约 30 秒降低亮度，首次触摸唤醒后再次点击操作。触摸错误和跨页点击会被取消；网络请求期间锁定操作，物理音量键仍可使用。

两块 WiFi 板的业务一致：Agent 只在平台选择和绑定，设备从认证响应 `data.agent` 取得当前绑定信息，每 60 秒刷新；换绑后停止旧通话并清除旧会话选择。设备只管理此 Agent 的会话列表、新建、选择、删除，以及开始/结束语音；删除保留二次确认。会话列表支持游标翻页。设备只使用激活下发的 `auth_key`，不需要用户登录令牌；未激活时先通过 6 位激活码认领。

2026-10-17 实测：设备认证、读取绑定 Agent、会话列表、已有会话的语音授权、WSS/TLS 与 `session.ready`、结束授权均通过。Agent/设备列表是账户接口，设备不再调用。新建会话仍因上游发布模板不可用返回业务码 1002；真实删除、实机配网、录音播放及硬件交互尚未验证。

`profiles.json` 中：

- `board`：板型目录及应用配置；`board_type` 保留已有 Kconfig 标识。
- `app`：`apps/` 下的 UI 和业务实现。
- `hardware`、`hardware_sources`：硬件目录和专用驱动。
- `drivers`：从 `boards/common/` 选择的公共驱动文件。
- `ui`：`lvgl`、`raster` 或 `none`；`backend` 固定接入方式。

CMake 在载入 ESP-IDF 前读取方案，依赖收集阶段也能得到同一份信息，组件依赖不再依据尚未载入的 Kconfig 判断。
托管依赖的版本仍由 Component Manager 锁定；底层库的间接依赖由 ESP-IDF 决定。

StopWatch WiFi 硬件对照：M5 官方 UserDemo 提交 `83de653c0be4e51e82079bee2386b90fb0538a25`。
CST820 使用 0x15、100 kHz，从 0x00 读取 7 字节并按 down/contact/up 解码；不套用 CST816 的休眠寄存器。
坐标不交换、不翻转；显示 RAM 的 x-gap=6 不加到触摸坐标。触摸复位低 10 ms、高后等待 50 ms。
IO 扩展支持 0x4F/0x6F，关闭 I2C 休眠并有界检查屏幕供电。WiFi 播放同时控制 IOE10 与 GPIO14，
切换时先释放 GPIO14，兼容可选连线；音量使用官方 codec-dev 默认曲线。
只有主机和构建验证，尚无 StopWatch 实机触摸、配网、录放音或功耗验收；不保证首次烧录的硬件结果。
主机回归：`python tests/test_stopwatch_controls.py`；平台 API 回归见 `tests/test_platform_api.py`。
