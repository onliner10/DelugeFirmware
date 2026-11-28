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

GDB_HEADER = r"""
set pagination off
set confirm off
set print pretty off
set width 0
set remotetimeout 30
target remote 127.0.0.1:{port}
"""

# Breakpoints sit on the exact entry addresses (from the symbol table) and the
# loop matches $pc against them: gdb doesn't run breakpoint `commands` for a
# `continue` inside a `while`, and plain gdb has no Python for anything fancier.
# Any other stop (e.g. a QMP `stop`) resumes and keeps watching, so crash
# detection can't be switched off by accident (a QMP pause is undone: use
# --no-watch for paused/stepped runs). Arguments are read from r0-r3 (AAPCS):
#   handle_cpu_fault(exception_lr, type, sys_lr, sys_sp)
#   freezeWithError(const char* code)  /  Display::freezeWithError(this, code)
GDB_LOOP = r"""
set $deluge_crashed = 0
while $deluge_crashed == 0
  continue
  if $pc == {fault}
    set $deluge_crashed = 1
    set $f_lr = $r0
    set $f_type = $r1
    set $f_syslr = $r2
    set $f_sp = $r3
    printf "{marker} kind=cpu_fault\n"
    printf "exception_type=%d (0=undefined 1=prefetch_abort 2=data_abort 3=reserved 15=svc)\n", $f_type
    printf "faulting_pc=0x%08x\n", $f_lr
    info symbol $f_lr
    info line *$f_lr
    printf "caller_lr=0x%08x\n", $f_syslr
    info symbol $f_syslr
    info line *$f_syslr
    printf "sys_sp=0x%08x\n", $f_sp
  end
  if {freeze_r0_cond}
    set $deluge_crashed = 1
    printf "{marker} kind=freeze\n"
    printf "error=\"%s\"\n", (char*)$r0
  end
  if {freeze_r1_cond}
    set $deluge_crashed = 1
    printf "{marker} kind=freeze\n"
    printf "error=\"%s\"\n", (char*)$r1
  end
end

# Only reached once a breakpoint above has fired. For cpu_fault the backtrace
# is of the abort handler; faulting_pc/caller_lr above locate the real culprit.
printf "\n--- backtrace ---\n"
bt 40
printf "\n--- registers ---\n"
info registers r0 r1 r2 r3 r4 r5 r6 r7 r8 r9 r10 r11 r12 sp lr pc cpsr
{extra}
printf "\n--- end ---\n"
detach
"""


def crash_entry_points(elf: Path) -> dict[str, list[int]]:
    """Entry addresses of the crash funnels, from the ELF's symbol table."""
    out = subprocess.run(
        [str(paths.NM), "-C", str(elf)], capture_output=True, text=True, check=True
    ).stdout
    found: dict[str, list[int]] = {"fault": [], "freeze_r0": [], "freeze_r1": []}
    for line in out.splitlines():
        parts = line.split(" ", 2)
        if len(parts) != 3 or parts[1] not in "tT":
            continue
        addr, name = int(parts[0], 16) & ~1, parts[2]
        if name == "handle_cpu_fault":
            found["fault"].append(addr)
        elif name == "freezeWithError":
            found["freeze_r0"].append(addr)
        elif name.endswith("::freezeWithError(char const*)"):
            found["freeze_r1"].append(addr)
    if not found["fault"] or not found["freeze_r0"]:
        raise RuntimeError(
            f"{elf}: handle_cpu_fault / freezeWithError not found in symbol table"
        )
    return found


def _extra_commands() -> str:
    """$DELUGE_EMU_GDB_EXTRA: a file of gdb commands to run when a crash is caught
    (e.g. `print currentUIMode`), for inspecting firmware state at the crash."""
    import os

    f = os.environ.get("DELUGE_EMU_GDB_EXTRA")
    if not f:
        return ""
    return '\nprintf "\\n--- extra ---\\n"\n' + Path(f).read_text()


def gdb_script(elf: Path, port: int) -> str:
    eps = crash_entry_points(elf)

    def cond(addrs):
        return " || ".join(f"$pc == {a:#x}" for a in addrs) or "0"

    breaks = "".join(f"break *{a:#x}\n" for group in eps.values() for a in group)
    return (
        GDB_HEADER.format(port=port)
        + breaks
        + GDB_LOOP.format(
            extra=_extra_commands(),
            marker=MARKER,
            fault=f"{eps['fault'][0]:#x}",
            freeze_r0_cond=cond(eps["freeze_r0"]),
            freeze_r1_cond=cond(eps["freeze_r1"]),
        )
    )


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
        text = gdb_script(self.elf, self.port)
        self._n_breaks = text.count("\nbreak *")
        script.write_text(text)
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
            if f"Breakpoint {self._n_breaks} at" in text:
                time.sleep(0.2)  # `continue` follows immediately
                return
            if self.proc.poll() is not None:
                raise RuntimeError(f"gdb exited during attach:\n{text[-2000:]}")
            time.sleep(0.05)
        raise RuntimeError("timed out waiting for gdb to attach to the emulator")

    def _marker_text(self) -> str | None:
        log = self.out_dir / "gdb.log"
        text = log.read_text(errors="replace") if log.exists() else ""
        return text[text.index(MARKER) :] if MARKER in text else None

    def crashed(self, timeout: float = 10.0) -> str | None:
        """Crash report text if a breakpoint fired, else None.

        After the marker appears, waits up to `timeout` for gdb to finish its
        dump; if it doesn't (corrupt stack, slow remote), the partial report is
        still returned - a crash is never downgraded to "no crash".
        """
        report = self._marker_text()
        if report is None:
            return None
        deadline = time.monotonic() + timeout
        while (
            self.proc is not None
            and self.proc.poll() is None
            and time.monotonic() < deadline
        ):
            time.sleep(0.1)
        report = self._marker_text() or report
        self.crash_file.write_text(report)
        return report

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
        m = re.search(r'error="([^"]*)"', report) or re.search(
            r'(?:error|text) = 0x[0-9a-f]+ "([^"]*)"', report
        )
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
