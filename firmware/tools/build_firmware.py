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
PRODUCTS = ROOT / "profiles/products.json"
PRIVATE = ROOT / "sdkconfig.private"
STATE = ROOT / ".firmware-profile.json"
SCHEMA = 5
SETTING = re.compile(r"^(CONFIG_\w+)=(.*)$")
UNSET = re.compile(r"^# (CONFIG_\w+) is not set$")
PRIVATE_KEYS = {
    "CONFIG_AGENT_LINK_PROMPT_USER_TOKEN",
    "CONFIG_GADGET_SDK_TOKEN", "CONFIG_HOMEHUB_WIFI_SSID", "CONFIG_HOMEHUB_WIFI_PASSWORD",
    "CONFIG_MUSE_TTS_MINIMAX", "CONFIG_MUSE_TTS_MINIMAX_KEY",
    "CONFIG_MUSE_TTS_MINIMAX_VOICE", "CONFIG_MUSE_TTS_MINIMAX_MODEL",
    "CONFIG_AGENT_LINK_PLATFORM_USER_TOKEN", "CONFIG_AGENT_LINK_REALTIME_DIRECT",
    "CONFIG_AGENT_LINK_REALTIME_BASE_URL", "CONFIG_AGENT_LINK_REALTIME_SESSION_ID",
    "CONFIG_AGENT_LINK_REALTIME_USER_API_KEY",
}
sys.path.insert(0, str(Path(__file__).resolve().parent))
from firmware_manifest import resolve_profiles, system_images as compose_images, flash_plan, initial_boot_slot

from firmware_menu import (application_selection, selected_factory, factory_boards, factory_symbol,
                           factory_kconfig as make_factory_kconfig, profiles_kconfig, write_application_cache)

BUILD_TARGET_SYMBOLS = (
    "CONFIG_AGENT_LINK_BUILD_APPLICATION",
    "CONFIG_AGENT_LINK_BUILD_FACTORY",
)


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


# Physical memory, clocks and console routing must not be inherited from a chip.
HARDWARE_PREFIXES = (
    "CONFIG_ESPTOOLPY_FLASHSIZE", "CONFIG_ESPTOOLPY_FLASHMODE_",
    "CONFIG_ESPTOOLPY_FLASHFREQ_", "CONFIG_ESPTOOLPY_HEADER_FLASHSIZE_UPDATE",
    "CONFIG_PARTITION_TABLE_", "CONFIG_SPIRAM_MODE_", "CONFIG_SPIRAM_SPEED_",
    "CONFIG_ESP_DEFAULT_CPU_FREQ_", "CONFIG_ESP_CONSOLE_", "CONFIG_RTC_CLK_SRC_",
)
HARDWARE_KEYS = {"CONFIG_SPIRAM", "CONFIG_SPIRAM_IGNORE_NOTFOUND"}


def check_hardware_defaults(profile):
    for path in defaults_files(profile)[:2]:
        misplaced = [key for key in settings(path)
                     if key in HARDWARE_KEYS or key.startswith(HARDWARE_PREFIXES)]
        if misplaced:
            raise ValueError(f"{path.name}: hardware settings belong to profile defaults: " +
                             ", ".join(sorted(misplaced)))
    values = expected_settings(profile)
    sizes = [key for key, value in values.items()
             if re.fullmatch(r"CONFIG_ESPTOOLPY_FLASHSIZE_\d+MB", key) and value == "y"]
    if len(sizes) != 1:
        raise ValueError(f"{profile['name']}: declare exactly one flash capacity in profile defaults")
    filename = values.get("CONFIG_PARTITION_TABLE_CUSTOM_FILENAME", "").strip('"')
    partition = ROOT / filename
    if (values.get("CONFIG_PARTITION_TABLE_CUSTOM") != "y" or not filename or
            not partition.resolve().is_relative_to(ROOT) or not partition.is_file()):
        raise ValueError(f"{profile['name']}: declare an existing custom partition table in profile defaults")
    if filename != profile["layout_info"]["csv"]:
        raise ValueError("Layout defaults select a different partition table")
    flash_bytes = int(re.search(r"_(\d+)MB$", sizes[0])[1]) * 1024 * 1024
    if max(p["offset"] + p["size"] for p in profile["layout_info"]["partitions"].values()) > flash_bytes:
        raise ValueError("Partition layout exceeds this board's flash capacity")


