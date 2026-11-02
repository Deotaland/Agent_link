# 按设备查第一次操作

[English](devices.md) · [完整首次使用](getting-started.zh-CN.md) · [安装环境](environment.zh-CN.md)

表中操作以当前源码为准，不代表每一款设备都已完成真机验收。
目录中的 `supported` 表示有源码方案。选择前用完整型号核对[目录](../AGENTS.md#board-catalog)及其备注；
表中的 WiFi 方案先完成配网、激活、绑定 Agent，再做首次操作。BLE 先取得 Deotaland App。

| 硬件与方案 | 连接入口 | 第一次操作与验收 |
| --- | --- | --- |
| AI Passport：`ai-passport-ble` | App 搜索 `AIPassport` | 按住 OK 说话、松开发送，确认喇叭回复 |
| AI Passport：`ai-passport-wifi` | `AIPassport-XXXX` | OK 开始/结束通话；长按 OK 选会话，UP/DOWN 调音量 |
| StopWatch C152：`m5stack-stopwatch-ble` | App 搜索 `M5StopWatch` | 按住黄键说话；蓝键调音量，长按蓝键 6 秒重置配对。触摸未使用 |
| StopWatch C152：`m5stack-stopwatch-wifi` | `M5StopWatch-XXXX` | 触摸 Start/End；Sessions 选择会话；黄/蓝键减/加音量。目录仍注明真机验收待完成 |
| 微雪 1.75 非 C 版（16 MB）：`waveshare-amoled-175-wifi` | `Amoled175-16M-XXXX` | BOOT 开始/结束通话；与 C 版引脚、固件型号不同，不能混刷；真机验收待完成 |
| 微雪 1.75**C**（32 MB）：`waveshare-amoled-175c-wifi` | `Amoled175-XXXX` | 短按 BOOT 开始/结束通话；左滑进应用，设置选语言；交友双机验收待完成 |
| 微雪 3.97 墨水屏：`waveshare-epaper-397-wifi` | `WavePaper397-XXXX` | 长按中键选会话；主页短按中键通话，UP/DOWN 调音量，BOOT 返回/结束；等待刷新，真机验收待完成 |
| 墨纸坊 paper V2：`paper-s31-wifi` | `PaperS31-XXXX` | KEY1/2/3 为上/下/确认，BOOT 返回；Voice 中选会话。长按 KEY1 通话、KEY2 停音，KEY3 请求刷新。六色屏刷新较慢，真机验收待完成 |
| Korvo：`korvo-ble` | App 搜索 `KORVO` | 进入本地 Voice 应用发起语音，或验证 Camera 预览；先确认 240×280 ST7789 屏幕变体 |
| Korvo：`korvo-wifi` | `KorvoCloud-XXXX` | 进入语音页选/建会话并开始通话；相机是本地预览，不应期待 Agent 图片上传 |
| RoRoLee RRL-01：`rorolee-ble` | App 搜索 `ROROLEE_S3` | 按住 BOOT 说话、松开发送，验证回复；与工牌方案用途不同 |
| RoRoLee RRL-01：`work-badge-ble` | App 搜索 `ROROLEE_XXXXXX` | 从 App 设置姓名/职位等字段，确认屏幕显示；此方案用于工牌，不是语音款 |
| RoRoLee：`rorolee-muse` | Muse App 添加 `MuseGadget-XXXXXX` | BOOT 确认配对；按住 BOOT 录音、松开发送，查看回复；VOL- 长按 6 秒重置 Muse 配对 |
| WOWNNY：`wownny-muse` | Muse App 添加 `MuseGadget-XXXXXX` | BOOT 确认配对，按住录音、松开发送；有 MiniMax 配置才有相应语音回复 |
| WOWNNY 旧版：`wownny-old-muse` | 同上 | 确认确实是旧板再选；新旧板音量键有差异 |
| AIoT Basic V2：`gc2145-camera-ble` | App 搜索 `GC2145_CAMERA` | 先看 LCD 预览，再用按钮或 App 拍照，确认 App 收到；不是默认语音设备 |
| OJBadge：`ojbadge-wifi` | 本地交友，无平台激活 | SW2 短按寻找/打招呼，长按 1 秒回主页；两端核对验证码后确认。无云端语音；真机和双机验收待完成 |
| 自接 ES8311：`es8311-asr-ble` | App 搜索 `ROROLEE_ES8311_ASR` | 核对引脚后按住说话键，验证转写；只有麦克风路径 |
| 自接 ES8311：`es8311-voice-ble` | App 搜索 `ROROLEE_ES8311` | 核对引脚后按住 BOOT 说话、松开发送，验证喇叭回复 |
| 微雪 P4 七寸屏 | 无可用方案 | 当前未适配，不烧录 S3/S31 固件 |

`XXXX`/`XXXXXX` 是设备后缀，实际名称可能因设备不同。平台绑定 Agent 后还需要选会话的方案，
按设备显示完成选择，不要把等待会话当作配网失败。

硬件细节：[AI Passport](../firmware/boards/ai-passport/README.md)、
[微雪 1.75 非 C 版](../firmware/boards/waveshare-amoled-175/README.md)、
[微雪 1.75C](../firmware/boards/waveshare-amoled-175C/README.md)、
[微雪 3.97](../firmware/boards/waveshare-epaper-397/README.md)、
[paper V2](../firmware/boards/paper-s31/README.md)、
[Korvo BLE](../firmware/boards/korvo-ble/README.md)、
[Korvo WiFi](../firmware/apps/korvo-wifi/README.md)、[工牌](../firmware/boards/work-badge/README.md)。
