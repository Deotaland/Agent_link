[English](SKILL.md) | **简体中文**

# 排查 Agent Link 设备问题

> 技能名 `agent-link-debug`。工具读取的是 [SKILL.md](SKILL.md) 里的英文 `name` / `description`，本文件是中文说明，内容与之一致。

除非用户要求修复，否则只读不改。现象对照表见 AGENTS.md"常见问题"。

## 1. 收集信息

- **用户看到的现象**，以及是哪块板子：用 `agent-link-flash` 的 `scripts/find_board.py`，把用户知道的信息传进去。
- **跑的是什么固件**：开机日志里的 `board = …, model = …, fw = …` 和 `init: … transport=…`。
- **串口日志**：用 `idf.py -p PORT monitor`（`build/agent_link.elf` 和烧进去的固件一致时会自动解析回溯），或者任意串口工具，115200 波特率。
  - 打开串口可能会让设备重启，在意的话先问用户。
  - WOWNNY（WWY-01）屏幕启动后，日志只从 USB 口输出。
- **连接状态**：以 `[link]` 开头的行，就是设备上显示的标题和提示。原样转告用户。

## 2. 常见原因

| 现象 | 检查什么 |
|---|---|
| 找不到串口或连不上 | 数据线、下载模式（按住 BOOT 重新插）、插对口（Korvo 用左边的 USB-C）、串口是否被别的程序占用 |
| 烧完还是旧固件 | 做过 OTA 后 `otadata` 指向 `ota_1`：把 `ota_data_initial.bin` 写到 `0xe000`，或者擦掉这一段 |
| 烧完就反复重启 | 板子类型或芯片选错；板子的 PSRAM / Flash 不符合要求（16 MB Flash、八线 PSRAM） |
| 屏幕不亮 | 固件和板子不对应（WOWNNY 换另一个硬件版本试试）；或者是标着 `not-adapted` 的板子 |
| WiFi："Platform error"、"Reconnecting"、"Already bound"、"No agent" | 网络、平台能否访问、控制台里的绑定状态 |
| Muse 一直连不上 | 访问 Meta 服务器的网络（中国大陆不给整个网络加代理就连不上） |
| `muse_tts: no speech: HTTP …` | MiniMax key 或额度问题；状态码后面是 MiniMax 返回的内容 |
| `Guru Meditation` / 回溯 | 只能用对应的 ELF 解析：`xtensa-esp32s3-elf-addr2line -pfiaC -e build/agent_link.elf <地址>` |
| 编译：缺某个 `CONFIG_…` | 改了 Kconfig 之后执行 `idf.py reconfigure` |

没有和烧进去的固件对应的 ELF，就如实说明，不要猜源码行号。

## 3. 汇报

汇报这几项：
- 日志里看到了什么，原样引用相关的行；
- 最可能的原因，以及有多大把握；
- 建议怎么修；
- 哪些没能确认。

改代码、重新编译、烧录，都要先征得用户同意。
