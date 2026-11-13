"""Crash detection via QEMU's gdbstub.

A batch-mode arm-none-eabi-gdb stays attached for the whole session with
breakpoints on the firmware's two "this went wrong" funnels:

* ``handle_cpu_fault``  - every CPU exception (data/prefetch abort, undefined
                          instruction, stray SVC) ends up here (fault_handler.c)
* ``freezeWithError``   - FREEZE_WITH_ERROR("XXXX") and display->freezeWithError()

When one is hit, gdb records the error code / faulting address, a backtrace and
the registers into ``crash.txt``, then detaches (the firmware carries on into
its normal freeze/fault screen). The session notices gdb has exited and
reports the crash.

QEMU is started paused (``-S``) and only released by gdb's ``continue``, so
nothing early in boot can slip past the breakpoints.
"""

from __future__ import annotations

import re
import socket
import subprocess
import time
from pathlib import Path

from . import paths

MARKER = "DELUGE_EMU_CRASH"

GDB_SCRIPT = r"""
set pagination off
set confirm off
set print pretty off
set width 0
set remotetimeout 30
target remote 127.0.0.1:{port}

break handle_cpu_fault
commands
  silent
  printf "{marker} kind=cpu_fault\n"
  printf "exception_type=%d (0=undefined 1=prefetch_abort 2=data_abort 3=reserved 15=svc)\n", exception_type
  printf "faulting_pc=0x%08x\n", exception_lr
  info symbol exception_lr
  info line *exception_lr
  printf "caller_lr=0x%08x\n", sys_lr
  info symbol sys_lr
  info line *sys_lr
  printf "sys_sp=0x%08x\n", sys_sp
end

break freezeWithError
commands
  silent
  printf "{marker} kind=freeze\n"
  info args
end

continue

# Only reached once a breakpoint above has fired. For cpu_fault the backtrace
# is of the abort handler; faulting_pc/caller_lr above locate the real culprit.
printf "\n--- backtrace ---\n"
bt 40
printf "\n--- registers ---\n"
info registers r0 r1 r2 r3 r4 r5 r6 r7 r8 r9 r10 r11 r12 sp lr pc cpsr
printf "\n--- end ---\n"
detach
"""


def free_port() -> int:
    with socket.socket() as s:
        s.bind(("127.0.0.1", 0))
        return s.getsockname()[1]


class CrashWatcher:
    def __init__(self, elf: Path, port: int, out_dir: Path):
        self.elf = Path(elf)
        self.port = port
        self.out_dir = Path(out_dir)
        self.crash_file = self.out_dir / "crash.txt"
        self.proc: subprocess.Popen | None = None
        self._log = None

    def start(self) -> None:
        script = self.out_dir / "watch.gdb"
        script.write_text(GDB_SCRIPT.format(port=self.port, marker=MARKER))
        self._log = open(self.out_dir / "gdb.log", "w")  # noqa: SIM115 - closed in stop()
        self.proc = subprocess.Popen(
            [str(paths.GDB), "-batch", "-nx", "-q", "-x", str(script), str(self.elf)],
            stdout=self._log,
            stderr=subprocess.STDOUT,
            stdin=subprocess.DEVNULL,
            start_new_session=True,
        )

    def wait_attached(self, timeout: float = 60.0) -> None:
        """Block until gdb has set its breakpoints and resumed the CPU."""
        log = self.out_dir / "gdb.log"
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            text = log.read_text(errors="replace") if log.exists() else ""
            if "Breakpoint 2 at" in text:
                time.sleep(0.2)  # `continue` follows immediately
                return
            if self.proc.poll() is not None:
                raise RuntimeError(f"gdb exited during attach:\n{text[-2000:]}")
            time.sleep(0.05)
        raise RuntimeError("timed out waiting for gdb to attach to the emulator")

    def crashed(self) -> str | None:
        """Crash report text if a breakpoint fired (and gdb finished), else None."""
        if self.proc is None or self.proc.poll() is None:
            return None
        text = (self.out_dir / "gdb.log").read_text(errors="replace")
        if MARKER not in text:
            return None
        report = text[text.index(MARKER) :]
        if not self.crash_file.exists():
            self.crash_file.write_text(report)
        return report

    def wait_for_report(self, timeout: float = 10.0) -> str | None:
        """After a breakpoint fired, give gdb time to dump everything and detach."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            r = self.crashed()
            if r:
                return r
            log = self.out_dir / "gdb.log"
            if MARKER not in (log.read_text(errors="replace") if log.exists() else ""):
                return None
            time.sleep(0.1)
        return self.crashed()

    def stop(self) -> None:
        if self.proc and self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()
        if self._log:
            self._log.close()
            self._log = None


def summarize(report: str) -> str:
    """One line describing a crash report, for test failures and result.json."""
    kind = re.search(r"kind=(\w+)", report)
    kind = kind.group(1) if kind else "unknown"
    if kind == "freeze":
        m = re.search(r'(?:error|text) = 0x[0-9a-f]+ "([^"]*)"', report)
        return f"freezeWithError({m.group(1) if m else '?'})"
    pc = re.search(r"faulting_pc=(0x[0-9a-f]+)", report)
    sym = re.search(r"faulting_pc=0x[0-9a-f]+\n(.+)", report)
    et = re.search(r"exception_type=(\d+)", report)
    names = {
        "0": "undefined instruction",
        "1": "prefetch abort",
        "2": "data abort",
        "15": "svc",
    }
    what = names.get(et.group(1), f"exception {et.group(1)}") if et else "cpu fault"
    return f"{what} at {pc.group(1) if pc else '?'} ({sym.group(1).strip() if sym else '?'})"
