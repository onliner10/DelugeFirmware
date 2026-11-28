"""Locations of everything the emulator harness touches.

All paths are absolute and derived from the firmware repo root, so the harness
works no matter which directory it is invoked from.
"""

from __future__ import annotations

import os
import sys
from pathlib import Path

FW_ROOT = Path(__file__).resolve().parents[3]

# The pinned delugemu revision lives in a checked-in lock file.
LOCK_FILE = FW_ROOT / "scripts" / "emu" / "delugemu.lock"

# Everything the harness downloads or generates lives under these
# (both are gitignored).
EMU_HOME = FW_ROOT / ".emu"
BUILD_DIR = FW_ROOT / "build-emu"
ARTIFACTS_DIR = BUILD_DIR / "artifacts"
SD_CACHE_DIR = BUILD_DIR / "sd"
LIVE_DIR = BUILD_DIR / "live"

FIXTURES_DIR = FW_ROOT / "tests" / "emu" / "fixtures"

TOOLCHAIN_VERSION = (FW_ROOT / "toolchain" / "REQUIRED_VERSION").read_text().strip()


def _toolchain_dir() -> Path:
    # Same naming as scripts/toolchain/dbtenv.sh: `uname -s`-`uname -m`, lowercased.
    import platform

    triple = f"{platform.system()}-{platform.machine()}".lower()
    return FW_ROOT / "toolchain" / f"v{TOOLCHAIN_VERSION}" / triple


TOOLCHAIN_DIR = _toolchain_dir()
GDB = TOOLCHAIN_DIR / "arm-none-eabi-gcc" / "bin" / "arm-none-eabi-gdb"
NM = TOOLCHAIN_DIR / "arm-none-eabi-gcc" / "bin" / "arm-none-eabi-nm"


def read_lock() -> dict[str, str]:
    out = {}
    for line in LOCK_FILE.read_text().splitlines():
        line = line.strip()
        if line and not line.startswith("#") and "=" in line:
            k, v = line.split("=", 1)
            out[k.strip()] = v.strip()
    return out


def delugemu_root() -> Path:
    """$DELUGEMU_ROOT (an existing delugemu checkout) or the managed clone."""
    env = os.environ.get("DELUGEMU_ROOT")
    if env:
        return Path(env).resolve()
    return EMU_HOME / "delugemu"


def qemu_binary() -> Path:
    return delugemu_root() / "qemu" / "build" / "qemu-system-arm"


def default_elf(config: str = "Debug") -> Path:
    return BUILD_DIR / config / "deluge.elf"


def ensure_pylib_on_path() -> None:
    """Make delugemu's stdlib-only Python package importable."""
    pylib = str(delugemu_root() / "pylib")
    if pylib not in sys.path:
        sys.path.insert(0, pylib)
