"""Compile a Windhawk mod with Windhawk's own toolchain, optionally install it.

Mirrors what the Windhawk 1.7 editor does when you press "Compile": same
clang++ invocation, same DLL naming, same mod .ini fields. Useful for CI (compile
check only) and for iterating on the mod against a portable Windhawk without the
GUI.

    python tools/build.py --windhawk C:\\path\\to\\windhawk             # compile only
    python tools/build.py --windhawk C:\\path\\to\\windhawk --install   # compile + load
    python tools/build.py --windhawk ... --install --include "C:\\wt\\WindowsTerminal.exe"

--install only supports portable Windhawk layouts (windhawk.ini with Portable=1),
because a standard install keeps mod configuration in HKLM and needs admin.
"""

from __future__ import annotations

import argparse
import configparser
import random
import re
import shlex
import shutil
import subprocess
import sys
import tempfile
import time
from pathlib import Path

REPO = Path(__file__).resolve().parent.parent
DEFAULT_SOURCE = REPO / "windows-terminal-vertical-tabs.wh.cpp"

TARGETS = {
    "x86-64": ("x86_64-w64-mingw32", "64"),
    "arm64": ("aarch64-w64-mingw32", "arm64"),
    "x86": ("i686-w64-mingw32", "32"),
}


def parse_metadata(source: str) -> dict[str, list[str]]:
    block = re.search(r"// ==WindhawkMod==(.*?)// ==/WindhawkMod==", source, re.S)
    if not block:
        sys.exit("no ==WindhawkMod== block")
    meta: dict[str, list[str]] = {}
    for line in block.group(1).splitlines():
        m = re.match(r"\s*//\s*@(\S+)\s+(.*?)\s*$", line)
        if m:
            meta.setdefault(m.group(1), []).append(m.group(2))
    return meta


def parse_setting_defaults(source: str) -> dict[str, str]:
    """Top-level `- key: value` entries of the settings YAML (flat settings only)."""
    block = re.search(r"// ==WindhawkModSettings==\s*/\*(.*?)\*/", source, re.S)
    defaults: dict[str, str] = {}
    if block:
        for m in re.finditer(r"^- (\w+): *(.*?)\s*$", block.group(1), re.M):
            value = m.group(2)
            if value in ("true", "false"):
                value = "1" if value == "true" else "0"
            defaults[m.group(1)] = value.strip('"')
    return defaults


def storage_paths(windhawk: Path) -> dict[str, Path | bool]:
    ini = configparser.ConfigParser(interpolation=None)
    ini.optionxform = str
    ini.read(windhawk / "windhawk.ini", encoding="utf-8")
    storage = ini["Storage"]

    def resolve(key: str) -> Path:
        return (windhawk / storage[key]).resolve()

    return {
        "portable": storage.get("Portable", "0") == "1",
        "appdata": resolve("AppDataPath"),
        "engine": resolve("EnginePath"),
        "compiler": resolve("CompilerPath"),
    }


def compile_mod(source_path: Path, windhawk: Path, arch: str, out_dir: Path,
                defines: list[str] | None = None) -> Path:
    source = source_path.read_text(encoding="utf-8")
    meta = parse_metadata(source)
    mod_id, version = meta["id"][0], meta["version"][0]
    triple, subfolder = TARGETS[arch]
    paths = storage_paths(windhawk)

    out_dir = out_dir.resolve()
    out_dir.mkdir(parents=True, exist_ok=True)
    dll = out_dir / f"{mod_id}_{version}_{random.randint(100000, 999999)}.dll"
    compiler_options = shlex.split(meta.get("compilerOptions", [""])[0], posix=False)

    args = [
        str(paths["compiler"] / "bin" / "clang++.exe"),
        "-std=c++23", "-O2", "-shared",
        "-DUNICODE", "-D_UNICODE",
        "-DWINVER=0x0A00", "-D_WIN32_WINNT=0x0A00", "-D_WIN32_IE=0x0A00",
        "-DNTDDI_VERSION=0x0A000008",
        "-D__USE_MINGW_ANSI_STDIO=0",
        "-DWH_MOD",
        f'-DWH_MOD_ID=L"{mod_id}"',
        f'-DWH_MOD_VERSION=L"{version}"',
        str(paths["engine"] / subfolder / "windhawk.lib"),
        "-x", "c++", "-",
        "-include", "windhawk_api.h",
        "-target", triple,
        "-Wl,--export-all-symbols",
        "-o", str(dll),
        *compiler_options,
        *[f"-D{d}" for d in (defines or [])],
    ]
    started = time.monotonic()
    result = subprocess.run(args, input=source.encode("utf-8"), cwd=paths["compiler"],
                            capture_output=True)
    elapsed = time.monotonic() - started
    output = (result.stdout + result.stderr).decode("utf-8", "replace").strip()
    if output:
        print(output)
    if result.returncode != 0:
        sys.exit(f"compile failed for {arch} ({elapsed:.1f}s)")
    print(f"compiled {arch}: {dll.name} ({elapsed:.1f}s)")
    return dll


