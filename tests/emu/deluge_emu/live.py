"""Long-running emulator sessions driven from separate commands.

    ./dbt emu start --sd basic          # boots in the background, returns when ready
    ./dbt emu ctl press PLAY            # each ctl attaches over QMP, acts, detaches
    ./dbt emu ctl oled
    ./dbt emu ctl log -n 20
    ./dbt emu stop                      # snapshot + result.json, then quit

A small supervisor process (``serve``) owns the emulator and the gdb crash
watcher; it releases the QMP socket so ``ctl`` can connect. Every ctl command
reports (and exits non-zero on) a firmware crash.
"""

from __future__ import annotations

import json
import os
import signal
import socket
import subprocess
import sys
import time
from pathlib import Path

from . import paths
from .session import EmuSession
from .watcher import MARKER, summarize

paths.ensure_pylib_on_path()
from delugemu import DelugeSession


def state_file(name: str) -> Path:
    return paths.LIVE_DIR / f"{name}.json"


def read_state(name: str) -> dict | None:
    try:
        return json.loads(state_file(name).read_text())
    except (OSError, ValueError):
        return None


def _write_state(name: str, **kw) -> None:
    paths.LIVE_DIR.mkdir(parents=True, exist_ok=True)
    st = read_state(name) or {}
    st.update(kw)
    tmp = state_file(name).with_suffix(".tmp")
    tmp.write_text(json.dumps(st, indent=2) + "\n")
    os.replace(tmp, state_file(name))


def _pid_is(pid: int | None, needle: str) -> bool:
    """pid is alive and its command line contains `needle` (guards against PID reuse)."""
    if not _pid_alive(pid):
        return False
    try:
        cmd = subprocess.run(
            ["ps", "-o", "command=", "-p", str(pid)],
            capture_output=True,
            text=True,
            check=False,
        ).stdout
    except OSError:
        return False
    return needle in cmd


def _pid_alive(pid: int | None) -> bool:
    if not pid:
        return False
    try:
        os.kill(pid, 0)
        return True
    except OSError:
        return False


# --------------------------------------------------------------------------
# supervisor
# --------------------------------------------------------------------------


def serve(name: str, start_kwargs: dict) -> int:
    """Run in the background: boot, publish state, supervise until QEMU exits."""
    stop = {"flag": False}
    signal.signal(signal.SIGTERM, lambda *_: stop.__setitem__("flag", True))
    try:
        emu = EmuSession.start(name=name, **start_kwargs)
    except Exception as e:  # noqa: BLE001 - report any boot failure to `start`
        _write_state(name, status="failed", error=f"{type(e).__name__}: {e}"[:4000])
        return 1

    # Release QMP so `ctl` commands can connect (QEMU serves one client at a time).
    emu.dz.q.close()
    _write_state(
        name,
        status="running",
        supervisor_pid=os.getpid(),
        qemu_pid=emu.dz.proc.pid,
        qmp_sock=emu.meta["qmp_sock"],
        artifacts=str(emu.artifacts),
        sd_image=emu.meta["sd_image"],
        crash=None,
        started=time.time(),
    )
    crash_seen = False
    while not stop["flag"] and emu.dz.proc.poll() is None:
        if not crash_seen:
            report = emu.crash()
            if report:
                crash_seen = True
                _write_state(name, crash=summarize(report))
        time.sleep(0.25)

    # Final snapshot + result.json while QEMU is still up.
    # A ctl client may hold QMP (one client at a time): bound the wait, including
    # the greeting read, so stop never hangs on it.
    emu.dz.q = None
    if emu.dz.proc.poll() is None:
        old = socket.getdefaulttimeout()
        socket.setdefaulttimeout(10)
        try:
            emu.dz.q = DelugeSession.attach(emu.meta["qmp_sock"], connect_timeout=10).q
        except Exception as e:  # noqa: BLE001 - snapshot is best effort
            print(f"could not reattach for the final snapshot: {e}", flush=True)
        finally:
            socket.setdefaulttimeout(old)
    result = emu.close()
    _write_state(name, status="stopped", result=result)
    return 0


# --------------------------------------------------------------------------
# client side
# --------------------------------------------------------------------------


def start(name: str, start_kwargs: dict, timeout: float = 180.0) -> dict:
    st = read_state(name)
    if (
        st
        and st.get("status") in ("starting", "running")
        and _pid_alive(st.get("supervisor_pid"))
    ):
        raise RuntimeError(
            f"live session {name!r} already running (./dbt emu stop --name {name})"
        )
    paths.LIVE_DIR.mkdir(parents=True, exist_ok=True)
    state_file(name).unlink(missing_ok=True)
    _write_state(name, status="starting")
    log = open(paths.LIVE_DIR / f"{name}.supervisor.log", "w")  # noqa: SIM115 - owned by child
    proc = subprocess.Popen(
        [sys.executable, "-m", "deluge_emu.live", name, json.dumps(start_kwargs)],
        cwd=str(Path(__file__).resolve().parent.parent),
        stdout=log,
        stderr=subprocess.STDOUT,
        stdin=subprocess.DEVNULL,
        start_new_session=True,
    )
    _write_state(name, supervisor_pid=proc.pid)
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        st = read_state(name) or {}
        if st.get("status") in ("running", "failed", "stopped"):
            return st
        if proc.poll() is not None:
            st = read_state(name) or {}
            st.setdefault(
                "error", (paths.LIVE_DIR / f"{name}.supervisor.log").read_text()[-3000:]
            )
            st["status"] = "failed"
            return st
        time.sleep(0.2)
    proc.terminate()
    raise TimeoutError(f"live session {name!r} did not come up in {timeout}s")


def attach(name: str) -> tuple[DelugeSession, dict]:
    st = read_state(name)
    if (
        not st
        or st.get("status") != "running"
        or not _pid_alive(st.get("supervisor_pid"))
    ):
        raise RuntimeError(
            f"no running live session {name!r} - start one with `./dbt emu start`"
        )
    return DelugeSession.attach(st["qmp_sock"], connect_timeout=10), st


def crash_report(st: dict) -> str | None:
    log = Path(st["artifacts"]) / "gdb.log"
    text = log.read_text(errors="replace") if log.exists() else ""
    return text[text.index(MARKER) :] if MARKER in text else None


def stop(name: str, timeout: float = 30.0) -> dict | None:
    """Stop a live session. Returns its final state (with "result" when the
    supervisor managed to write one), or None if there is no such session."""
    st = read_state(name)
    if not st:
        return None
    pid = st.get("supervisor_pid")
    if _pid_is(pid, "deluge_emu.live"):
        os.kill(pid, signal.SIGTERM)
        deadline = time.monotonic() + timeout
        while _pid_alive(pid) and time.monotonic() < deadline:
            time.sleep(0.2)
    if _pid_is(st.get("qemu_pid"), "qemu-system-arm"):
        os.kill(st["qemu_pid"], signal.SIGKILL)
    st = read_state(name) or st
    if st.get("status") in ("starting", "running"):
        _write_state(name, status="stopped")
        st["status"] = "stopped"
    return st


if __name__ == "__main__":
    sys.exit(serve(sys.argv[1], json.loads(sys.argv[2])))