def system_images(profile):
    return compose_images(profile, ROOT)


def default_image_role(profile, bundle):
    """Match the profile's real application, never infer factory from its board."""
    if not bundle:
        return "standard"
    matches = [role for role, image in bundle["images"].items()
               if role != "factory" and image["path"] == f"apps/{profile['app']}" and
               image["shared_app"] == profile.get("app_shared", "")]
    if len(matches) != 1:
        raise ValueError("Board manifest must declare exactly one matching profile application")
    return matches[0]


def image_target_symbol(profile, bundle, role):
    if role == default_image_role(profile, bundle):
        return BUILD_TARGET_SYMBOLS[0]
    if not bundle or role not in bundle["images"]:
        raise ValueError("Select an image declared by this board/application combination")
    if role == "factory":
        return BUILD_TARGET_SYMBOLS[1]
    raise ValueError("Image has no firmware build target mapping")


def selected_image_role(profile, config):
    bundle = system_images(profile)
    values = settings(config)
    selected = [symbol for symbol in BUILD_TARGET_SYMBOLS if values.get(symbol) == "y"]
    if len(selected) > 1:
        raise ValueError("Multiple firmware build targets selected")
    symbol = selected[0] if selected else BUILD_TARGET_SYMBOLS[0]
    if symbol == BUILD_TARGET_SYMBOLS[0]:
        return default_image_role(profile, bundle)
    image_target_symbol(profile, bundle, "factory")
    return "factory"


def select_image_role(profile, config, role):
    """Select the mode and its own selector without replacing the other mode."""
    symbol = image_target_symbol(profile, system_images(profile), role)
    original = config.read_text(encoding="utf-8-sig").splitlines() if config.exists() else []
    def replaced(key):
        return (key in BUILD_TARGET_SYMBOLS or
                (symbol == BUILD_TARGET_SYMBOLS[0] and key.startswith("CONFIG_AGENT_LINK_PROFILE_")) or
                (symbol == BUILD_TARGET_SYMBOLS[1] and key.startswith("CONFIG_AGENT_LINK_FACTORY_BOARD_")))
    lines = []
    for line in original:
        match = SETTING.fullmatch(line) or UNSET.fullmatch(line)
        if not match or not replaced(match[1]):
            lines.append(line)
    lines += [f"{key}=y" if key == symbol else f"# {key} is not set" for key in BUILD_TARGET_SYMBOLS]
    selected = factory_symbol(profile["board"]) if role == "factory" else profile_symbol(profile["name"])
    lines.append(f"{selected}=y")
    config.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")


def image_config_overrides(profile, role):
    bundle = system_images(profile)
    return bundle["images"][role].get("config_overrides", {}) if bundle else {}


