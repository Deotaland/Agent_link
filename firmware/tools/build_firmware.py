#!/usr/bin/env python3
"""Configure and build one supported firmware profile in the existing build directory."""

import argparse
import hashlib
from importlib.metadata import version as package_version
import json
import os
from pathlib import Path
import re
import shutil
import subprocess
import sys

ROOT = Path(__file__).resolve().parents[1]
MANIFEST = ROOT / "profiles/profiles.json"
PRIVATE = ROOT / "sdkconfig.private"
STATE = ROOT / ".firmware-profile.json"
SCHEMA = 3
SETTING = re.compile(r"^(CONFIG_\w+)=(.*)$")
UNSET = re.compile(r"^# (CONFIG_\w+) is not set$")
PRIVATE_KEYS = {
    "CONFIG_GADGET_SDK_TOKEN", "CONFIG_HOMEHUB_WIFI_SSID", "CONFIG_HOMEHUB_WIFI_PASSWORD",
    "CONFIG_MUSE_TTS_MINIMAX", "CONFIG_MUSE_TTS_MINIMAX_KEY",
    "CONFIG_MUSE_TTS_MINIMAX_VOICE", "CONFIG_MUSE_TTS_MINIMAX_MODEL",
    "CONFIG_AGENT_LINK_PLATFORM_USER_TOKEN", "CONFIG_AGENT_LINK_REALTIME_DIRECT",
    "CONFIG_AGENT_LINK_REALTIME_BASE_URL", "CONFIG_AGENT_LINK_REALTIME_SESSION_ID",
    "CONFIG_AGENT_LINK_REALTIME_USER_API_KEY",
}


def idf_context():
    configured = os.environ.get("IDF_PATH")
    if not configured:
        return {}
    path = Path(configured).resolve()
    version_file = path / "tools/cmake/version.cmake"
    if not version_file.is_file():
        raise ValueError("IDF_PATH does not point to an ESP-IDF checkout")
    text = version_file.read_text(encoding="utf-8")
    version = tuple(int(re.search(rf"set\(IDF_VERSION_{part}\s+(\d+)\)", text)[1])
                    for part in ("MAJOR", "MINOR", "PATCH"))
    return {"path": path.as_posix(), "version": ".".join(map(str, version))}


def supported_idf(version):
    return tuple(map(int, version.split(".")[:2])) in {(5, 5), (6, 1)}


def previous_build_context(build_dir):
    description = build_dir / "project_description.json"
    if description.is_file():
        data = json.loads(description.read_text(encoding="utf-8"))
        return data.get("idf_path"), data.get("target")
    return None, None


def settings(path):
    values = {}
    if path.exists():
        for line in path.read_text(encoding="utf-8-sig").splitlines():
            match = SETTING.fullmatch(line.strip())
            if match:
                values[match[1]] = match[2]
            else:
                match = UNSET.fullmatch(line.strip())
                if match:
                    values[match[1]] = "n"
    return values


def defaults_files(profile):
    return [ROOT / "sdkconfig.defaults", ROOT / f"sdkconfig.defaults.{profile['target']}",
            *(ROOT / name for name in profile["defaults"])]


def expected_settings(profile):
    values = {}
    for path in defaults_files(profile):
        values.update(settings(path))
    for key in identity(profile):
        values[key] = "y"
    values["CONFIG_AGENT_LINK_FIRMWARE_PROFILE"] = json.dumps(profile["name"])
    return values


def identity(profile):
    keys = {f"CONFIG_BOARD_TYPE_{profile['board_type']}"}
    if profile["backend"] != "NONE":
        keys.add(f"CONFIG_AGENT_LINK_TRANSPORT_{profile['backend']}")
    return keys


def profile_symbol(name):
    return "CONFIG_AGENT_LINK_PROFILE_" + name.upper().replace("-", "_")


