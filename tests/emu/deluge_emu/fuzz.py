"""Random-input fuzzer: drive the front panel with random gestures until the
firmware crashes or hangs, recording every raw input event so a failure can be
replayed.

    ./dbt emu fuzz --sd basic --seed 7 --actions 400
    ./dbt emu fuzz --replay build-emu/artifacts/fuzz-7/events.json

Gestures are what a user does: tap a button, tap a pad, turn an encoder, hold a
button (SHIFT, a mod button, an encoder push...) while doing something else,
hold a pad while turning a knob. Every gesture is a list of raw events
(`button x y down|up`, `pad x y down|up`, `encoder i n`, `wait s`), written to
events.json as it happens, so even a run that kills the emulator leaves its
input history behind.

Hang detection: every --probe-every gestures the scheduler heartbeat
(taskManager.lastFinishTime, read from guest RAM) is checked; if it doesn't
change for 3 s the main loop is stuck and the run ends with status "hang",
naming the functions the CPU was sampled in.
"""

from __future__ import annotations

import bisect
import json
import random
import re
import subprocess
import time
from pathlib import Path
from typing import ClassVar

from . import paths

paths.ensure_pylib_on_path()
from delugemu._controls import CONTROLS

BUTTONS = {n: (c["col"], c["row"]) for n, c in CONTROLS.items()}
# Firmware encoder order (delugemu DelugeSession.encoder).
ENCODERS = {
    "Y_ENC": 0,
    "X_ENC": 1,
    "TEMPO_ENC": 2,
    "SELECT_ENC": 3,
    "MOD_ENCODER_1": 4,
    "MOD_ENCODER_0": 5,
}
MODIFIERS = ["SHIFT", "SHIFT", "SHIFT", "SELECT_ENC", "X_ENC", "Y_ENC", "AFFECT_ENTIRE", "LEARN",
             "SYNC_SCALING", "CROSS_SCREEN", "RECORD", "MOD0", "MOD3", "MOD5", "SAVE", "LOAD",
             "SESSION_VIEW", "TEMPO_ENC"]  # fmt: skip


class Fuzzer:
    def __init__(
        self,
        rng: random.Random,
        avoid: set[str] = frozenset(),
        weights: dict | None = None,
    ):
        self.rng = rng
        self.weights = dict(self.DEFAULT_WEIGHTS, **(weights or {}))
        self.buttons = [b for b in BUTTONS if b not in avoid]

    # each returns a list of raw events
    def tap_button(self, name=None):
        name = name or self.rng.choice(self.buttons)
        x, y = BUTTONS[name]
        return [
            ["button", x, y, 1],
            ["wait", self.rng.uniform(0.04, 0.15)],
            ["button", x, y, 0],
        ]

    def tap_pad(self):
        x, y = self.rng.randrange(18), self.rng.randrange(8)
        return [
            ["pad", x, y, 1],
            ["wait", self.rng.uniform(0.03, 0.3)],
            ["pad", x, y, 0],
        ]

    def turn(self):
        i = self.rng.choice(list(ENCODERS.values()))
        n = self.rng.choice([-8, -4, -2, -1, -1, 1, 1, 2, 4, 8])
        return [["encoder", i, n]]

    def inner(self):
        r = self.rng.random()
        if r < 0.35:
            return self.tap_pad()
        if r < 0.7:
            return self.turn()
        return self.tap_button()

    def hold_button(self):
        name = self.rng.choice(MODIFIERS)
        x, y = BUTTONS[name]
        ev = [["button", x, y, 1], ["wait", 0.08]]
        for _ in range(self.rng.randint(1, 3)):
            ev += self.inner() + [["wait", self.rng.uniform(0.02, 0.2)]]
        return ev + [["button", x, y, 0]]

    def hold_pad(self):
        x, y = self.rng.randrange(16), self.rng.randrange(8)
        ev = [["pad", x, y, 1], ["wait", 0.08]]
        for _ in range(self.rng.randint(1, 3)):
            ev += (self.turn() if self.rng.random() < 0.7 else self.tap_pad()) + [
                ["wait", 0.05]
            ]
        return ev + [["pad", x, y, 0]]

    DEFAULT_WEIGHTS: ClassVar[dict[str, int]] = {
        "button": 25,
        "pad": 20,
        "turn": 20,
        "hold_button": 20,
        "hold_pad": 15,
    }

    def gesture(self):
        w = self.weights
        kinds = list(w)
        kind = (
            self.rng.choices(kinds, weights=[w[k] for k in kinds])[0]
            if kinds != list(self.DEFAULT_WEIGHTS) or w != self.DEFAULT_WEIGHTS
            else None
        )
        if (
            kind is None
        ):  # default mix: keep the original draw so old seeds replay identically
            r = self.rng.random()
            kind = (
                "button"
                if r < 0.25
                else "pad"
                if r < 0.45
                else "turn"
                if r < 0.65
                else "hold_button"
                if r < 0.85
                else "hold_pad"
            )
        ev = {
            "button": self.tap_button,
            "pad": self.tap_pad,
            "turn": self.turn,
            "hold_button": self.hold_button,
            "hold_pad": self.hold_pad,
        }[kind]()
        return ev + [["wait", self.rng.choice([0.05, 0.1, 0.2, 0.4, 1.0])]]