def prepare_image_config(profile, config):
    """Apply image policy and restore the saved values when leaving that image.

    Both CLI and VS Code call --prepare before IDF reads sdkconfig. Keep these
    settings out of board/profile defaults so application builds do not inherit
    a Factory-only workaround through the reused build directory.
    """
    state = read_state()
    previous = state.get("image_config", {}).get("saved", {})
    overrides = image_config_overrides(profile, selected_image_role(profile, config))
    if not previous and not overrides:
        return False
    if set(overrides) & PRIVATE_KEYS:
        raise ValueError("Image config overrides must not contain private settings")
    actual = settings(config)
    values = dict(actual)
    # Restore the baseline before capturing the next image's overrides. A missing
    # value is removed, allowing IDF to choose its normal SDK/profile default.
    for key, value in previous.items():
        if value is None:
            values.pop(key, None)
        else:
            values[key] = value
    saved = {key: values.get(key) for key in overrides}
    values.update(overrides)
    affected = previous.keys() | overrides.keys()
    original = config.read_text(encoding="utf-8-sig")
    lines = []
    for line in original.splitlines():
        match = SETTING.fullmatch(line.strip()) or UNSET.fullmatch(line.strip())
        if not match or match[1] not in affected:
            lines.append(line)
    lines += [f"# {key} is not set" if values[key] == "n" else f"{key}={values[key]}"
              for key in sorted(affected) if key in values]
    updated = "\n".join(lines) + "\n"
    if overrides:
        state["image_config"] = {"saved": saved}
    else:
        state.pop("image_config", None)
    # IDF rewrites sdkconfig in Kconfig order. Leave it byte-for-byte alone when
    # the values already match, keeping ordinary incremental builds incremental.
    changed = any(actual.get(key) != values.get(key) for key in affected)
    if changed:
        config.write_text(updated, encoding="utf-8", newline="\n")
    if state != read_state():
        STATE.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
    return changed


def load_profiles():
    profiles = resolve_profiles(ROOT)
    seen = set()
    for name, profile in profiles.items():
        profile["name"] = name
        if not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", name):
            raise ValueError("Invalid profile name")
        for field in ("board", "app", "hardware"):
            if not re.fullmatch(r"[a-zA-Z0-9]+(?:-[a-zA-Z0-9]+)*", profile[field]):
                raise ValueError(f"{name}: invalid {field}")
        app = ROOT / "apps" / profile["app"]
        if not any(app.glob("*.cc")) or profile["ui"] not in {"lvgl", "raster", "none"}:
            raise ValueError(f"{name}: missing application or invalid UI")
        shared_app = profile.get("app_shared")
        if shared_app:
            if not isinstance(shared_app, str) or not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", shared_app):
                raise ValueError(f"{name}: invalid shared application")
            if not any((ROOT / "apps/common" / shared_app).glob("*.cc")):
                raise ValueError(f"{name}: missing shared application")
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
        pair = (profile["board_type"], profile["app"], profile["backend"])
        if pair in seen:
            raise ValueError(f"{name}: duplicate firmware combination")
        seen.add(pair)
        for path in defaults_files(profile):
            if not path.resolve().is_relative_to(ROOT) or not path.is_file():
                raise ValueError(f"{name}: missing or invalid defaults file")
            if PRIVATE_KEYS.intersection(settings(path)):
                raise ValueError(f"{name}: private settings must not appear in public defaults")
        check_hardware_defaults(profile)
        if profile.get("ui_component") and not (ROOT / "components" / profile["ui_component"] / "CMakeLists.txt").is_file():
            raise ValueError(f"{name}: missing UI component")
        system_images(profile)
        partitions = set()
        for asset in profile.get("flash_assets", []):
            partition = asset.get("partition", "")
            source = ROOT / asset.get("file", "")
            if (not re.fullmatch(r"[a-zA-Z0-9_-]{1,16}", partition) or
                    partition in partitions or not source.resolve().is_relative_to(ROOT) or
                    not source.is_file() or source.stat().st_size == 0):
                raise ValueError(f"{name}: invalid or missing flash asset")
            partitions.add(partition)
        selected = {k for k, v in expected_settings(profile).items()
                    if v == "y" and k.startswith(("CONFIG_BOARD_TYPE_", "CONFIG_AGENT_LINK_TRANSPORT_"))}
        if selected != identity(profile):
            raise ValueError(f"{name}: defaults do not select the declared board and backend")
        if expected_settings(profile).get("CONFIG_AGENT_LINK_FIRMWARE_PROFILE") != json.dumps(name):
            raise ValueError(f"{name}: defaults do not select the declared profile")
        if expected_settings(profile).get(profile_symbol(name)) != "y":
            raise ValueError(f"{name}: defaults do not select the declared profile choice")
    load_products(profiles)
    return profiles


