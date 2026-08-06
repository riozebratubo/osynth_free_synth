#!/usr/bin/env python3
"""osynth — build the firmware with ESP-IDF and collect a flashable release.

Wraps `idf.py build` and drops the three images a chip actually needs —
bootloader, partition table and app — into

    private_releases/firmware<version>/

where <version> is read from app_osyntho/CMakeLists.txt (APP_VERSION), the
same single source deploy_linux.sh / deploy_macos_multi.sh read. A hard-coded
copy here would drift from the app within a release or two and then every
artifact would be stamped with a version nothing else ever had.

Why this is not just `idf.py build && cp`:

  * It never touches the root `sdkconfig`. That file is the bench config —
    whatever flash model, PSRAM setting and log level the last debugging
    session left behind — and a release must not inherit it. Each target gets
    its own build_release_<target>/ with its own sdkconfig, generated fresh by
    ESP-IDF from the checked-in sdkconfig.defaults + sdkconfig.defaults.<target>.
    (`build_*/` is already gitignored.)

  * Names and offsets come out of the build's own flasher_args.json rather
    than being written down here. Offsets move with partitions.csv and the
    bootloader offset differs between chips (0x0 on the S3/P4, 0x1000 on the
    classic ESP32) — reading them back is the only version that stays true.

  * It refuses to ship silently-wrong firmware: no drum kit image means a
    build that links fine and has a mute drum bus, which is easy to miss on
    the bench and impossible to miss in a release.

The ESP-IDF environment is discovered and exported for you, so this runs from
a plain shell — no need to be inside an "ESP-IDF PowerShell" window first.

Usage
    python tools/build_release.py                       # esp32s3
    python tools/build_release.py -t esp32 -t esp32p4   # two targets
    python tools/build_release.py --all --clean         # all three, from scratch
    python tools/build_release.py --out dist/rc1        # somewhere else
    python tools/build_release.py --version 1.0.0       # override the stamp

With more than one target the images go into <out>/<target>/ so the names
never collide; with a single target they sit directly in <out>.
"""
from __future__ import annotations

import argparse
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import time
from pathlib import Path

PROJECT_DIR = Path(__file__).resolve().parent.parent
APP_CMAKELISTS = PROJECT_DIR / "app_osyntho" / "CMakeLists.txt"
DEFAULT_DRUMKIT = PROJECT_DIR / "tools" / "out" / "drumkit.bin"

TARGETS = ("esp32s3", "esp32", "esp32p4")
DEFAULT_TARGET = "esp32s3"
IS_WINDOWS = os.name == "nt"

# Colour only when a terminal is listening; a redirected log should stay clean.
_TTY = sys.stdout.isatty()
_C = {k: v if _TTY else "" for k, v in
      {"red": "\033[0;31m", "green": "\033[0;32m", "yellow": "\033[1;33m",
       "cyan": "\033[0;36m", "off": "\033[0m"}.items()}


def info(msg: str) -> None:
    print(f"{_C['green']}[release] {msg}{_C['off']}", flush=True)


def step(msg: str) -> None:
    print(f"{_C['cyan']}[release] >>> {msg}{_C['off']}", flush=True)


def warn(msg: str) -> None:
    print(f"{_C['yellow']}[release] WARNING: {msg}{_C['off']}", flush=True)


class Fail(RuntimeError):
    """Anything that should stop the run with a message instead of a traceback."""


# ── Version ───────────────────────────────────────────────────────────────────

def read_version() -> str:
    """APP_VERSION out of app_osyntho/CMakeLists.txt — see the module docstring."""
    try:
        text = APP_CMAKELISTS.read_text(encoding="utf-8", errors="replace")
    except OSError as exc:
        raise Fail(f"cannot read {APP_CMAKELISTS}: {exc}") from exc
    m = re.search(r"^\s*set\(\s*APP_VERSION\s+([0-9][0-9.]*)\s*\)",
                  text, re.MULTILINE)
    if not m:
        raise Fail(f"no 'set(APP_VERSION ...)' found in {APP_CMAKELISTS} — "
                   f"pass --version explicitly")
    return m.group(1)


def git_describe() -> str:
    """Short commit for the manifest, '-dirty' when the tree is not clean."""
    def git(*args: str) -> str | None:
        try:
            out = subprocess.run(("git", *args), cwd=PROJECT_DIR, check=True,
                                 capture_output=True, text=True,
                                 encoding="utf-8", errors="replace")
        except (OSError, subprocess.CalledProcessError):
            return None
        return out.stdout.strip()

    head = git("rev-parse", "--short", "HEAD")
    if head is None:
        return "unknown"
    dirty = git("status", "--porcelain", "--untracked-files=no")
    return head + ("-dirty" if dirty else "")