def load_profiles():
    profiles = json.loads(MANIFEST.read_text(encoding="utf-8"))
    seen = set()
    for name, profile in profiles.items():
        profile["name"] = name
        if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", name):
            raise ValueError("Invalid profile name")
        for field in ("board", "app", "hardware"):
            if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", profile[field]):
                raise ValueError(f"{name}: invalid {field}")
        app = ROOT / "apps" / profile["app"]
        if not any(app.glob("*.cc")) or profile["ui"] not in {"lvgl", "raster", "none"}:
            raise ValueError(f"{name}: missing application or invalid UI")
        for field, folder in (("drivers", ROOT / "boards/common"),
                              ("hardware_sources", ROOT / "boards" / profile["hardware"])):
            for source in profile[field]:
                path = folder / source
                if path.name != source or not path.is_file():
                    raise ValueError(f"{name}: invalid {field} source")
        board_file = ROOT / "boards" / profile["board"] / "config.json"
        catalog = json.loads(board_file.read_text(encoding="utf-8"))
        entry = next((b for b in catalog["boards"] if b["board_type"] == profile["board_type"]), None)
        if (not entry or entry["status"] == "not-adapted" or
                entry["transport"] != profile["backend"] or catalog["target"] != profile["target"]):
            raise ValueError(f"{name}: board, backend or target does not match the board catalog")
        pair = (profile["board_type"], profile["backend"])
        if pair in seen:
            raise ValueError(f"{name}: duplicate firmware combination")
        seen.add(pair)
        for path in defaults_files(profile):
            if not path.resolve().is_relative_to(ROOT) or not path.is_file():
                raise ValueError(f"{name}: missing or invalid defaults file")
            if PRIVATE_KEYS.intersection(settings(path)):
                raise ValueError(f"{name}: private settings must not appear in public defaults")
        selected = {k for k, v in expected_settings(profile).items()
                    if v == "y" and k.startswith(("CONFIG_BOARD_TYPE_", "CONFIG_AGENT_LINK_TRANSPORT_"))}
        if selected != identity(profile):
            raise ValueError(f"{name}: defaults do not select the declared board and backend")
        if expected_settings(profile).get("CONFIG_AGENT_LINK_FIRMWARE_PROFILE") != json.dumps(name):
            raise ValueError(f"{name}: defaults do not select the declared profile")
        if expected_settings(profile).get(profile_symbol(name)) != "y":
            raise ValueError(f"{name}: defaults do not select the declared profile choice")
    return profiles


def check_private(path=PRIVATE):
    unknown = settings(path).keys() - PRIVATE_KEYS
    if unknown:
        raise ValueError("sdkconfig.private accepts credentials and related options only; unexpected: " +
                         ", ".join(sorted(unknown)))


def import_private(config, private=PRIVATE):
    saved = settings(private)
    inherited = {k: v for k, v in settings(config).items()
                 if k in PRIVATE_KEYS and k not in saved and v != '\"\"'}
    if inherited:
        with private.open("a", encoding="utf-8", newline="\n") as output:
            output.write("\n# Local settings imported from the previous sdkconfig. Never commit this file.\n")
            for key, value in sorted(inherited.items()):
                output.write(f"{key}={value}\n")
    return len(inherited)


def fingerprint(name, profile):
    digest = hashlib.sha256(f"{SCHEMA}:{name}".encode())
    digest.update(json.dumps(idf_context(), sort_keys=True).encode())
    for path in [MANIFEST, *defaults_files(profile), PRIVATE]:
        digest.update(path.name.encode())
        digest.update(path.read_bytes() if path.exists() else b"")
    return digest.hexdigest()


def read_state():
    return json.loads(STATE.read_text(encoding="utf-8")) if STATE.exists() else {}


def selected_profile(profiles, config, fallback=None):
    values = settings(config)
    selected = [name for name in profiles if values.get(profile_symbol(name)) == "y"]
    if len(selected) > 1:
        raise ValueError("Multiple firmware profiles selected; select exactly one")
    if selected:
        return selected[0]
    legacy = values.get("CONFIG_AGENT_LINK_FIRMWARE_PROFILE", "").strip('"')
    for name in [legacy, fallback, read_state().get("profile")]:
        if name in profiles:
            return name
    return "ai-passport-ble" if values.get("CONFIG_IDF_TARGET") == '"esp32c3"' else "rorolee-ble"