def factory_kconfig(profiles):
    return make_factory_kconfig(profiles, ROOT)


def load_products(profiles):
    catalog = json.loads(PRODUCTS.read_text(encoding="utf-8"))
    platform = (ROOT / "boards/common/platform_config.h").read_text(encoding="utf-8")
    origin = re.search(r'^#define DEOTALAND_PLATFORM_BASE_URL "([^"]+)"$', platform, re.MULTILINE)
    if not origin or catalog.get("platform") != origin[1]:
        raise ValueError("products.json belongs to another platform; verify product IDs for the configured URL")
    products, mappings = catalog["products"], catalog["profiles"]
    ids = set()
    for key, product in products.items():
        value = product.get("id")
        if (not re.fullmatch(r"[a-z0-9]+(?:-[a-z0-9]+)*", key) or
                type(value) is not int or not 0 < value <= 2147483647 or value in ids or
                not isinstance(product.get("name"), str) or not product["name"].strip() or
                not re.fullmatch(r"esp32(?:-[a-z][0-9]+)?", product.get("chip_type", ""))):
            raise ValueError(f"products.json: invalid or duplicate product: {key}")
        ids.add(value)
    for name, key in mappings.items():
        if name not in profiles or (key is not None and key not in products):
            raise ValueError(f"products.json: unknown profile or product: {name}")
        if key is not None and products[key]["chip_type"].replace("-", "") != profiles[name]["target"]:
            raise ValueError(f"{name}: platform product chip does not match firmware target")
    for name, profile in profiles.items():
        if profile["backend"] == "WIFI" and name not in mappings:
            raise ValueError(f"{name}: declare a product mapping (or null if not registered) in products.json")
    return catalog


def product_for(profile, catalog):
    key = catalog["profiles"].get(profile["name"])
    if key is None:
        if profile["backend"] == "WIFI":
            raise ValueError(f"{profile['name']}: no registered product on {catalog['platform']}; "
                             "query the platform and add its product to profiles/products.json before building")
        return {"id": 0, "key": ""}
    return dict(catalog["products"][key], key=key)


def write_product_header(profile, output):
    catalog = load_products(load_profiles())
    product = product_for(profile, catalog)
    content = ("// Generated from profiles/products.json; do not edit.\n#pragma once\n"
               f"#define AGENT_LINK_PRODUCT_ID {product['id']}\n"
               f"#define AGENT_LINK_PRODUCT_KEY {json.dumps(product['key'])}\n")
    output.parent.mkdir(parents=True, exist_ok=True)
    if not output.is_file() or output.read_text(encoding="utf-8") != content:
        output.write_text(content, encoding="utf-8", newline="\n")
    print(f"Platform product: {product['key'] or 'unregistered'} (id={product['id']})")


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
    for path in [MANIFEST, ROOT / "partitions/layouts.json",
                 ROOT / "boards" / profile["board"] / "config.json",
                 ROOT / "apps" / profile["app"] / "manifest.json",
                 *([ROOT / "factory" / profile["factory"] / "manifest.json"] if profile.get("factory") else []),
                 *defaults_files(profile), PRIVATE]:
        digest.update(path.name.encode())
        digest.update(path.read_bytes() if path.exists() else b"")
    return digest.hexdigest()


def read_state():
    return json.loads(STATE.read_text(encoding="utf-8")) if STATE.exists() else {}


