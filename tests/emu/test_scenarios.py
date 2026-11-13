"""Run every JSON scenario in tests/emu/scenarios/ as a test.

A scenario may set "sd" (card spec) and "boot_wait"; see deluge_emu/scenario.py
for the verbs. Dropping a repro file in that folder is all it takes to turn a
bug report into a regression test.
"""

import json
from pathlib import Path

import pytest
from deluge_emu.scenario import run_steps

SCENARIOS = sorted((Path(__file__).parent / "scenarios").glob("*.json"))


@pytest.mark.parametrize("path", SCENARIOS, ids=[p.stem for p in SCENARIOS])
def test_scenario(path, request):
    from conftest import _require_built
    from deluge_emu.session import EmuSession

    _require_built()
    sc = json.loads(path.read_text())
    with EmuSession.start(
        name=f"scenario/{path.stem}",
        sd=sc.get("sd", "empty"),
        boot_wait=sc.get("boot_wait", 6.0),
    ) as emu:
        run_steps(emu, sc)