# ── Finding ESP-IDF ───────────────────────────────────────────────────────────
#
# There are two kinds of install in the wild and they activate differently:
#
#   * the classic one — `install.sh`/`install.bat` inside the IDF checkout,
#     tools under IDF_TOOLS_PATH (~/.espressif by default), activated by
#     export.sh / export.bat;
#
#   * the ESP-IDF Installation Manager (EIM) one, which puts the IDF and its
#     tools wherever it likes (C:\esp\<ver>\esp-idf + C:\Espressif\tools here),
#     records them in eim_idf.json, and ships a per-version activation script.
#     Its tools are NOT registered with idf_tools.py, so export.bat cannot
#     activate it: that script looks for a virtualenv named after whatever
#     `python` happens to be on PATH and gives up when it is missing. Going
#     through the recorded activation script is the only thing that works.
#
# So: find the IDF directory first, then look for an install record describing
# how to activate that particular directory.

class IdfInstall:
    """Where ESP-IDF is and how to turn it on."""

    def __init__(self, path: Path, source: str, tools_path: Path | None = None,
                 python: Path | None = None, activation: Path | None = None):
        self.path = path
        self.source = source
        self.tools_path = tools_path
        self.python = python
        self.activation = activation


def _is_idf(path: Path) -> bool:
    return (path / "tools" / "idf.py").is_file()


def _idf_from_vscode() -> Path | None:
    """.vscode/settings.json knows where this repo's IDF lives (idf.currentSetup)."""
    settings = PROJECT_DIR / ".vscode" / "settings.json"
    try:
        data = json.loads(settings.read_text(encoding="utf-8", errors="replace"))
    except (OSError, ValueError):
        return None
    value = data.get("idf.currentSetup")
    return Path(value) if isinstance(value, str) and value else None


def _eim_registries() -> list[Path]:
    home = Path.home()
    roots = [home / ".espressif" / "tools", home / ".espressif"]
    if os.environ.get("IDF_TOOLS_PATH"):
        roots.insert(0, Path(os.environ["IDF_TOOLS_PATH"]))
    if IS_WINDOWS:
        roots += [Path("C:/Espressif/tools"), Path("C:/Espressif")]
    else:
        roots += [Path("/opt/esp/tools")]
    return [root / "eim_idf.json" for root in roots]


def _eim_installs() -> list[dict]:
    """Every install EIM knows about, selected one first."""
    for registry in _eim_registries():
        try:
            data = json.loads(registry.read_text(encoding="utf-8",
                                                 errors="replace"))
        except (OSError, ValueError):
            continue
        installed = data.get("idfInstalled")
        if not isinstance(installed, list) or not installed:
            continue
        selected = data.get("idfSelectedId")
        return sorted(installed, key=lambda e: e.get("id") != selected)
    return []


def _eim_record(entry: dict, path: Path) -> IdfInstall:
    def maybe(key: str) -> Path | None:
        value = entry.get(key)
        return Path(value) if isinstance(value, str) and value else None

    return IdfInstall(path, source=f"EIM install {entry.get('name', '?')}",
                      tools_path=maybe("idfToolsPath"), python=maybe("python"),
                      activation=maybe("activationScript"))


def _idf_candidates() -> list[Path]:
    home = Path.home()
    globs: list[tuple[Path, str]] = []
    if IS_WINDOWS:
        globs += [(Path("C:/esp"), "*/esp-idf"), (Path("C:/esp"), "esp-idf"),
                  (Path("C:/Espressif/frameworks"), "esp-idf-*"),
                  (home / "esp", "*/esp-idf"), (home / "esp", "esp-idf")]
    else:
        globs += [(home / "esp", "esp-idf"), (home / "esp", "*/esp-idf"),
                  (Path("/opt"), "esp-idf"), (Path("/opt/esp"), "idf")]
    found: list[Path] = []
    for root, pattern in globs:
        if root.is_dir():
            found += sorted(p for p in root.glob(pattern) if _is_idf(p))
    return found


def _same_dir(a: Path, b: Path) -> bool:
    try:
        return a.resolve() == b.resolve()
    except OSError:
        return False


