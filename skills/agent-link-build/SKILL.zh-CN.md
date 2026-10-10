[English](SKILL.md) | **简体中文**

# 编译 Agent Link 固件

> 技能名 `agent-link-build`。工具读取的是 [SKILL.md](SKILL.md) 里的英文 `name` / `description`，本文件是中文说明，内容与之一致。

在用户的仓库里做；没有仓库就 clone <https://github.com/Deotaland/Agent_link>。先执行 `git status --short --branch`，不要动和任务无关的改动。细节见 AGENTS.md"路径 C"。

## 1. 环境

- `idf.py --version` 必须是 ESP-IDF v5.5.x。
- 不在 PATH 里：先找已经装好的 ESP-IDF 并激活，再考虑重装。Windows 用"ESP-IDF PowerShell"快捷方式或 `export.ps1`；macOS/Linux 执行 `. $IDF_PATH/export.sh`。
- 安装 ESP-IDF 要下载很多东西，先征得用户同意。不要替换掉其他版本的 IDF。

## 2. 配置

- 板子类型：来自 `agent-link-flash`（`scripts/find_board.py` 会打印配置），或者用户告诉你。芯片（`esp32s3` / `esp32p4`）写在该板子的 `config.json` 里。
- 不要打开 menuconfig。在**仓库外面**建一个默认配置文件，写入 `find_board.py` 打印的两行：
  ```
  CONFIG_BOARD_TYPE_<板子类型>=y
  CONFIG_AGENT_LINK_TRANSPORT_<连接方式>=y
  ```
  Muse 固件还要加 `CONFIG_GADGET_SDK_TOKEN`，需要语音播报再加 `CONFIG_MUSE_TTS_MINIMAX_KEY`。值由用户提供，不要打印，也不要提交。
- 然后：
  ```
  idf.py -D "SDKCONFIG_DEFAULTS=sdkconfig.defaults;<该文件的绝对路径>" set-target <芯片>
  idf.py reconfigure
  ```
  `set-target` 会重新生成 `sdkconfig`（旧的存为 `sdkconfig.old`）。用户的 `sdkconfig` 里有密钥的话，执行前先告诉用户。
- 检查：
  - reconfigure 的日志里有 `agent_link: patched sdkconfig for BOARD_TYPE_…`；
  - `build/config/sdkconfig.h` 里定义了 `CONFIG_BOARD_TYPE_<板子类型> 1`。
  缺了这一步，板子和连接方式需要的配置就没有加上，Muse 固件会内存不够。

## 3. 编译

- 执行 `idf.py build`。你的改动引起的警告和错误要修掉，其余的如实汇报。
- 编译失败就是失败：`build/` 里残留的旧文件不能当作结果。
- 合并包：`cd build && python -m esptool --chip <芯片> merge_bin -o merged.bin @flash_args`。
- 看大小那一行 `agent_link.bin binary size … (N%) free`。App 分区是 2 MB。

## 4. 汇报

汇报这几项：
- 板子类型、连接方式、IDF 版本；
- 警告；
- 产物：bootloader、分区表、`ota_data_initial.bin`、App、合并包；
- 剩余空间。

带 token 或密钥的固件不能外发。编译完不代表可以烧录，烧录请接着用 `agent-link-flash`。