def selected_profile(profiles, config, fallback=None):
    values = settings(config)
    if sum(values.get(key) == "y" for key in BUILD_TARGET_SYMBOLS) > 1:
        raise ValueError("Multiple firmware build targets selected")
    if values.get("CONFIG_AGENT_LINK_BUILD_FACTORY") == "y":
        return selected_factory(profiles, values)
    selected = application_selection(profiles, values)
    if selected:
        return selected
    legacy = values.get("CONFIG_AGENT_LINK_FIRMWARE_PROFILE", "").strip('"')
    for name in [legacy, fallback, read_state().get("application_profile"), read_state().get("profile")]:
        if name in profiles:
            return name
    return None


def choose_profile(profiles, current, read=input):
    names = list(profiles)
    print("Choose firmware:")
    for number, name in enumerate(names, 1):
        marker = " *" if name == current else ""
        profile = profiles[name]
        print(f"  {number:2}. {name:28} {profile['board']} / {profile['target']} / {profile['backend']}{marker}")
    while True:
        answer = read(f"Number (Enter keeps {current}): " if current else "Number (select your hardware explicitly): ").strip()
        if not answer and current:
            return current
        if answer.isdecimal() and 1 <= int(answer) <= len(names):
            return names[int(answer) - 1]
        print("Choose a number from the list.")


def prepare_config(name, profile, config, reset=False):
    check_private(PRIVATE)
    state = read_state()
    digest = fingerprint(name, profile)
    if not reset and not needs_regeneration(state, name, digest, config):
        return prepare_image_config(profile, config)
    imported = import_private(config, PRIVATE)
    backup = ROOT / "sdkconfig.before-profile"
    if config.exists() and not backup.exists():
        shutil.copy2(config, backup)
    values = settings(config)
    saved_app = application_selection(resolve_profiles(ROOT), values,
                                      state.get("application_profile", state.get("profile")))
    factory_mode = values.get("CONFIG_AGENT_LINK_BUILD_FACTORY") == "y"
    # A manifest/schema migration within the same board must retain local log
    # levels and the saved application/factory choice. Switching profiles still
    # starts from that profile's defaults.
    if not reset and config.exists() and state.get("profile") == name:
        owned = expected_settings(profile)
        if factory_mode:
            owned = {key: value for key, value in owned.items() if not key.startswith("CONFIG_AGENT_LINK_PROFILE_")}
        lines = []
        for line in config.read_text(encoding="utf-8-sig").splitlines():
            match = SETTING.fullmatch(line) or UNSET.fullmatch(line)
            if match and match[1] in owned:
                continue
            lines.append(line)
        lines += [f"# {key} is not set" if value == "n" else f"{key}={value}" for key, value in owned.items()]
        config.write_text("\n".join(lines) + "\n", encoding="utf-8", newline="\n")
    else:
        kept = {key: value for key, value in values.items()
                if key in BUILD_TARGET_SYMBOLS or key.startswith(("CONFIG_AGENT_LINK_FACTORY_BOARD_", "CONFIG_LOG_"))}
        if saved_app:
            kept["CONFIG_AGENT_LINK_SAVED_APPLICATION_PROFILE"] = json.dumps(saved_app)
        if not factory_mode:
            kept[profile_symbol(name)] = "y"
        config.write_text("\n".join(f"# {key} is not set" if value == "n" else f"{key}={value}"
                                   for key, value in kept.items()) + "\n", encoding="utf-8")
    record = {"schema": SCHEMA, "profile": name, "application_profile": saved_app,
              "idf": idf_context(), "fingerprint": fingerprint(name, profile)}
    if not reset and state.get("profile") == name and state.get("image_config"):
        record["image_config"] = state["image_config"]
    STATE.write_text(json.dumps(record, indent=2) + "\n",
                     encoding="utf-8")
    prepare_image_config(profile, config)
    print(f"Applying firmware profile: {state.get('profile', 'unconfigured')} -> {name}.", flush=True)
    if imported:
        print(f"Preserved {imported} private settings (values hidden).", flush=True)
    return True


def needs_regeneration(state, name, digest, config):
    return (not config.exists() or state.get("schema") != SCHEMA or
            state.get("profile") != name or state.get("fingerprint") != digest)


