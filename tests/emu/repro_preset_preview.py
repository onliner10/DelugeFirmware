"""Repro: notes dropped / preview not taking effect while scrolling multisampled synth presets.

Boots a new song, programs a 16-step melody into the synth clip, plays it, then turns SELECT in clip view to
step through multisampled presets (one sample per semitone, each preset with its own WAVs), and finally revisits
them once their samples are in RAM.

Needs the PREVIEW_DIAG log lines from the diagnostic firmware build. Usage:
    python3 tests/emu/repro_preset_preview.py <card dir> [--name N] [--audio wav]
"""

from __future__ import annotations

import argparse
import re
import sys
import time
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from deluge_emu.session import EmuSession

# (column, row) pads: one note per 16th step, rows 0..7 = different pitches
MELODY = [
    (c, r) for c, r in zip(range(16), [0, 3, 5, 7, 2, 6, 1, 4, 7, 5, 3, 0, 6, 2, 4, 1])
]


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("card")
    ap.add_argument("--name", default="preset-preview")
    ap.add_argument("--audio", default=None, help="run.sh --audio driver, e.g. wav")
    ap.add_argument("--presets", type=int, default=4)
    ap.add_argument(
        "--dwell", type=float, default=3.0, help="seconds to listen to each preset"
    )
    args = ap.parse_args()

    with EmuSession.start(
        name=args.name, sd=args.card, boot_wait=8.0, audio=args.audio
    ) as emu:
        dz = emu.dz
        print("--- after boot:\n" + dz.oled_text(), flush=True)

        for c, r in MELODY:
            dz.pad(c, r)
            emu.wait(0.05)
        dz.press("PLAY")
        emu.wait(2.0)
        print("--- playing:\n" + dz.oled_text(), flush=True)

        marks = []

        def step(label: str, delta: int):
            before = len(emu.log())
            t0 = time.monotonic()
            dz.turn("SELECT_ENC", delta)
            emu.wait(args.dwell)
            chunk = emu.log()[before:]
            swapped = re.findall(r"PREVIEW_DIAG preset swapped in[^:]*: (\S*)", chunk)
            drops = re.findall(r"PREVIEW_DIAG dropped notes[^:]*: (\d+)", chunk)
            stuck = "PREVIEW_DIAG STUCK" in chunk
            marks.append((label, swapped, drops[-1] if drops else None, stuck))
            print(
                f"[{label}] turn {delta:+d} at +{t0:.1f}: swapped={swapped} total_drops={drops[-1] if drops else '-'}"
                f"{' STUCK' if stuck else ''}",
                flush=True,
            )

        for i in range(args.presets):
            step(f"first visit {i + 1}", +1)
        for i in range(args.presets):
            step(f"revisit {i + 1}", -1)

        dz.press("PLAY")
        emu.wait(0.5)
        print("\n--- PREVIEW_DIAG log:")
        for line in emu.log().splitlines():
            if "PREVIEW_DIAG" in line:
                print("  " + line.strip())
        emu.snapshot(prefix="end-")
    return 0


if __name__ == "__main__":
    sys.exit(main())