def apply(emu, ev) -> None:
    dz = emu.dz
    kind = ev[0]
    if kind == "wait":
        emu.wait(ev[1])
    elif kind == "button":
        dz.button(ev[1], ev[2], bool(ev[3]))
    elif kind == "pad":
        dz.pad_event(ev[1], ev[2], bool(ev[3]))
    elif kind == "encoder":
        dz.encoder(ev[1], ev[2])
    else:
        raise ValueError(f"bad event {ev!r}")


# --------------------------------------------------------------------------
# hang detection
# --------------------------------------------------------------------------


class Symbols:
    def __init__(self, elf: Path):
        out = subprocess.run(
            [str(paths.NM), "-C", "-n", str(elf)],
            capture_output=True,
            text=True,
            check=True,
        ).stdout
        self.addrs, self.names = [], []
        for line in out.splitlines():
            parts = line.split(" ", 2)
            if len(parts) == 3 and parts[1] in "tTwW":
                self.addrs.append(int(parts[0], 16))
                self.names.append(parts[2])

    def name(self, addr: int) -> str:
        i = bisect.bisect_right(self.addrs, addr & ~1) - 1
        return self.names[i] if i >= 0 else hex(addr)


def sample_pcs(emu, n=12, gap=0.08) -> list[int]:
    pcs = []
    for _ in range(n):
        out = emu.dz.q.execute(
            "human-monitor-command", **{"command-line": "info registers"}
        )
        m = re.search(r"R15=([0-9a-f]{8})", out)
        if m:
            pcs.append(int(m.group(1), 16))
        time.sleep(gap)
    return pcs


def heartbeat_addr(elf: Path) -> int:
    """Address of taskManager.lastFinishTime: rewritten every time a scheduler
    task finishes, so it stops changing when the main loop is stuck."""
    out = subprocess.run(
        [
            str(paths.GDB),
            "-batch",
            "-nx",
            "-ex",
            "print/x &taskManager.lastFinishTime",
            str(elf),
        ],
        capture_output=True,
        text=True,
        check=True,
    ).stdout
    return int(re.search(r"= (0x[0-9a-f]+)", out).group(1), 16)


def read_heartbeat(emu, addr: int) -> str:
    out = emu.dz.q.execute(
        "human-monitor-command", **{"command-line": f"x /2wx {addr:#x}"}
    )
    return out.split(":", 1)[-1].strip()


def probe_hang(emu, syms: Symbols, addr: int, quiet: float = 3.0) -> str | None:
    """None if the main loop is alive, else a description of where it's stuck."""
    first = read_heartbeat(emu, addr)
    deadline = time.monotonic() + quiet
    while time.monotonic() < deadline:
        time.sleep(0.25)
        if read_heartbeat(emu, addr) != first:
            return None
    pcs = sample_pcs(emu, n=20)
    funcs = sorted({syms.name(pc) for pc in pcs})
    return f"main loop stalled >{quiet:.0f}s; CPU in: {'; '.join(funcs)[:400]}"


# --------------------------------------------------------------------------


def run(
    name: str,
    sd: str,
    seed: int,
    actions: int,
    elf: str | None = None,
    probe_every: int = 20,
    replay: list | None = None,
    avoid: set[str] = frozenset(),
    setup: list | None = None,
    weights: dict | None = None,
) -> dict:
    """Fuzz (or replay `replay`, a list of gestures). Returns result dict."""
    from .session import EmuCrash, EmuSession

    rng = random.Random(seed)
    fz = Fuzzer(rng, avoid, weights)
    elf_path = Path(elf) if elf else paths.default_elf()
    syms = Symbols(elf_path)
    hb = heartbeat_addr(elf_path)
    outcome = {"status": "ok", "seed": seed, "gestures": 0}
    with EmuSession.start(name=name, sd=sd, elf=elf_path) as emu:
        events_file = emu.artifacts / "events.json"
        done: list = []
        try:
            if setup:
                from .scenario import run_steps

                run_steps(emu, {"steps": setup})
            total = len(replay) if replay is not None else actions
            for g in range(total):
                gesture = replay[g] if replay is not None else fz.gesture()
                done.append(gesture)
                events_file.write_text(
                    json.dumps(
                        {"seed": seed, "sd": sd, "setup": setup, "gestures": done}
                    )
                    + "\n"
                )
                for ev in gesture:
                    apply(emu, ev)
                emu.check()
                outcome["gestures"] = g + 1
                if probe_every and (g + 1) % probe_every == 0:
                    stuck = probe_hang(emu, syms, hb)
                    if stuck:
                        outcome.update(status="hang", detail=stuck)
                        emu.snapshot(prefix="hang-")
                        break
            if outcome["status"] == "ok" and probe_every:
                stuck = probe_hang(emu, syms, hb)
                if stuck:
                    outcome.update(status="hang", detail=stuck)
                    emu.snapshot(prefix="hang-")
        except EmuCrash as e:
            outcome.update(status="crash", detail=e.summary)
        outcome["artifacts"] = str(emu.artifacts)
    (Path(outcome["artifacts"]) / "fuzz.json").write_text(
        json.dumps(outcome, indent=2) + "\n"
    )
    return outcome