def find_idf(explicit: str | None) -> IdfInstall:
    eim = _eim_installs()

    def describe(path: Path, source: str) -> IdfInstall:
        """Attach the EIM record for this directory, when there is one."""
        for entry in eim:
            recorded = entry.get("path")
            if isinstance(recorded, str) and _same_dir(Path(recorded), path):
                install = _eim_record(entry, path)
                info(f"ESP-IDF from {source}: {path} ({install.source})")
                return install
        info(f"ESP-IDF from {source}: {path}")
        return IdfInstall(path, source=source)

    if explicit:
        path = Path(explicit).expanduser().resolve()
        if not _is_idf(path):
            raise Fail(f"--idf-path {path} has no tools/idf.py")
        return describe(path, "--idf-path")

    for source, raw in (("$IDF_PATH", os.environ.get("IDF_PATH")),
                        (".vscode/settings.json", _idf_from_vscode())):
        if not raw:
            continue
        path = Path(raw).expanduser()
        if _is_idf(path):
            return describe(path.resolve(), source)
        warn(f"{source} points at {path}, which has no tools/idf.py — ignoring")

    for entry in eim:  # selected install first
        raw = entry.get("path")
        if isinstance(raw, str) and _is_idf(Path(raw)):
            return describe(Path(raw).resolve(), "eim_idf.json")

    candidates = _idf_candidates()
    if not candidates:
        raise Fail("no ESP-IDF install found. Set IDF_PATH or pass --idf-path.")
    # Newest install wins when several are side by side (C:\esp\v5.3, v6.0.1, …).
    if len(candidates) > 1:
        info(f"found {len(candidates)} ESP-IDF installs, using the newest")
    return describe(candidates[-1].resolve(), "the usual install locations")


# ── Exporting the ESP-IDF environment ─────────────────────────────────────────

def already_exported(idf_path: Path) -> bool:
    """True when this shell already ran export.sh / the IDF PowerShell profile."""
    current = os.environ.get("IDF_PATH")
    if not current or not os.environ.get("IDF_PYTHON_ENV_PATH"):
        return False
    return _same_dir(Path(current), idf_path)


def _parse_pairs(text: str, sep: str | None) -> dict[str, str]:
    chunks = text.split(sep) if sep else text.splitlines()
    pairs = {}
    for chunk in chunks:
        key, eq, value = chunk.partition("=")
        if eq and key:
            pairs[key] = value if sep else value.rstrip("\r\n")
    return pairs


def _run_capture(cmd: list[str]) -> subprocess.CompletedProcess:
    try:
        return subprocess.run(cmd, capture_output=True, text=True,
                              encoding="utf-8", errors="replace")
    except OSError as exc:
        raise Fail(f"cannot run {cmd[0]}: {exc}") from exc


def _env_from_activation(script: Path) -> dict[str, str] | None:
    """Environment from an EIM activation script.

    The PowerShell flavour takes -e and prints its variables instead of setting
    them, which is exactly what is wanted here — but its PATH line carries only
    the IDF prefix (SYSTEM_PATH is a snapshot of the PATH at install time), so
    the prefix goes in front of the PATH this process actually has.
    """
    if script.suffix.lower() == ".ps1":
        shell = shutil.which("powershell") or shutil.which("pwsh")
        if not shell:
            return None
        proc = _run_capture([shell, "-NoProfile", "-ExecutionPolicy", "Bypass",
                             "-File", str(script), "-e"])
        pairs = _parse_pairs(proc.stdout, None)
        if "IDF_PATH" not in pairs:
            return None
        env = dict(os.environ)
        prefix = pairs.pop("PATH", "")
        fallback = pairs.pop("SYSTEM_PATH", "")
        env.update(pairs)
        base = env.get("PATH") or fallback
        env["PATH"] = os.pathsep.join(p for p in (prefix, base) if p)
        return env

    # POSIX flavour: an ordinary script to source, so dump the whole env.
    proc = _run_capture(["bash", "-c",
                         f'. "{script}" >/dev/null 2>&1 && env -0'])
    env = _parse_pairs(proc.stdout, "\0")
    return env if "IDF_PATH" in env else None