def validate_settings(profile, actual):
    problems = []
    role = ("factory" if actual.get("CONFIG_AGENT_LINK_BUILD_FACTORY") == "y"
            else default_image_role(profile, system_images(profile)))
    expected = expected_settings(profile)
    expected.update(image_config_overrides(profile, role))
    selected = {k for k, v in actual.items() if v == "y" and
                k.startswith(("CONFIG_BOARD_TYPE_", "CONFIG_AGENT_LINK_TRANSPORT_"))}
    if selected != identity(profile):
        problems.append("board/backend does not match the firmware profile")
    if actual.get("CONFIG_IDF_TARGET") != json.dumps(profile["target"]):
        problems.append("target chip does not match the firmware profile")
    for key, value in expected.items():
        if actual.get("CONFIG_AGENT_LINK_BUILD_FACTORY") == "y" and key.startswith("CONFIG_AGENT_LINK_PROFILE_"):
            continue
        if actual.get(key, "n") != value:
            problems.append(f"{key} does not match the profile")
    if actual.get("CONFIG_BT_NIMBLE_MEM_ALLOC_MODE_EXTERNAL") == "y":
        if actual.get("CONFIG_SPIRAM") != "y":
            problems.append("external NimBLE buffers require PSRAM")
    return problems


def validate(name, profile, config, expected_role=None):
    check_private()
    if needs_regeneration(read_state(), name, fingerprint(name, profile), config):
        raise ValueError(f"Configuration is missing or belongs to another profile. Run: "
                         f"python tools/build_firmware.py {name}")
    problems = validate_settings(profile, settings(config))
    role = selected_image_role(profile, config)
    if expected_role is not None and role != expected_role:
        problems.append("build target changed during configuration; configure again")
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
    bundle = system_images(profile)
    if args.image_role:
        image_target_symbol(profile, bundle, args.image_role)
    product_for(profile, load_products(load_profiles()))
    context = idf_context()
    idf_path = Path(context.get("path", ""))
    idf = idf_path / "tools/idf.py"
    if not idf.is_file() or not shutil.which("cmake"):
        raise ValueError("Activate an ESP-IDF 5.5.x or 6.1.x environment before building. "
                         f"From the repository root run: python tools/check_env.py --profile {args.profile}. "
                         "Installation and China/offline downloads: guides/environment.zh-CN.md")
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
    if args.image_role and config.exists():
        select_image_role(profile, config, args.image_role)
    regenerated = prepare_config(args.profile, profile, config, args.reset_config)
    if args.image_role:
        select_image_role(profile, config, args.image_role)
    role = selected_image_role(profile, config)
    print(f"Selected build target: {role} (board {profile['board']})", flush=True)
    if not regenerated:
        validate(args.profile, profile, config)

    run([*idf_command, "-D", f"AGENT_LINK_PROFILE={args.profile}",
         "-D", f"IDF_TARGET={profile['target']}", "-D", f"SDKCONFIG={config.as_posix()}",
         "reconfigure"], env=env)
    validate(args.profile, profile, config, role)
    if args.configure_only:
        return
    run(["cmake", "--build", build_dir, "--parallel", args.jobs], env=env)
    archive_build(profile, role, build_dir)
    if bundle and role != "factory":
        # A merged image spanning ota_0 would fill the launcher's area with FF.
        # App updates therefore use separate files; initial USB packages contain
        # a separately built and verified board-bound launcher.
        (build_dir / "merged.bin").unlink(missing_ok=True)
        print(f"Built {role}: app -> {flash_plan(profile, role)['partition']}; "
              "Factory preserved. Initial USB package: tools/package_firmware.py "
              f"{args.profile} --jobs 2. No device flashed.")
    else:
        merge_command = "merge-bin" if int(package_version("esptool").split(".")[0]) >= 5 else "merge_bin"
        run([sys.executable, "-m", "esptool", "--chip", profile["target"], merge_command,
             "-o", "merged.bin", "@flash_args"], cwd=build_dir, env=env)
        verify_merged(build_dir)
        print(f"Built {args.profile}: build/agent_link.bin and build/merged.bin. No device flashed.")


