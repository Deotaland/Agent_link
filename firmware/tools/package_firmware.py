#!/usr/bin/env python3
"""Build and verify one board's launcher + selected app USB packages; never flash."""
import argparse
import hashlib
from importlib.metadata import version
import json
from pathlib import Path
import re
import shutil
import subprocess
import sys

from build_firmware import ROOT, PRIVATE, PRIVATE_KEYS, settings, load_profiles, default_image_role, system_images
from firmware_image import boot_data, image_info
from firmware_manifest import flash_plan


def checked_artifacts(folder, profile, role):
    record = json.loads((folder / "artifacts.json").read_text(encoding="utf-8"))
    if record["profile"] != profile["name"] or record["role"] != role:
        raise ValueError("Wrong build artifacts for the selected firmware")
    if record["plan"] != flash_plan(profile, role):
        raise ValueError("Build artifacts use an older board/application/layout manifest; rebuild before packaging")
    for name, digest in record["sha256"].items():
        if hashlib.sha256((folder / name).read_bytes()).hexdigest() != digest:
            raise ValueError(f"Build artifact changed: {name}")
    return record


def package(profile, output):
    bundle = system_images(profile)
    if not bundle:
        raise ValueError("Use build_firmware.py for this board's single-app USB package")
    catalog = json.loads((ROOT / "boards" / profile["board"] / "config.json").read_text(encoding="utf-8"))
    hardware = next(entry for entry in catalog["boards"] if entry["board_type"] == profile["board_type"])
    package_name = profile.get("package_name", "")
    if not isinstance(package_name, str) or (package_name and not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", package_name)):
        raise ValueError("Invalid board package filename prefix")
    role = default_image_role(profile, bundle)
    images = ROOT / "build/images" / profile["name"]
    factory = images / "factory"
    app = images / role
    factory_record = checked_artifacts(factory, profile, "factory")
    app_record = checked_artifacts(app, profile, role)
    if factory_record["idf"] != app_record["idf"]:
        raise ValueError("Factory and application must use the same ESP-IDF build environment")
    if (factory / "partition_table/partition-table.bin").read_bytes() != (app / "partition_table/partition-table.bin").read_bytes():
        raise ValueError("Factory and app use different partition tables")
    plan = app_record["plan"]
    if plan.get("dynamic_slots"):
        from dynamic_layout import verify_bootloader
        for source in (factory, app):
            verify_bootloader(source / 'bootloader/bootloader.bin', profile)
            if (source / 'flash_assets/pt_guard.bin').read_bytes() != b'\xff' * 0x4000:
                raise ValueError('Full USB package must reset the partition recovery journal')
    factory_info = image_info(factory / "agent_link.bin", profile["model"], bundle["layout"],
                              plan["partitions"]["factory"]["size"], chip=plan["chip"])
    app_info = image_info(app / "agent_link.bin", profile["model"], bundle["layout"], plan["app_max_bytes"], chip=plan["chip"])
    if factory_info["app_id"] != "agent.launcher" or app_info["app_id"] != plan["app_id"]:
        raise ValueError("Factory/application identity does not match package plan")
    output.mkdir(parents=True, exist_ok=True)
    metadata = json.loads((factory / "flasher_args.json").read_text(encoding="utf-8"))
    files = {}
    for offset, filename in metadata["flash_files"].items():
        target = "factory.bin" if filename == "agent_link.bin" else filename
        destination = output / target
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(factory / filename, destination)
        files[offset] = target
    # Initial board packages always enter the launcher. Application USB updates
    # instead use their own plan/boot-selection record to start the selected app.
    (output / "boot-selection.bin").write_bytes(boot_data())
    for selected_role, source in (("factory", factory), (role, app)):
        for suffix in ("bin", "elf", "map"):
            shutil.copy2(source / f"agent_link.{suffix}", output / f"{selected_role}.{suffix}")
    shutil.copy2(ROOT / "boards/common/fonts/OFL.txt", output / "fonts-OFL.txt")
    notices = {name for name in factory_record["sha256"]
               if name.endswith(("-LICENSE.txt", "-NOTICE.txt"))}
    for name in notices:
        shutil.copy2(factory / name, output / name)
    full_files = dict(files)
    full_files[hex(plan["app_offset"])] = role + ".bin"
    merge = "merge-bin" if int(version("esptool").split(".")[0]) >= 5 else "merge_bin"
    for merged_name, segments in (("factory-only-merged.bin", files), ("merged.bin", full_files)):
        command = [sys.executable, "-m", "esptool", "--chip", plan["chip"], merge, "-o", merged_name]
        for key, value in metadata["flash_settings"].items():
            command += ["--" + (key.replace("_", "-") if merge == "merge-bin" else key), value]
        for offset, filename in sorted(segments.items(), key=lambda item: int(item[0], 0)):
            command += [offset, filename]
        subprocess.run(command, cwd=output, check=True)
        combined = (output / merged_name).read_bytes()
        for offset, filename in segments.items():
            value = (output / filename).read_bytes()
            start = int(offset, 0)
            if combined[start:start + len(value)] != value:
                raise ValueError("Merged package does not match its source images")
        # Empty slots must actually be FF, including stale app/cover areas on a
        # used device. A contiguous initial package is intentionally a reset.
        for slot in plan["app_slots"]:
            partition = plan["partitions"][slot]
            if hex(partition["offset"]) in segments:
                continue
            value = combined[partition["offset"]:partition["offset"] + partition["size"]]
            if value != b"\xff" * partition["size"]:
                raise ValueError("Initial package contains unexpected slot data")
    metadata["flash_files"] = full_files
    metadata["app"]["file"] = "factory.bin"
    metadata[plan["partition"]] = dict(offset=hex(plan["app_offset"]), file=role + ".bin", encrypted="false")
    (output / "flasher_args.json").write_text(json.dumps(metadata, indent=2) + "\n", encoding="utf-8")
    package_info = dict(schema=1, layout=plan["layout_version"], layout_id=plan["layout"], board=profile["model"],
                        display_name=hardware["product"], brand=hardware["vendor"],
                        hardware_model=hardware["model"], aliases=hardware["aliases"],
                        profile=profile["name"], boot="factory", apps=[dict(app_info, file=role + ".bin")],
                        settings_impact="Initial merged USB packages replace settings and installed apps.")
    if plan.get("dynamic_slots"):
        package_info.update(app_abi=bundle["layout"], dynamic_slots=plan["dynamic_slots"])
    package_files = set(full_files.values()) | {"merged.bin", "factory-only-merged.bin", "flasher_args.json",
                                               "factory.elf", "factory.map", role + ".elf", role + ".map", "fonts-OFL.txt"}
    package_files.update(notices)
    if package_name:
        for source, suffix in (("merged.bin", "full"), ("factory-only-merged.bin", "factory-full")):
            name = f"{package_name}-{suffix}.bin"
            shutil.copy2(output / source, output / name)
            package_files.add(name)
    package_info["sha256"] = {name: hashlib.sha256((output / name).read_bytes()).hexdigest()
                              for name in sorted(package_files)}
    (output / "package.json").write_text(json.dumps(package_info, indent=2) + "\n", encoding="utf-8")
    print(f"Verified board USB packages: {output}. Starts Factory with {role} installed. No device flashed.")
    return output


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile")
    parser.add_argument("--jobs", type=int, choices=(1, 2), default=2)
    parser.add_argument("--package-only", action="store_true", help="verify/package existing matching image artifacts")
    parser.add_argument("--reset-config", action="store_true")
    parser.add_argument("--public", action="store_true", help="refuse credential-bearing builds for a shared test service")
    parser.add_argument("--output", type=Path)
    args = parser.parse_args()
    try:
        profile = load_profiles()[args.profile]
        bundle = system_images(profile)
        if not bundle:
            raise ValueError("This board has no launcher package")
        if args.public and any(value not in {'""', 'n', '0'} for key, value in settings(PRIVATE).items() if key in PRIVATE_KEYS):
            raise ValueError("Private settings are present; public test packages require a credential-free build")
        role = default_image_role(profile, bundle)
        if not args.package_only:
            for index, selected_role in enumerate(("factory", role)):
                subprocess.run([sys.executable, str(ROOT / "tools/build_firmware.py"), args.profile,
                                "--image-role", selected_role, "--jobs", str(args.jobs),
                                *(["--reset-config"] if args.reset_config and index == 0 else [])], check=True)
        package(profile, (args.output or ROOT / "build/packages" / args.profile).resolve())
    except (ValueError, KeyError, OSError, subprocess.CalledProcessError) as error:
        print(f"Package failed: {error}", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