def _env_from_export(idf_path: Path) -> dict[str, str] | None:
    """Environment from the IDF checkout's own export script."""
    if IS_WINDOWS:
        script = idf_path / "export.bat"
        # /d skips AutoRun scripts that would otherwise print into `set`.
        cmd = ["cmd", "/d", "/s", "/c", f'call "{script}" >nul 2>&1 && set']
        sep = None
    else:
        script = idf_path / "export.sh"
        cmd = ["bash", "-c", f'. "{script}" >/dev/null 2>&1 && env -0']
        sep = "\0"
    if not script.is_file():
        return None
    env = _parse_pairs(_run_capture(cmd).stdout, sep)
    # export.bat sets IDF_PATH before it activates anything, so IDF_PATH alone
    # does not mean it worked — the virtualenv is the thing that proves it.
    return env if env.get("IDF_PYTHON_ENV_PATH") else None


def export_env(install: IdfInstall) -> dict[str, str]:
    """Capture the environment ESP-IDF wants, without staying inside a shell.

    Capturing it — rather than running each build through a shell — keeps every
    later command a plain argv list, so nothing in a Windows path with a space
    in it has to survive two rounds of quoting.
    """
    if already_exported(install.path):
        info("ESP-IDF environment already exported in this shell")
        env = dict(os.environ)
    else:
        step("exporting the ESP-IDF environment")
        env = None
        if install.activation and install.activation.is_file():
            env = _env_from_activation(install.activation)
            if env is None:
                warn(f"{install.activation.name} produced no environment, "
                     f"falling back to the export script")
        if env is None:
            env = _env_from_export(install.path)
        if env is None:
            raise Fail(
                f"could not activate the ESP-IDF at {install.path}.\n"
                f"Open the IDF shell yourself and re-run this script from "
                f"there, or reinstall the toolchain "
                f"({'install.bat' if IS_WINDOWS else 'install.sh'} in the IDF "
                f"directory).")

    env["IDF_PATH"] = str(install.path)
    if install.tools_path:
        env.setdefault("IDF_TOOLS_PATH", str(install.tools_path))

    # Fail here, with a sentence, rather than inside cmake ten seconds later.
    missing = [tool for tool in ("cmake", "ninja")
               if not shutil.which(tool, path=env.get("PATH"))]
    if missing:
        raise Fail(f"{', '.join(missing)} not on the exported PATH — the "
                   f"toolchain at {install.path} looks incomplete")
    return env


def idf_python(install: IdfInstall, env: dict[str, str]) -> str:
    """The interpreter idf.py must run under: IDF's virtualenv, never the system one."""
    if install.python and install.python.is_file():
        return str(install.python)
    venv = env.get("IDF_PYTHON_ENV_PATH")
    if venv:
        exe = (Path(venv) / "Scripts" / "python.exe" if IS_WINDOWS
               else Path(venv) / "bin" / "python")
        if exe.is_file():
            return str(exe)
    return shutil.which("python", path=env.get("PATH")) or sys.executable


def idf_version(idf_path: Path) -> str:
    version_txt = idf_path / "version.txt"
    if version_txt.is_file():
        return version_txt.read_text(encoding="utf-8", errors="replace").strip()
    try:
        out = subprocess.run(("git", "describe", "--tags", "--dirty"),
                             cwd=idf_path, check=True, capture_output=True,
                             text=True, encoding="utf-8", errors="replace")
        return out.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


def run_idf(python: str, env: dict[str, str], args: list[str]) -> None:
    """Invoke idf.py with the exported env, output streaming to the console."""
    cmd = [python, str(Path(env["IDF_PATH"]) / "tools" / "idf.py"), *args]
    proc = subprocess.run(cmd, cwd=PROJECT_DIR, env=env)
    if proc.returncode != 0:
        raise Fail(f"idf.py {' '.join(args)} failed (exit {proc.returncode})")


# ── Build ─────────────────────────────────────────────────────────────────────

def cached_target(build_dir: Path) -> str | None:
    cache = build_dir / "CMakeCache.txt"
    if not cache.is_file():
        return None
    m = re.search(r"^IDF_TARGET:STRING=(\S+)",
                  cache.read_text(encoding="utf-8", errors="replace"),
                  re.MULTILINE)
    return m.group(1) if m else None


