"""Replay minimized bug repros (tests/emu/repros/*.json) found by the fuzzer.

Each file is a `./dbt emu fuzz --replay` recording (raw button/pad/encoder
events from a fresh boot) plus:

    "bug":      short id / title
    "crash":    the crash/hang summary substring the bug produces (e.g. "E369")
    "open":     true while the bug is unfixed - the test is then an expected
                failure (xfail), and reports XPASS once a fix makes it pass
    "attempts": replays per run (the emulator isn't cycle-exact; default 3)

The test fails if any attempt ends in a crash or hang.
"""

import json
from pathlib import Path

import pytest

REPROS = sorted((Path(__file__).parent / "repros").glob("*.json"))


def _params():
    for p in REPROS:
        rec = json.loads(p.read_text())
        marks = (
            [pytest.mark.xfail(reason=f"open bug: {rec.get('bug')}", strict=False)]
            if rec.get("open")
            else []
        )
        yield pytest.param(p, id=p.stem, marks=marks)


@pytest.mark.parametrize("path", list(_params()))
def test_repro(path):
    from conftest import _require_built
    from deluge_emu import fuzz

    _require_built()
    rec = json.loads(path.read_text())
    for attempt in range(rec.get("attempts", 3)):
        out = fuzz.run(
            f"repro/{path.stem}-{attempt}",
            sd=rec.get("sd", "basic"),
            seed=0,
            actions=0,
            probe_every=1,
            replay=rec["gestures"],
            setup=rec.get("setup"),
        )
        assert out["status"] == "ok", (
            f"{rec.get('bug')}: {out.get('detail')} (artifacts: {out['artifacts']})"
        )
