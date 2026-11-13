"""pytest fixtures for emulator tests.

Every test using the `emu` fixture gets a freshly booted firmware with its own
SD card copy and artifact directory (build-emu/artifacts/<test name>/), and
fails if the firmware crashes (CPU fault or freezeWithError) at any point,
including after the test body finished.

Choose the card with a marker:  @pytest.mark.sd("basic")   (default: "empty";
"none" = no card). Tests are skipped, not failed, when the emulator or the
firmware isn't built - run `./dbt emu setup && ./dbt emu build`.
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

import pytest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from deluge_emu import paths


def pytest_configure(config):
    config.addinivalue_line(
        "markers",
        "sd(spec): SD card for the emu fixture (fixture name, 'none', dir, .img)",
    )
    config.addinivalue_line(
        "markers", "boot_wait(seconds): override the boot wait for the emu fixture"
    )


@pytest.hookimpl(hookwrapper=True)
def pytest_runtest_makereport(item, call):
    outcome = yield
    rep = outcome.get_result()
    setattr(item, f"rep_{rep.when}", rep)


def _require_built():
    if not paths.qemu_binary().exists():
        pytest.skip("emulator not built: run `./dbt emu setup`")
    if not paths.default_elf().exists():
        pytest.skip(f"{paths.default_elf()} missing: run `./dbt emu build`")


@pytest.fixture
def emu(request):
    _require_built()
    from deluge_emu.session import EmuSession

    sd_marker = request.node.get_closest_marker("sd")
    bw_marker = request.node.get_closest_marker("boot_wait")
    name = re.sub(r"[^A-Za-z0-9_.-]+", "_", request.node.name)
    session = EmuSession.start(
        name=f"pytest/{name}",
        sd=sd_marker.args[0] if sd_marker else "empty",
        boot_wait=bw_marker.args[0] if bw_marker else 6.0,
    )
    yield session

    rep = getattr(request.node, "rep_call", None)
    failed = rep is not None and rep.failed
    problem = None
    try:
        session.check()  # a crash after the last assertion still fails the test
    except Exception as e:  # noqa: BLE001 - crash, or the emulator died
        problem = e
    finally:
        if problem is not None and not isinstance(problem, AssertionError):
            status = "error"  # emulator died
        else:
            status = "failed" if failed and not problem else None
        session.close(status=status)
    if problem and not failed:
        pytest.fail(str(problem), pytrace=False)
