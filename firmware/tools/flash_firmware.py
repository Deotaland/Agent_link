#!/usr/bin/env python3
"""IDF/esptool adapter: honor the generated plan and check managed USB writes.

No serial connection occurs for builds, packaging, --describe, or esptool's
offline commands. Preflight runs only when the user actually invokes Flash.
"""
import argparse
import json
from pathlib import Path
import shlex
import subprocess
import sys

from firmware_image import CHIP_IDS, descriptor, image_info, partition_records


def expand_args(arguments, folder):
    result = []
    for value in arguments:
        if value.startswith("@"):
            result += shlex.split((folder / value[1:]).read_text(encoding="utf-8"), comments=True)
        else:
            result.append(value)
    return result


def write_images(arguments):
    index = next((i for i, word in enumerate(arguments) if word in {"write-flash", "write_flash"}), None)
    if index is None:
        return None, []
    pairs = []
    cursor = index + 1
    while cursor < len(arguments) - 1:
        word = arguments[cursor]
        if word.startswith("0x"):
            try:
                address = int(word, 0)
            except ValueError:
                cursor += 1
                continue
            pairs.append((address, arguments[cursor + 1]))
            cursor += 2
        else:
            cursor += 1
    return index, pairs


def compatible_identity(data, plan, app_id):
    info = descriptor(data)
    if (info["app_id"] != app_id or info["board"] != plan["model"] or
            info["layout"] != plan.get("app_abi", plan["layout_version"]) or info["sdk"] != "0.1.0" or info["chip_id"] != CHIP_IDS.get(plan["chip"])):
        raise ValueError("Incompatible application identity")
    return info


def check_device(plan, table, expected_table, launcher, target=None):
    if partition_records(table) != partition_records(expected_table):
        raise ValueError("Device layout differs. Install the board USB package before an application update.")
    if plan["role"] == "factory":
        return
    compatible_identity(launcher, plan, "agent.launcher")
    if target is not None and target != b"\xff" * len(target):
        try:
            compatible_identity(target, plan, plan["app_id"])
        except ValueError as error:
            raise ValueError(f"{plan['partition']} contains another or unrecognized application. "
                             "Use the Factory installer to choose a free slot, or remove that app first.") from error


def preflight(plan, arguments, folder, run=subprocess.run):
    index, images = write_images(arguments)
    if index is None or plan["mode"] != "managed":
        return
    app_file = folder / "agent_link.bin"
    # Only board application writes are managed here; bootloader-only and other
    # low-level IDF utility targets retain their original esptool behavior.
    selected = [(address, name) for address, name in images if (folder / name).resolve() == app_file.resolve()]
    if not selected:
        return
    if len(selected) != 1 or selected[0][0] != plan["app_offset"]:
        raise ValueError("Flash arguments do not match the generated application destination")
    if plan.get("dynamic_slots"):
        required = {0, plan['partition_table_offset'],
                    plan['partitions'][plan['dynamic_slots']['guard_partition']]['offset']}
        if not required.issubset(address for address, _ in images):
            raise ValueError("Dynamic layouts require the matching full USB package. Install apps through Agent_link Launcher.")
    info = image_info(app_file, plan["model"], plan.get("app_abi", plan["layout_version"]), plan["app_max_bytes"], chip=plan["chip"])
    if info["app_id"] != plan["app_id"]:
        raise ValueError("Build image does not match the selected firmware")
    prefix = arguments[:index]
    if not any(word in {"-p", "--port"} or word.startswith("--port=") for word in prefix):
        raise ValueError("Select the device port in VS Code or pass idf.py -p PORT.")
    if any(word in {"--encrypt", "--encrypt-files", "--encrypted"} for word in arguments[index + 1:]):
        raise ValueError("Managed USB preflight requires a supported plaintext development device")
    read_prefix = []
    cursor = 0
    while cursor < len(prefix):
        word = prefix[cursor]
        if word in {"--after", "--before"}:
            cursor += 2
            continue
        if not word.startswith(("--after=", "--before=")):
            read_prefix.append(word)
        cursor += 1
    read_prefix += ["--before", "default-reset", "--after", "no-reset"]
    scratch = folder / "flash-preflight"
    scratch.mkdir(exist_ok=True)

    def read(address, size, name):
        output = scratch / name
        run([sys.executable, "-m", "esptool", *read_prefix, "read-flash", hex(address), hex(size), str(output)], check=True)
        value = output.read_bytes()
        if len(value) != size:
            raise ValueError("Incomplete device read; nothing was written")
        return value

    table = read(plan["partition_table_offset"], 0x1000, "partition-table.bin")
    expected = (folder / "partition_table/partition-table.bin").read_bytes()
    # Factory is also the first-install/recovery entrypoint. Flashing it into a
    # blank device is allowed; replacing an existing layout requires the explicit
    # full package path rather than app-flash.
    if plan["role"] == "factory" and table == b"\xff" * len(table):
        if not any(address == plan["partition_table_offset"] for address, _ in images):
            raise ValueError("A blank device requires full Flash, not app-flash")
    elif not plan.get("dynamic_slots"):
        launcher = read(plan["partitions"][plan.get("recovery_partition", "factory")]["offset"], 0x200, "factory-header.bin") if plan["role"] != "factory" else b""
        target = read(plan["app_offset"], 0x200, "app-header.bin") if plan["role"] != "factory" else None
        check_device(plan, table, expected, launcher, target)
    print(f"Verified {plan['board']}: {plan['app_id']} -> {plan['partition']} @ {plan['app_offset']:#x}", flush=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plan", type=Path, required=True)
    parser.add_argument("--describe", action="store_true")
    parser.add_argument("arguments", nargs=argparse.REMAINDER)
    args = parser.parse_args()
    plan = json.loads(args.plan.read_text(encoding="utf-8"))
    if args.describe:
        print(json.dumps(plan, indent=2))
        return 0
    arguments = args.arguments[1:] if args.arguments[:1] == ["--"] else args.arguments
    folder = args.plan.resolve().parent
    try:
        preflight(plan, expand_args(arguments, folder), folder)
        return subprocess.run([sys.executable, "-m", "esptool", *arguments]).returncode
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"Flash refused: {error}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