def choose_profile(profiles, current, read=input):
    names = list(profiles)
    print("Choose firmware:")
    for number, name in enumerate(names, 1):
        marker = " *" if name == current else ""
        print(f"  {number:2}. {name:24} {profiles[name]['target']}{marker}")
    while True:
        answer = read(f"Number (Enter keeps {current}): ").strip()
        if not answer:
            return current
        if answer.isdecimal() and 1 <= int(answer) <= len(names):
            return names[int(answer) - 1]
        print("Choose a number from the list.")


def prepare_config(name, profile, config, reset=False):
    check_private(PRIVATE)
    state = read_state()
    digest = fingerprint(name, profile)
    if not reset and not needs_regeneration(state, name, digest, config):
        return False
    imported = import_private(config, PRIVATE)
    backup = ROOT / "sdkconfig.before-profile"
    if config.exists() and not backup.exists():
        shutil.copy2(config, backup)
    config.write_text(f"{profile_symbol(name)}=y\n", encoding="utf-8")
    STATE.write_text(json.dumps({"schema": SCHEMA, "profile": name,
                                 "idf": idf_context(),
                                 "fingerprint": fingerprint(name, profile)}, indent=2) + "\n",
                     encoding="utf-8")
    print(f"Applying firmware profile: {state.get('profile', 'unconfigured')} -> {name}.", flush=True)
    if imported:
        print(f"Preserved {imported} private settings (values hidden).", flush=True)
    return True


def needs_regeneration(state, name, digest, config):
    return (not config.exists() or state.get("schema") != SCHEMA or
            state.get("profile") != name or state.get("fingerprint") != digest)


def validate_settings(profile, actual):
    problems = []
    selected = {k for k, v in actual.items() if v == "y" and
                k.startswith(("CONFIG_BOARD_TYPE_", "CONFIG_AGENT_LINK_TRANSPORT_"))}
    if selected != identity(profile):
        problems.append("board/backend does not match the firmware profile")
    if actual.get("CONFIG_IDF_TARGET") != json.dumps(profile["target"]):
        problems.append("target chip does not match the firmware profile")
    for key, value in expected_settings(profile).items():
        if actual.get(key, "n") != value:
            problems.append(f"{key} does not match the profile")
    if actual.get("CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL") == "y":
        if actual.get("CONFIG_SPIRAM") != "y":
            problems.append("external NimBLE buffers require PSRAM")
    return problems


def validate(name, profile, config):
    check_private()
    if needs_regeneration(read_state(), name, fingerprint(name, profile), config):
        raise ValueError(f"Configuration is missing or belongs to another profile. Run: "
                         f"python tools/build_firmware.py {name}")
    problems = validate_settings(profile, settings(config))
    if problems:
        raise ValueError("Invalid firmware configuration:\n  " + "\n  ".join(problems) +
                         f"\nRegenerate with: python tools/build_firmware.py {name} --reset-config")


def run(command, cwd=ROOT, env=None):
    subprocess.run([str(part) for part in command], cwd=cwd, env=env, check=True)


def verify_merged(build_dir):
    files = json.loads((build_dir / "flasher_args.json").read_text(encoding="utf-8"))["flash_files"]
    with (build_dir / "merged.bin").open("rb") as merged:
        for offset, filename in files.items():
            expected = (build_dir / filename).read_bytes()
            merged.seek(int(offset, 0))
            if merged.read(len(expected)) != expected:
                raise ValueError("Merged image does not match its build artifacts")


