[English](README.md) | **简体中文**

# 技能

本仓库的 agent 技能。每个文件夹是一个技能：`SKILL.md`（英文，开头的 `name` 和 `description` 是给工具读的），`SKILL.zh-CN.md`（中文版，内容一致），以及可选的 `scripts/` 和 `references/`。

| 技能 | 用途 |
|---|---|
| [agent-link-flash](agent-link-flash/SKILL.zh-CN.md) | 根据型号、厂家、店铺、蓝牙名称或外观认出用户的板子，征得同意后烧录，完成设置 |
| [agent-link-build](agent-link-build/SKILL.zh-CN.md) | 用 ESP-IDF 5.5 为指定板子编译固件，生成合并包 |
| [agent-link-debug](agent-link-debug/SKILL.zh-CN.md) | 根据串口日志和现象排查设备问题 |

## 安装

由 agent 自己安装，见 [AGENTS.zh-CN.md](../AGENTS.zh-CN.md) 的"技能"一节。Claude Code：

```bash
python tools/install_skills.py --user      # ~/.claude/skills，所有项目可用
python tools/install_skills.py --project   # 本仓库的 .claude/skills（已被 gitignore）
python tools/install_skills.py --dest DIR  # 其他工具加载技能的任意目录
```

加 `--check` 只查看会改动什么。不是这个脚本装的文件夹不会被覆盖，除非加 `--force`。

## 维护

- `agent-link-flash/references/boards.json` 由 `python tools/board_catalog.py` 从 `firmware/boards/*/config.json` 生成，不要手改。它或 AGENTS 里的板子表过期时，`--check` 会失败。
- `scripts/find_board.py` 在仓库里运行时直接读各 `config.json`；技能被单独安装时读自带的 `boards.json`。
- `SKILL.md` 和 `SKILL.zh-CN.md` 要同步修改。规则写在 AGENTS.md 里，技能只写做事的顺序。
