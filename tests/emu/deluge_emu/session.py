"""`EmuSession`: one emulator run with crash detection and on-disk artifacts.

    from deluge_emu import EmuSession
    with EmuSession.start(sd="basic", name="my-check") as emu:
        emu.dz.press("PLAY")            # full delugemu DelugeSession API on .dz
        emu.check()                      # raises EmuCrash on abort / freezeWithError
        print(emu.log_tail())            # firmware D_PRINTLN output
    # artifacts in build-emu/artifacts/my-check/

Artifacts written to the session directory:

    serial.log    firmware debug output (D_PRINTLN) + emulator messages
    gdb.log       crash watcher output; crash.txt when something crashed
    oled.txt      final OLED contents as '#'/'.' rows
    screen.png    final front-panel screenshot
    state.json    final LEDs, 7-seg, pad colours
    result.json   status, crash summary, firmware/emulator revisions, timing
    sd.img        the (sparse) card image after the run, if a card was inserted
"""

from __future__ import annotations

import json
import re
import shutil
import subprocess
import time
from pathlib import Path

from . import paths, sdcard
from .watcher import CrashWatcher, free_port, summarize

paths.ensure_pylib_on_path()
from delugemu import DelugeSession

ANSI = re.compile(r"\x1b\[[0-9;]*m")
# The log shares the DIN-MIDI UART, so drop raw MIDI/control bytes.
NONPRINT = re.compile(r"[^\x20-\x7e\n\t]")


class EmuCrash(AssertionError):
    """The firmware hit handle_cpu_fault or freezeWithError."""

    def __init__(self, summary: str, report: str, artifacts: Path):
        super().__init__(f"{summary}\n(see {artifacts / 'crash.txt'})\n{report[:3000]}")
        self.summary = summary
        self.report = report


def _git_rev(path: Path) -> str:
    try:
        return subprocess.run(
            ["git", "-C", str(path), "rev-parse", "--short=12", "HEAD"],
            capture_output=True,
            text=True,
            check=True,
        ).stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return "unknown"