def build(args, profile):
    context = idf_context()
    idf_path = Path(context.get("path", ""))
    idf = idf_path / "tools/idf.py"
    if not idf.is_file() or not shutil.which("cmake"):
        raise ValueError("Activate an ESP-IDF 5.5.x or 6.1.x environment before building")
    if not supported_idf(context["version"]):
        raise ValueError("This firmware supports ESP-IDF 5.5.x and 6.1.x")
    if not (idf_path / "components/soc" / profile["target"]).is_dir():
        raise ValueError(f"ESP-IDF {context['version']} does not support {profile['target']}; "
                         "activate an SDK with support for this chip before building")
    print(f"Building with ESP-IDF {context['version']} ({idf_path})", flush=True)

    config = ROOT / "sdkconfig"
    build_dir = ROOT / "build"
    if build_dir.resolve() != ROOT.resolve() / "build":
        raise ValueError("The build directory must resolve inside the firmware checkout")
    check_private()
    old_target = settings(config).get("CONFIG_IDF_TARGET", "").strip('"')
    old_idf, build_target = previous_build_context(build_dir)
    framework_changed = bool(old_idf and Path(old_idf).resolve() != idf_path)
    target_changed = bool((build_target or old_target) and
                          (build_target or old_target) != profile["target"])
    previous_idf = read_state().get("idf", {})
    framework_changed = framework_changed or bool(previous_idf and previous_idf != context)
    env = dict(os.environ, IDF_TARGET=profile["target"])
    idf_command = [sys.executable, idf, "-B", build_dir]
    if (target_changed or framework_changed) and build_dir.exists():
        print("Target chip or ESP-IDF changed; clearing the existing build cache.", flush=True)
        run([*idf_command, "fullclean"],
            env=dict(os.environ, IDF_TARGET=build_target or old_target or profile["target"]))
    if not prepare_config(args.profile, profile, config, args.reset_config):
        validate(args.profile, profile, config)

    run([*idf_command, "-D", f"AGENT_LINK_PROFILE={args.profile}",
         "-D", f"IDF_TARGET={profile['target']}", "-D", f"SDKCONFIG={config.as_posix()}",
         "reconfigure"], env=env)
    validate(args.profile, profile, config)
    if args.configure_only:
        return
    run(["cmake", "--build", build_dir, "--parallel", args.jobs], env=env)
    merge_command = "merge-bin" if int(package_version("esptool").split(".")[0]) >= 5 else "merge_bin"
    run([sys.executable, "-m", "esptool", "--chip", profile["target"], merge_command,
         "-o", "merged.bin", "@flash_args"], cwd=build_dir, env=env)
    verify_merged(build_dir)
    print(f"Built {args.profile}: build/agent_link.bin and build/merged.bin. No device flashed.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", nargs="?")
    parser.add_argument("--list", action="store_true", help="list supported firmware combinations")
    parser.add_argument("--check", action="store_true", help="check profile metadata without building")
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--reset-config", action="store_true", help="regenerate this profile's configuration")
    parser.add_argument("--jobs", type=int, choices=range(1, 33), default=2)
    parser.add_argument("--validate", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--resolve", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--prepare", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--fallback", help=argparse.SUPPRESS)
    parser.add_argument("--sdkconfig", type=Path, default=ROOT / "sdkconfig", help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        profiles = load_profiles()
        if args.list:
            for name, profile in profiles.items():
                print(f"{name:24} {profile['target']:10} {profile['backend']:5} {profile['board_type']}")
        elif args.check:
            print(f"Checked {len(profiles)} firmware profiles against the board catalog.")
        elif args.resolve:
            print(selected_profile(profiles, args.sdkconfig, args.fallback))
        elif args.profile is None:
            if not sys.stdin.isatty():
                parser.error("choose firmware in menuconfig, or run this tool in a terminal to select from the list")
            args.profile = choose_profile(profiles, selected_profile(profiles, args.sdkconfig))
            build(args, profiles[args.profile])
        elif args.profile not in profiles:
            parser.error("choose a supported profile with --list")
        elif args.prepare:
            prepare_config(args.profile, profiles[args.profile], args.sdkconfig, args.reset_config)
        elif args.validate:
            validate(args.profile, profiles[args.profile], args.sdkconfig)
            print(f"Firmware configuration verified: {args.profile}")
        else:
            build(args, profiles[args.profile])
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        print(f"Error: {error}", file=sys.stderr)
        return 1
    except (KeyboardInterrupt, EOFError):
        print("Cancelled.", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
