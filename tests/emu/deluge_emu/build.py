"""Fetch/build the pinned emulator and build the emulator flavour of the firmware."""

from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

from . import paths


def log(msg: str) -> None:
    print(f"[emu] {msg}", flush=True)


def _run(cmd, cwd=None, env=None) -> None:
    log("$ " + " ".join(str(c) for c in cmd))
    subprocess.run([str(c) for c in cmd], cwd=cwd, env=env, check=True)


# --------------------------------------------------------------------------
# Emulator
# --------------------------------------------------------------------------

STAMP = paths.EMU_HOME / "delugemu.stamp"


def check_host_tools() -> list[str]:
    """Return a list of human-readable problems (empty when all good)."""
    problems = []
    for tool, pkg in (
        ("mkfs.fat", "dosfstools"),
        ("mcopy", "mtools"),
        ("ninja", "ninja-build"),
    ):
        if shutil.which(tool) is None:
            problems.append(f"`{tool}` not found (install the `{pkg}` package)")
    try:
        subprocess.run(
            ["pkg-config", "--exists", "glib-2.0", "pixman-1"],
            check=True,
            capture_output=True,
        )
    except (OSError, subprocess.CalledProcessError):
        problems.append(
            "glib-2.0 / pixman-1 dev packages not found (needed to build QEMU)"
        )
    return problems


def emulator_ready() -> bool:
    return paths.qemu_binary().exists()


def setup_emulator(force: bool = False) -> None:
    """Clone delugemu at the pinned revision and build qemu-system-arm.

    With $DELUGEMU_ROOT set, that checkout is used as-is (only built if needed).
    """
    root = paths.delugemu_root()
    managed = "DELUGEMU_ROOT" not in os.environ
    lock = paths.read_lock()

    for p in check_host_tools():
        log(f"WARNING: {p}")

    if managed:
        stamp = STAMP.read_text().strip() if STAMP.exists() else ""
        if not force and stamp == lock["rev"] and emulator_ready():
            log(f"delugemu {lock['rev'][:10]} already built at {root}")
            return
        paths.EMU_HOME.mkdir(parents=True, exist_ok=True)
        if not (root / ".git").exists():
            root.mkdir(parents=True, exist_ok=True)
            _run(["git", "init", "-q"], cwd=root)
            _run(["git", "remote", "add", "origin", lock["url"]], cwd=root)
        _run(["git", "fetch", "--depth", "1", "origin", lock["rev"]], cwd=root)
        _run(["git", "checkout", "-q", "--force", "FETCH_HEAD"], cwd=root)
    else:
        log(f"Using DELUGEMU_ROOT={root} (not pinned)")
        if emulator_ready() and not force:
            log("qemu-system-arm already built")
            return

    # delugemu's own scripts fetch the pinned QEMU submodule and build it
    # (headless works without SDL/GTK/audio dev packages).
    _run([root / "scripts" / "bootstrap.sh"], cwd=root)
    _run([root / "scripts" / "build.sh"], cwd=root)
    if managed:
        STAMP.write_text(lock["rev"] + "\n")
    log(f"Emulator ready: {paths.qemu_binary()}")


# --------------------------------------------------------------------------
# Firmware
# --------------------------------------------------------------------------


def _toolchain_env() -> dict[str, str]:
    env = dict(os.environ)
    env.setdefault("DBT_TOOLCHAIN_PATH", str(paths.FW_ROOT))
    env.setdefault("DELUGE_FW_ROOT", str(paths.FW_ROOT))
    extra = [
        paths.TOOLCHAIN_DIR / "cmake" / "bin",
        paths.TOOLCHAIN_DIR / "ninja-build" / "bin",
        paths.TOOLCHAIN_DIR / "arm-none-eabi-gcc" / "bin",
    ]
    env["PATH"] = os.pathsep.join(
        [str(p) for p in extra if p.exists()] + [env.get("PATH", "")]
    )
    return env


def build_firmware(config: str = "Debug") -> Path:
    """Build the firmware for the emulator into build-emu/<config>/deluge.elf.

    Differences from `./dbt build`: a separate build dir, and RTT disabled so
    D_PRINTLN output goes to the DIN-MIDI UART, which delugemu prints to the
    session's serial log (RTT is invisible inside QEMU).
    """
    env = _toolchain_env()
    if not (paths.BUILD_DIR / "build.ninja").exists():
        _run(
            [
                "cmake",
                "-B",
                paths.BUILD_DIR,
                "-S",
                paths.FW_ROOT,
                "-G",
                "Ninja Multi-Config",
                "-DCMAKE_CROSS_CONFIGS=all",
                "-DCMAKE_DEFAULT_CONFIGS=Debug;Release",
                "-DENABLE_RTT=OFF",
            ],
            env=env,
        )
    _run(["cmake", "--build", paths.BUILD_DIR, "--config", config], env=env)
    elf = paths.default_elf(config)
    log(f"Firmware ready: {elf}")
    return elf