class EmuSession:
    def __init__(self, dz, artifacts: Path, watcher: CrashWatcher | None, meta: dict):
        self.dz = dz
        self.artifacts = artifacts
        self.watcher = watcher
        self.meta = meta
        self.sd_image: Path | None = meta.get("sd_image") and Path(meta["sd_image"])
        self._t0 = time.monotonic()
        self._closed = False

    # ------------------------------------------------------------------
    @classmethod
    def start(
        cls,
        name: str = "session",
        elf: str | Path | None = None,
        sd: str | Path | None = "empty",
        artifacts: str | Path | None = None,
        icount: str | int | None = None,
        watch_crashes: bool = True,
        boot_wait: float = 6.0,
        extra_qemu: tuple[str, ...] = (),
        qmp_sock: str | Path | None = None,
    ) -> EmuSession:
        """Boot the firmware in delugemu.

        name       artifacts subdirectory (build-emu/artifacts/<name>/), unless `artifacts` given
        elf        firmware ELF (default build-emu/Debug/deluge.elf)
        sd         card spec, see deluge_emu.sdcard (default: "empty" card)
        icount     QEMU -icount shift (int or "auto"); None = real-time clock
        boot_wait  seconds to let the firmware boot before returning
        """
        if not paths.qemu_binary().exists():
            raise RuntimeError("emulator not built - run `./dbt emu setup`")
        elf = Path(elf) if elf else paths.default_elf()
        if not elf.exists():
            raise FileNotFoundError(f"{elf} not found - run `./dbt emu build`")

        out = Path(artifacts) if artifacts else paths.ARTIFACTS_DIR / name
        if out.exists():
            shutil.rmtree(out)
        out.mkdir(parents=True)

        sd_image = None
        pristine = sdcard.build_image(sd)
        if pristine:
            sd_image = sdcard.working_copy(pristine, out / "sd.img")

        # Keep the QMP socket path short (AF_UNIX limit ~104 chars).
        sock = str(qmp_sock) if qmp_sock else f"/tmp/deluge-emu-{free_port()}.sock"
        port = free_port() if watch_crashes else None
        qemu = list(extra_qemu)
        if watch_crashes:
            qemu += ["-gdb", f"tcp:127.0.0.1:{port}", "-S"]

        meta = {
            "name": name,
            "elf": str(elf),
            "firmware_rev": _git_rev(paths.FW_ROOT),
            "delugemu_rev": _git_rev(paths.delugemu_root()),
            "sd": str(sd) if sd is not None else None,
            "sd_image": str(sd_image) if sd_image else None,
            "icount": icount,
            "qmp_sock": sock,
            "gdb_port": port,
        }
        (out / "session.json").write_text(json.dumps(meta, indent=2) + "\n")

        dz = DelugeSession.launch(
            firmware=str(elf),
            sd=str(sd_image) if sd_image else None,
            icount=icount,
            sock=sock,
            run_log=str(out / "serial.log"),
            extra_qemu=tuple(qemu),
            repo_root=str(paths.delugemu_root()),
            artifacts_dir=str(out),
        )
        watcher = None
        try:
            if watch_crashes:
                watcher = CrashWatcher(elf, port, out)
                watcher.start()
                watcher.wait_attached()
        except BaseException:
            if watcher:
                watcher.stop()
            dz.close()
            raise
        sess = cls(dz, out, watcher, meta)
        try:
            if boot_wait:
                sess.wait(boot_wait)
        except BaseException as exc:
            sess.__exit__(
                type(exc), exc, None
            )  # writes result.json for the failed boot
            raise
        return sess

    # ------------------------------------------------------------------
    def wait(self, seconds: float) -> None:
        """Sleep, but stop early (and raise) if the firmware crashes."""
        deadline = time.monotonic() + seconds
        while True:
            self.check()
            left = deadline - time.monotonic()
            if left <= 0:
                return
            time.sleep(min(0.25, left))

    def crash(self) -> str | None:
        """Crash report text, or None if the firmware hasn't crashed."""
        if not self.watcher:
            return None
        if self.watcher.proc and self.watcher.proc.poll() is None:
            log = self.artifacts / "gdb.log"
            if log.exists() and "DELUGE_EMU_CRASH" in log.read_text(errors="replace"):
                return self.watcher.wait_for_report()
            return None
        return self.watcher.crashed()

    def check(self) -> None:
        """Raise EmuCrash if the firmware crashed, RuntimeError if QEMU died."""
        report = self.crash()
        if report:
            raise EmuCrash(summarize(report), report, self.artifacts)
        if self.dz.proc is not None and self.dz.proc.poll() is not None:
            raise RuntimeError(
                f"emulator exited unexpectedly (code {self.dz.proc.returncode});"
                f" see {self.artifacts / 'serial.log'}\n{self.log_tail(30)}"
            )

    # ------------------------------------------------------------------
    def log(self) -> str:
        """Everything the firmware printed so far (ANSI colour codes stripped)."""
        p = self.artifacts / "serial.log"
        if not p.exists():
            return ""
        return NONPRINT.sub("", ANSI.sub("", p.read_text(encoding="latin-1")))

    def log_tail(self, n: int = 40) -> str:
        return "\n".join(self.log().splitlines()[-n:])

    def wait_for_log(self, pattern: str, timeout: float = 30.0) -> re.Match:
        """Wait until a regex appears in the firmware log; returns the match."""
        rx = re.compile(pattern)
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            m = rx.search(self.log())
            if m:
                return m
            self.wait(0.2)
        raise TimeoutError(
            f"{pattern!r} not in firmware log after {timeout}s:\n{self.log_tail()}"
        )

    # ------------------------------------------------------------------
    def snapshot(self, prefix: str = "") -> dict:
        """Write OLED text, screenshot and LED/pad state to the artifacts dir."""
        dz = self.dz
        state = {}
        try:
            (self.artifacts / f"{prefix}oled.txt").write_text(dz.oled_text() + "\n")
        except Exception as e:  # noqa: BLE001 - best effort
            state["oled_error"] = str(e)
        try:
            dz.screenshot(str(self.artifacts / f"{prefix}screen.png"))
        except Exception as e:  # noqa: BLE001
            state["screenshot_error"] = str(e)
        try:
            state["leds_on"] = sorted(k for k, v in dz.read_leds().items() if v)
            state["seven_segment"] = dz.read_segment()
            state["pads"] = dz.read_pads()
        except Exception as e:  # noqa: BLE001
            state["state_error"] = str(e)
        (self.artifacts / f"{prefix}state.json").write_text(json.dumps(state) + "\n")
        return state

    def close(self, status: str | None = None, error: str | None = None) -> dict:
        """Snapshot, shut down, write result.json. Returns the result dict."""
        if self._closed:
            return {}
        self._closed = True
        report = self.crash()
        alive = self.dz.proc is None or self.dz.proc.poll() is None
        if alive:
            if report:
                time.sleep(1.5)  # let the firmware draw its error/fault screen
            self.snapshot()
        result = dict(self.meta)
        result.update(
            status=status or ("crash" if report else "ok"),
            crash=summarize(report) if report else None,
            error=error,
            wall_seconds=round(time.monotonic() - self._t0, 2),
            artifacts=str(self.artifacts),
        )
        self.dz.close()
        if self.watcher:
            self.watcher.stop()
        (self.artifacts / "result.json").write_text(json.dumps(result, indent=2) + "\n")
        return result

    def __enter__(self):
        return self

    def __exit__(self, exc_type, exc, tb):
        if exc_type is None:
            self.close()
        else:
            if isinstance(exc, EmuCrash):
                status = "crash"
            elif isinstance(exc, AssertionError):
                status = "failed"
            else:
                status = "error"
            self.close(status=status, error=f"{exc_type.__name__}: {exc}"[:2000])
        return False
