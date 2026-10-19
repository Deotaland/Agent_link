# Agent skills

| Skill | Use |
| --- | --- |
| [agent-link-flash](agent-link-flash/SKILL.md) | Identify a board, select firmware, flash and set up |
| [agent-link-build](agent-link-build/SKILL.md) | Build and merge one supported firmware profile |
| [agent-link-port](agent-link-port/SKILL.md) | Add hardware using the BLE or WiFi SDK |
| [agent-link-debug](agent-link-debug/SKILL.md) | Diagnose logs and propose a fix |

Assistants can read these files directly. For tools that load installed skills,
copy each folder to that tool's skill directory:

```sh
python tools/install_skills.py --project
python tools/install_skills.py --user
python tools/install_skills.py --dest PATH_TO_SKILLS
```

`--project` and `--user` target Claude Code. Use `--dest` for another tool's
configured directory. `--check` previews changes; existing folders without the
installer marker are preserved unless `--force` is supplied. Installation does
not authorize device writes, erases, commits or pushes.

`SKILL.md` is the English entrypoint. Board lookup uses the checkout's catalog;
standalone installations use `agent-link-flash/references/boards.json`. Regenerate
that snapshot and the AGENTS table with `python tools/board_catalog.py` after
changing hardware or profiles, then update installed copies.