def build(target: str, python: str, env: dict[str, str], clean: bool,
          drumkit: Path | None) -> Path:
    build_dir = PROJECT_DIR / f"build_release_{target}"

    # A build dir left over from a different chip cannot be reconfigured in
    # place — CMake caches the toolchain. Start it over rather than emitting a
    # confusing failure half an hour into the run.
    stale = cached_target(build_dir)
    if clean or (stale is not None and stale != target):
        if build_dir.exists():
            reason = "--clean" if clean else f"was configured for {stale}"
            step(f"wiping {build_dir.name} ({reason})")
            shutil.rmtree(build_dir)
    build_dir.mkdir(parents=True, exist_ok=True)

    # SDKCONFIG inside the build dir: the root sdkconfig is the bench config
    # and stays untouched. Absent, ESP-IDF generates this one from
    # sdkconfig.defaults + sdkconfig.defaults.<target>.
    args = ["-B", str(build_dir),
            "-D", f"SDKCONFIG={build_dir / 'sdkconfig'}",
            "-D", f"IDF_TARGET={target}"]
    if drumkit is not None:
        args += ["-D", f"OSYNTH_DRUMKIT={drumkit}"]
    args.append("build")

    env = dict(env)
    env["IDF_TARGET"] = target

    step(f"building {target}")
    started = time.monotonic()
    run_idf(python, env, args)
    info(f"{target} built in {time.monotonic() - started:.0f}s")
    return build_dir


# ── Collect ───────────────────────────────────────────────────────────────────

def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1 << 20), b""):
            digest.update(block)
    return digest.hexdigest()


def collect(build_dir: Path, out_dir: Path, target: str, version: str,
            idf_ver: str) -> dict:
    """Copy the flashable images out of build_dir and describe them."""
    args_file = build_dir / "flasher_args.json"
    if not args_file.is_file():
        raise Fail(f"{args_file} missing — did the build finish?")
    flasher = json.loads(args_file.read_text(encoding="utf-8"))

    out_dir.mkdir(parents=True, exist_ok=True)

    # Sorted by offset so the manifest and the flash command read in the same
    # order the chip lays them out.
    images = sorted(flasher["flash_files"].items(), key=lambda kv: int(kv[0], 16))
    entries = []
    for offset, rel in images:
        src = build_dir / rel
        if not src.is_file():
            raise Fail(f"{src} listed in flasher_args.json but not on disk")
        dst = out_dir / Path(rel).name
        shutil.copy2(src, dst)
        entries.append({"offset": offset, "file": dst.name,
                        "size": dst.stat().st_size, "sha256": sha256(dst)})
        info(f"  {offset:>8}  {dst.name}  ({dst.stat().st_size / 1024:.0f} KB)")

    extra = flasher.get("extra_esptool_args", {})
    chip = extra.get("chip", target)
    settings = flasher.get("flash_settings", {})

    # esptool v5 spelling (write-flash, hard-reset) — that is what IDF v5.3+
    # ships and what flasher_args.json itself is written in.
    command = ["esptool", "--chip", chip, "--baud", "460800"]
    if extra.get("before"):
        command += ["--before", extra["before"]]
    if extra.get("after"):
        command += ["--after", extra["after"]]
    command += ["write-flash"]
    for key, flag in (("flash_mode", "--flash-mode"),
                      ("flash_size", "--flash-size"),
                      ("flash_freq", "--flash-freq")):
        if settings.get(key):
            command += [flag, settings[key]]
    for entry in entries:
        command += [entry["offset"], entry["file"]]
    command_line = " ".join(command)

    manifest = {
        "project": "osynth",
        "version": version,
        "target": target,
        "chip": chip,
        "git": git_describe(),
        "idf_version": idf_ver,
        "built_utc": time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()),
        "flash_settings": settings,
        "images": entries,
        "flash_command": command_line,
    }
    (out_dir / "manifest.json").write_text(
        json.dumps(manifest, indent=2) + "\n", encoding="utf-8")

    table = "\n".join(f"  {e['offset']:>8}  {e['file']}" for e in entries)
    (out_dir / "flash.txt").write_text(
        f"osynth {version} — {chip}\n"
        f"built {manifest['built_utc']} from {manifest['git']} "
        f"with ESP-IDF {idf_ver}\n\n"
        f"Offsets:\n{table}\n\n"
        f"Flash with esptool (pass -p COM8 / -p /dev/ttyUSB0 to pick a port):\n\n"
        f"  {command_line}\n\n"
        f"Erase first if the board is coming from a different build:\n\n"
        f"  esptool --chip {chip} erase-flash\n",
        encoding="utf-8")

    return manifest


# ── Main ──────────────────────────────────────────────────────────────────────

