"""JSON scenarios: scripted bug repros / explorations, no Python needed.

    {"steps": [
        {"press": "PLAY"},
        {"wait": 1},
        {"assert_led": ["PLAY", true]},
        {"load_song": "/SONGS/MULTISAMPLE.XML"},
        {"wait_log": "some regex"},
        {"snapshot": "after-load"}
    ]}

All of delugemu's scenario verbs work (wait, press, key, turn, pad, hold,
release, screenshot, assert_led, assert_segment, assert_oled_contains), plus:

    load_song PATH       open the browser and load a song by card path
    type TEXT            type on the pad-grid keyboard
    chord [NAMES]        hold several controls together (e.g. ["SHIFT", "SAVE"])
    wait_log REGEX       wait (default 30 s) for a firmware log line; or {"regex":..,"timeout":..}
    assert_log REGEX     the firmware log (so far) matches
    assert_no_log REGEX  the firmware log (so far) does not match
    wait_sd_idle SECS    wait until the SD card has been quiet for SECS (default 1)
    snapshot PREFIX      write PREFIX-oled.txt / -screen.png / -state.json

The session checks for crashes between every step, so a crash fails the
scenario at the step that caused it.
"""

from __future__ import annotations

import re

from . import paths
from .session import EmuCrash

paths.ensure_pylib_on_path()
from delugemu import scenario as dz_scenario


def _step(emu, verb, arg):
    dz = emu.dz
    if verb == "wait":
        emu.wait(float(arg))  # crash-aware wait
    elif verb == "load_song":
        dz.load_song(arg)
    elif verb == "type":
        dz.type_text(arg)
    elif verb == "chord":
        with dz.latch(arg):
            dz.wait(0.2)
    elif verb == "wait_log":
        if isinstance(arg, dict):
            emu.wait_for_log(arg["regex"], float(arg.get("timeout", 30)))
        else:
            emu.wait_for_log(arg)
    elif verb == "assert_log":
        assert re.search(arg, emu.log()), f"firmware log does not match {arg!r}"
    elif verb == "assert_no_log":
        m = re.search(arg, emu.log())
        assert not m, f"firmware log unexpectedly matched {arg!r}: {m.group(0)!r}"
    elif verb == "wait_sd_idle":
        dz.wait_sd_idle(quiet=float(arg or 1.0))
    elif verb == "snapshot":
        emu.snapshot(prefix=f"{arg}-")
    elif verb == "screenshot":
        dz.screenshot(str(emu.artifacts / arg))
    else:
        dz_scenario._apply(dz, verb, arg)


def run_steps(emu, scenario: dict) -> None:
    for i, step in enumerate(scenario.get("steps", [])):
        if len(step) != 1:
            raise ValueError(f"step {i} must have exactly one verb: {step!r}")
        ((verb, arg),) = step.items()
        try:
            _step(emu, verb, arg)
            emu.check()
        except EmuCrash:
            raise
        except AssertionError as e:
            emu.snapshot(prefix="failure-")
            raise AssertionError(f"step {i} ({verb} {arg!r}) failed: {e}") from e
