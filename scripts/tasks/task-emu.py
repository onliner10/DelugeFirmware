#! /usr/bin/env python3
"""`./dbt emu ...` - run the firmware in the delugemu emulator (see tests/emu/README.md)."""

import argparse
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[2] / "tests" / "emu"))


def argparser() -> argparse.ArgumentParser:
    from deluge_emu.cli import parser

    p = parser()
    p.prog = "emu"
    p.group = "Development"
    return p


def main() -> int:
    from deluge_emu.cli import main as emu_main

    return emu_main(sys.argv[1:])


if __name__ == "__main__":
    sys.exit(main())