def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Build the osynth firmware and collect a flashable release.",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        epilog="Images land in private_releases/firmware<version>/ by default.")
    parser.add_argument("-t", "--target", action="append", choices=TARGETS,
                        metavar="CHIP",
                        help=f"chip to build, repeatable ({', '.join(TARGETS)}); "
                             f"default {DEFAULT_TARGET}")
    parser.add_argument("--all", action="store_true",
                        help="build every supported chip")
    parser.add_argument("--version", metavar="X.Y.Z",
                        help="override the version read from "
                             "app_osyntho/CMakeLists.txt")
    parser.add_argument("--out", metavar="DIR",
                        help="output directory (default "
                             "private_releases/firmware<version>)")
    parser.add_argument("--idf-path", metavar="DIR",
                        help="ESP-IDF install to use (default: $IDF_PATH, then "
                             ".vscode/settings.json, then the usual locations)")
    parser.add_argument("--drumkit", metavar="BIN",
                        help=f"drum kit image to embed (default "
                             f"{DEFAULT_DRUMKIT.relative_to(PROJECT_DIR).as_posix()})")
    parser.add_argument("--clean", action="store_true",
                        help="wipe each build directory first")
    parser.add_argument("--check-env", action="store_true",
                        help="report the ESP-IDF this would use and exit "
                             "without building")
    parser.add_argument("--no-kit-check", action="store_true",
                        help="build even without a drum kit image (mute drums)")
    return parser.parse_args(argv)


def check_env(args: argparse.Namespace) -> int:
    """--check-env: everything the build depends on, without spending an hour on it."""
    install = find_idf(args.idf_path)
    env = export_env(install)
    python = idf_python(install, env)
    info(f"IDF_PATH            {env['IDF_PATH']}")
    info(f"version             {idf_version(install.path)}")
    info(f"IDF_TOOLS_PATH      {env.get('IDF_TOOLS_PATH', '(unset)')}")
    info(f"python              {python}")
    for tool in ("cmake", "ninja", "ccache",
                 "xtensa-esp32s3-elf-gcc", "riscv32-esp-elf-gcc"):
        found = shutil.which(tool, path=env.get("PATH"))
        line = f"{tool:<23} {found or '(not found)'}"
        info(line) if found else warn(line)
    return 0


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)

    if args.check_env:
        return check_env(args)

    targets = list(TARGETS) if args.all else (args.target or [DEFAULT_TARGET])
    targets = list(dict.fromkeys(targets))  # de-dupe, keep order

    version = args.version or read_version()
    out_root = (Path(args.out).expanduser().resolve() if args.out
                else PROJECT_DIR / "private_releases" / f"firmware{version}")

    drumkit = Path(args.drumkit).expanduser().resolve() if args.drumkit else None
    kit_path = drumkit or DEFAULT_DRUMKIT
    if not kit_path.is_file():
        # Not fatal by request only: the build links fine without a kit and the
        # drum bus is simply silent, which is far easier to ship than to notice.
        message = (f"no drum kit image at {kit_path} — the drum bus would be "
                   f"silent. Build one with:\n"
                   f"    python tools/gen_drumkit.py --pack opendrums "
                   f"--out tools/out/drumkit.bin")
        if args.no_kit_check:
            warn(message)
        else:
            raise Fail(message + "\n(or pass --no-kit-check to ship it anyway)")

    install = find_idf(args.idf_path)
    env = export_env(install)
    python = idf_python(install, env)
    idf_ver = idf_version(install.path)
    info(f"osynth {version} · ESP-IDF {idf_ver} · targets: {', '.join(targets)}")
    info(f"output: {out_root}")

    manifests = []
    for target in targets:
        build_dir = build(target, python, env, args.clean, drumkit)
        # One target keeps the images at the top level, as asked; several would
        # collide there (every chip produces the same three names).
        out_dir = out_root if len(targets) == 1 else out_root / target
        step(f"collecting {target} into {out_dir}")
        manifests.append(collect(build_dir, out_dir, target, version, idf_ver))

    print()
    info(f"release {version} ready in {out_root}")
    for manifest in manifests:
        total = sum(e["size"] for e in manifest["images"])
        info(f"  {manifest['target']:<9} {len(manifest['images'])} images, "
             f"{total / 1024:.0f} KB total")
    return 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except Fail as exc:
        print(f"{_C['red']}[release] ERROR: {exc}{_C['off']}", file=sys.stderr)
        sys.exit(1)
    except KeyboardInterrupt:
        print(f"\n{_C['yellow']}[release] interrupted{_C['off']}", file=sys.stderr)
        sys.exit(130)