def archive_build(profile, role, build_dir):
    metadata = json.loads((build_dir / "flasher_args.json").read_text(encoding="utf-8"))
    plan = flash_plan(profile, role)
    if plan.get('dynamic_slots'):
        from dynamic_layout import verify_bootloader
        verify_bootloader(build_dir / 'bootloader/bootloader.bin', profile)
    files = metadata["flash_files"]
    app_offsets = [int(offset, 0) for offset, file in files.items() if file == "agent_link.bin"]
    if app_offsets != [plan["app_offset"]]:
        raise ValueError("IDF flash metadata disagrees with the generated application destination")
    if (build_dir / "agent_link.bin").stat().st_size > plan["app_max_bytes"]:
        raise ValueError("Application exceeds its usable partition size")
    if plan["mode"] == "managed":
        from firmware_image import image_info
        info = image_info(build_dir / "agent_link.bin", plan["model"], plan.get("app_abi", plan["layout_version"]), plan["app_max_bytes"], chip=plan["chip"])
        if info["app_id"] != plan["app_id"]:
            raise ValueError("Built application identity differs from the selected target")
    if plan.get('launcher_ab'):
        recovery = 'recovery/SDGOODS_LAUNCHER.bin'
        if files.get('0x10000') != recovery:
            raise ValueError('Full flash metadata must include the immutable recovery entry')
        image_info(build_dir / recovery, plan['model'], plan['app_abi'],
                   plan['partitions'][plan.get('recovery_partition', 'factory')]['size'], chip=plan['chip'])
        if (build_dir / 'launcher-state-reset.bin').read_bytes() != b'\xff' * 8192:
            raise ValueError('Initial package must reset Launcher trial state')
    output = build_dir / "images" / profile["name"] / role
    output.mkdir(parents=True, exist_ok=True)
    names = set(files.values()) | {"agent_link.elf", "agent_link.map", "flasher_args.json", "flash-plan.json"}
    if plan.get('launcher_ab'):
        names.update({'recovery/SDGOODS_LAUNCHER.elf', 'recovery/SDGOODS_LAUNCHER.map'})
    if profile.get("community_store"):
        names.update({"webp-LICENSE.txt", "webp-NOTICE.txt", "lodepng-LICENSE.txt"})
    for filename in ("LICENSE", "NOTICE"):
        source = ROOT / "boards" / profile["board"] / filename
        if source.is_file():
            name = f"{profile['board']}-{filename}.txt"
            shutil.copy2(source, build_dir / name)
            names.add(name)
    if any(asset["file"].startswith("components/paper_resources/") for asset in profile.get("flash_assets", [])):
        names.update({"fonts-OFL.txt", "pinyin-LICENSE.txt", "jieba-LICENSE.txt"})
    for name in names:
        destination = output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(build_dir / name, destination)
    record = dict(schema=1, profile=profile["name"], role=role, plan=plan, idf=idf_context(),
                  sha256={name: hashlib.sha256((output / name).read_bytes()).hexdigest() for name in sorted(names)})
    (output / "artifacts.json").write_text(json.dumps(record, indent=2) + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("profile", nargs="?")
    parser.add_argument("--list", action="store_true", help="list supported firmware combinations")
    parser.add_argument("--check", action="store_true", help="check profile metadata without building")
    parser.add_argument("--configure-only", action="store_true")
    parser.add_argument("--reset-config", action="store_true", help="regenerate this profile's configuration")
    parser.add_argument("--jobs", type=int, choices=range(1, 33), default=2)
    parser.add_argument("--image-role", help="Select an image through the Menuconfig build target; otherwise keep that selection (default: profile application)")
    parser.add_argument("--generate-kconfig", action="store_true", help="regenerate factory/layout Kconfig bindings")
    parser.add_argument("--menu-cache", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--export-profiles", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--flash-plan", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--validate", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--resolve", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--resolve-image-role", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--prepare", action="store_true", help=argparse.SUPPRESS)
    parser.add_argument("--product-header", type=Path, help=argparse.SUPPRESS)
    parser.add_argument("--fallback", help=argparse.SUPPRESS)
    parser.add_argument("--sdkconfig", type=Path, default=ROOT / "sdkconfig", help=argparse.SUPPRESS)
    args = parser.parse_args()
    try:
        profiles = load_profiles()
        if args.generate_kconfig:
            (ROOT / "main/Kconfig.factories").write_text(factory_kconfig(profiles), encoding="utf-8", newline="\n")
            (ROOT / "main/Kconfig.profiles").write_text(profiles_kconfig(profiles), encoding="utf-8", newline="\n")
        elif args.menu_cache:
            state = read_state()
            name = application_selection(profiles, settings(args.sdkconfig), state.get("application_profile", state.get("profile")))
            write_application_cache(args.menu_cache, name)
            if name and state.get("application_profile") != name:
                state["application_profile"] = name
                STATE.write_text(json.dumps(state, indent=2) + "\n", encoding="utf-8")
        elif args.export_profiles:
            for profile in profiles.values():
                profile["system"] = system_images(profile)
            args.export_profiles.parent.mkdir(parents=True, exist_ok=True)
            args.export_profiles.write_text(json.dumps(profiles, indent=2) + "\n", encoding="utf-8")
        elif args.list:
            for name, profile in profiles.items():
                print(f"{name:24} {profile['target']:10} {profile['backend']:5} {profile['board_type']}")
        elif args.check:
            for filename, expected in (("Kconfig.factories", factory_kconfig(profiles)),
                                       ("Kconfig.profiles", profiles_kconfig(profiles))):
                if (ROOT / "main" / filename).read_text(encoding="utf-8") != expected:
                    raise ValueError("Kconfig bindings are stale; run --generate-kconfig")
            print(f"Checked {len(profiles)} firmware profiles against the board catalog.")
        elif args.resolve:
            selected = selected_profile(profiles, args.sdkconfig, args.fallback)
            if not selected:
                raise ValueError("No firmware profile selected. Run python tools/build_firmware.py "
                                 "in a terminal, or provide an explicit profile from --list.")
            print(selected)
        elif args.profile is None:
            if not sys.stdin.isatty():
                parser.error("choose firmware in menuconfig, or run this tool in a terminal to select from the list")
            args.profile = choose_profile(profiles, selected_profile(profiles, args.sdkconfig))
            build(args, profiles[args.profile])
        elif args.profile not in profiles:
            parser.error("choose a supported profile with --list")
        elif args.product_header:
            write_product_header(profiles[args.profile], args.product_header)
        elif args.resolve_image_role:
            print(selected_image_role(profiles[args.profile], args.sdkconfig))
        elif args.flash_plan:
            role = selected_image_role(profiles[args.profile], args.sdkconfig)
            args.flash_plan.parent.mkdir(parents=True, exist_ok=True)
            plan = flash_plan(profiles[args.profile], role)
            args.flash_plan.write_text(json.dumps(plan, indent=2) + "\n", encoding="utf-8")
            from firmware_image import boot_data
            args.flash_plan.with_name("boot-selection.bin").write_bytes(boot_data(initial_boot_slot(plan)))
        elif args.prepare:
            prepare_config(args.profile, profiles[args.profile], args.sdkconfig, args.reset_config)
        elif args.validate:
            validate(args.profile, profiles[args.profile], args.sdkconfig, args.image_role)
            print(f"Firmware configuration verified: {args.profile} (image {selected_image_role(profiles[args.profile], args.sdkconfig)})")
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