def install(source_path: Path, windhawk: Path, dll: Path, arch: str,
            include_override: list[str] | None) -> None:
    paths = storage_paths(windhawk)
    if not paths["portable"]:
        sys.exit("--install needs a portable Windhawk (Portable=1 in windhawk.ini)")

    source = source_path.read_text(encoding="utf-8")
    meta = parse_metadata(source)
    mod_id, version = meta["id"][0], meta["version"][0]
    triple, subfolder = TARGETS[arch]

    mods_dir = paths["appdata"] / "Engine" / "Mods"
    target_dir = mods_dir / subfolder
    target_dir.mkdir(parents=True, exist_ok=True)

    # The runtime libraries the editor copies next to compiled mods.
    libs = paths["compiler"] / triple / "bin"
    for src, dst in (("libc++.dll", "libc++.whl"), ("libunwind.dll", "libunwind.whl"),
                     ("windhawk-mod-shim.dll", "windhawk-mod-shim.dll")):
        if (libs / src).exists():
            try:
                shutil.copy2(libs / src, target_dir / dst)
            except PermissionError:
                pass  # in use by a running process and already current

    final = target_dir / dll.name
    shutil.copy2(dll, final)

    ini_path = mods_dir / f"{mod_id}.ini"
    ini = configparser.ConfigParser(interpolation=None)
    ini.optionxform = str
    if ini_path.exists():
        ini.read(ini_path, encoding="utf-8")
    if "Mod" not in ini:
        ini["Mod"] = {}
    mod = ini["Mod"]

    old_dll = mod.get("LibraryFileName")
    mod["LibraryFileName"] = final.name
    mod["Disabled"] = "0"
    mod["LoggingEnabled"] = "1"
    mod["DebugLoggingEnabled"] = "0"
    mod["Include"] = "|".join(include_override or meta.get("include", []))
    mod["Exclude"] = "|".join(meta.get("exclude", []))
    mod["IncludeCustom"] = ""
    mod["ExcludeCustom"] = ""
    mod["IncludeExcludeCustomOnly"] = "0"
    mod["PatternsMatchCriticalSystemProcesses"] = "0"
    mod["Architecture"] = "|".join(meta.get("architecture", []))
    mod["Version"] = version

    if "Settings" not in ini:
        ini["Settings"] = parse_setting_defaults(source)
        mod["SettingsChangeTime"] = str(int(time.time()) & 0x7FFFFFFF)

    with open(ini_path, "w", encoding="utf-8") as f:
        ini.write(f, space_around_delimiters=False)
    print(f"installed {final} -> {ini_path}")

    if old_dll and old_dll != final.name:
        try:
            (target_dir / old_dll).unlink()
        except OSError:
            pass  # still loaded somewhere; Windhawk cleans these up later


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__,
                                     formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--windhawk", type=Path, required=True,
                        help="Windhawk install folder (the one with windhawk.ini)")
    parser.add_argument("--source", type=Path, default=DEFAULT_SOURCE)
    parser.add_argument("--arch", action="append", choices=TARGETS,
                        help="architectures to build (default: the mod's @architecture)")
    parser.add_argument("--out", type=Path,
                        default=Path(tempfile.gettempdir()) / "wt-vertical-tabs-build",
                        help="where compiled DLLs go (default: a temp folder, not the repo)")
    parser.add_argument("--install", action="store_true",
                        help="copy the x86-64 build into a portable Windhawk and enable it")
    parser.add_argument("--define", action="append",
                        help="extra preprocessor define, e.g. WTVT_TEST_HOOKS for test builds")
    parser.add_argument("--include", action="append",
                        help="override @include when installing (e.g. a test terminal path)")
    args = parser.parse_args()

    meta = parse_metadata(args.source.read_text(encoding="utf-8"))
    # Without --arch, build what Windhawk builds on an ARM64 machine, which is
    # the widest set: there `x86-64` expands to both the arm64 and the x86-64
    # target, and no @architecture at all means x86 plus x86-64.
    arches = args.arch
    if not arches:
        arches = []
        for declared in meta.get("architecture", ["x86", "x86-64"]):
            expanded = {"x86-64": ["x86-64", "arm64"], "amd64": ["x86-64"]}.get(declared, [declared])
            arches += [a for a in expanded if a not in arches]
    built = {arch: compile_mod(args.source, args.windhawk, arch, args.out, args.define)
             for arch in arches}

    if args.install:
        if "x86-64" not in built:
            sys.exit("--install needs an x86-64 build")
        install(args.source, args.windhawk, built["x86-64"], "x86-64", args.include)


if __name__ == "__main__":
    main()
