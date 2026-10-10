**English** | [简体中文](README.zh-CN.md)

# Skills

Agent skills for this repository. Each folder is one skill: `SKILL.md` (English, with the `name` and `description` frontmatter that tools read), `SKILL.zh-CN.md` (the same in Chinese), and optional `scripts/` and `references/`.

| Skill | What it does |
|---|---|
| [agent-link-flash](agent-link-flash/SKILL.md) | Identifies the user's board from model, company, shop, Bluetooth name or looks; flashes it with their consent; sets it up |
| [agent-link-build](agent-link-build/SKILL.md) | Builds firmware for a board with ESP-IDF 5.5 and produces a merged image |
| [agent-link-debug](agent-link-debug/SKILL.md) | Diagnoses a device from its serial log and symptoms |

## Installing

Agents install these themselves; see "Skills" in [AGENTS.md](../AGENTS.md). For Claude Code:

```bash
python tools/install_skills.py --user      # ~/.claude/skills, every project
python tools/install_skills.py --project   # .claude/skills in this checkout (gitignored)
python tools/install_skills.py --dest DIR  # any folder another tool loads skills from
```

Add `--check` to see what would change. A folder that the script didn't install is left alone unless you pass `--force`.

## Maintaining

- `agent-link-flash/references/boards.json` is generated from `firmware/boards/*/config.json` by `python tools/board_catalog.py`. Don't edit it by hand. `--check` fails when it, or the board tables in AGENTS.md, are out of date.
- `scripts/find_board.py` reads the `config.json` files directly when run inside a checkout, and the bundled `boards.json` when the skill is installed on its own.
- Keep `SKILL.md` and `SKILL.zh-CN.md` in step. Rules belong in AGENTS.md; a skill only gives the order of work.
