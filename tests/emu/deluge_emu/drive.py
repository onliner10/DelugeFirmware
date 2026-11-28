"""`./dbt emu check` - drive the firmware in delugemu from one short script, on the guest's clock.

    emu check [--elf A.elf,B.elf] [--sd SPEC] [--clock fast|rt|timectl] [-j N] SCRIPT... | -e 'STEPS'

One process per run does start -> steps -> checks -> stop and prints one line per
check plus a RESULT line, so a whole UI test is a single command. Several scripts
and/or ELFs run as a matrix in parallel, each emulator under a unique name.

Clocks (all waits below are GUEST seconds, read from the virtual clock):
  fast     -icount shift=2,sleep=off (default): 250 MIPS guest, close to the real
           Deluge; runs 1.2-1.6x faster than real time and is unaffected by host
           load, so parallel runs and builds can't drop presses. For UI/logic checks.
  timectl  dzcache cycle clock (400 MHz, cpi 1.45): for load / voice / audio checks.
  rt       host real time (the old behaviour; flaky under load).

Script: one step per line or ';'-separated, '#' comments. CONTROL is a button name
(SHIFT, PLAY, LEARN, SELECT, X, Y, TEMPO, MOD0..7, MOD_ENCODER_0/1, ...) or a pad 'x,y'
(x 0..17, y 0..7 bottom-up; 16/17 = mute/audition columns).

  press C [C...]        tap; several = chord (down in order, up in reverse)
  hold C SECS           press and hold for SECS
  down C / up C         keep held / release
  turn ENC N            turn SELECT|X|Y|TEMPO|MOD_ENCODER_0|MOD_ENCODER_1 by N detents
  wait SECS             guest seconds
  waitlog REGEX [SECS]  until the log matches (since the last mark), default 10 s
  idle [QUIET]          until the SD card has been quiet QUIET s (default 0.5)
  load-song PATH        browse + load, then idle
  type TEXT             type on the pad keyboard
  mark                  checks/prints of the log only look after this point
  expect log REGEX      log since mark matches        expect nolog REGEX
  expect last RX PAT    last log line matching RX matches PAT (retried for 2 s)
  expect oled TEXT      OLED text contains TEXT (case-insensitive, spaces squeezed)
  expect led NAME on|off
  oled | oledraw | leds | seg | pads | log [N] | last RX | shot FILE      print state
                        (oled = text read with the firmware fonts; oledraw = the pixels)
  include FILE          splice another script in
Every step checks for a firmware crash (gdb watcher); a crash ends the run.
"""

from __future__ import annotations

import argparse
import contextlib
import os
import re
import shlex
import subprocess
import sys
import tempfile
import time
import types
from pathlib import Path

from . import paths

paths.ensure_pylib_on_path()
from delugemu import session as dz_session_mod
from delugemu._controls import CONTROLS

BUTTONS = {n: (c["col"], c["row"]) for n, c in CONTROLS.items()}
ALIASES = {
    "SELECT": "SELECT_ENC",
    "X": "X_ENC",
    "Y": "Y_ENC",
    "TEMPO": "TEMPO_ENC",
    "SONG": "SESSION_VIEW",
    "CLIP": "CLIP_VIEW",
    "SCALE": "SCALE_MODE",
    "TAP": "TAP_TEMPO",
    "CROSS": "CROSS_SCREEN",
    "AFFECT": "AFFECT_ENTIRE",
    "REC": "RECORD",
    "XENC": "X_ENC",
    "YENC": "Y_ENC",
}
ENCODERS = {
    "Y_ENC": 0,
    "X_ENC": 1,
    "TEMPO_ENC": 2,
    "SELECT_ENC": 3,
    "MOD_ENCODER_1": 4,
    "MOD_ENCODER_0": 5,
}
CLOCKS = {
    "fast": ("-icount", "shift=2,sleep=off"),
    "rt": (),
    "timectl": (
        "-plugin",
        "{dzcache},timectl=1,cachesim=0,cpi=1.45,lean=1,hz=400000000",
    ),
}
ANSI = re.compile(r"\x1b\[[0-9;]*m")
NONPRINT = re.compile(r"[^\x20-\x7e\n\t]")
TAP = 0.1  # guest seconds a tap is held
GAP = 0.15  # guest seconds after each input event


class Fail(AssertionError):
    pass


def canon(name: str) -> str:
    n = name.upper()
    return ALIASES.get(n, n)


def clock_args(clock: str) -> tuple[str, ...]:
    dzcache = paths.delugemu_root() / "build" / "plugins" / "libdzcache.so"
    return tuple(a.format(dzcache=dzcache) for a in CLOCKS[clock])


def read_script(text: str, base: Path, depth=0) -> list[list[str]]:
    steps = []
    for raw in text.replace(";", "\n").splitlines():
        line = raw.split(" #")[0].strip() if not raw.strip().startswith("#") else ""
        if not line:
            continue
        toks = shlex.split(line)
        if toks[0] == "include":
            if depth > 8:
                raise ValueError("include nesting too deep")
            f = (base / toks[1]) if not Path(toks[1]).is_absolute() else Path(toks[1])
            steps += read_script(f.read_text(), f.parent, depth + 1)
        else:
            steps.append(toks)
    return steps


class Runner:
    def __init__(self, emu, out):
        self.emu, self.dz, self.out = emu, emu.dz, out
        self.mark = 0
        self.fails = 0
        self.held: list[str] = []
        # Make every wait and timeout inside delugemu (wait_sd_idle, load_song, browser_open...) use guest time.
        gclock = lambda: self.dz.vclock_ns() / 1e9
        dz_session_mod.time = types.SimpleNamespace(
            monotonic=gclock, time=gclock, sleep=self._host_nap
        )
        self.dz.wait = self.wait

    # -- clock -----------------------------------------------------------
    @staticmethod
    def _host_nap(s):
        time.sleep(min(s, 0.002))

    def now(self) -> float:
        return self.dz.vclock_ns() / 1e9

    def wait(self, secs: float) -> None:
        end = self.now() + float(secs)
        n = 0
        while self.now() < end:
            time.sleep(0.002)
            n += 1
            if n % 50 == 0:
                self.emu.check()
        self.emu.check()

    # -- log -------------------------------------------------------------
    def log_lines(self) -> list[str]:
        p = self.emu.artifacts / "serial.log"
        text = p.read_text(encoding="latin-1") if p.exists() else ""
        return NONPRINT.sub("", ANSI.sub("", text)).splitlines()

    def since_mark(self) -> list[str]:
        return self.log_lines()[self.mark :]

    def last(self, rx: str) -> str | None:
        r = re.compile(rx)
        for line in reversed(self.since_mark()):
            if r.search(line):
                return line.strip()
        return None

    def wait_log(self, rx: str, timeout: float) -> bool:
        r = re.compile(rx)
        end = self.now() + timeout
        while True:
            if any(r.search(x) for x in self.since_mark()):
                return True
            if self.now() >= end:
                return False
            self.wait(0.05)

    # -- input -----------------------------------------------------------
    def _set(self, ctl: str, down: bool) -> None:
        if "," in ctl:
            x, y = (int(v) for v in ctl.split(","))
            self.dz.pad_event(x, y, down)
        else:
            name = canon(ctl)
            if name not in BUTTONS:
                raise ValueError(f"unknown control {ctl!r}")
            self.dz.button(*BUTTONS[name], down)
        if down:
            self.held.append(ctl)
        elif ctl in self.held:
            self.held.remove(ctl)

    def press(self, ctls: list[str], hold: float = TAP) -> None:
        for c in ctls:
            self._set(c, True)
            self.wait(0.05 if len(ctls) > 1 else 0)
        self.wait(hold)
        for c in reversed(ctls):
            self._set(c, False)
            self.wait(0.03 if len(ctls) > 1 else 0)
        self.wait(GAP)

    # -- output ----------------------------------------------------------
    def say(self, s: str) -> None:
        print(f"{self.out}{s}", flush=True)

    def check(self, ok: bool, what: str, got: str = "") -> None:
        if ok:
            self.say(f"PASS {what}")
        else:
            self.fails += 1
            self.say(f"FAIL {what}" + (f"\n{self.out}     got: {got}" if got else ""))

    def pads(self) -> str:
        grid = self.dz.read_pads()
        rows = []
        for y in reversed(range(8)):
            s = ""
            for x in range(len(grid)):
                r, g, b = grid[x][y]
                m = max(r, g, b)
                s += (
                    "."
                    if m == 0
                    else (
                        "-" if r == g == b and m <= 0x20 else ("o" if m < 0x40 else "#")
                    )
                )
                if x == 15:
                    s += " "
            rows.append(f"   y{y} {s}")
        return "\n".join(rows)

    def oled(self) -> str:
        """OLED text, lines joined by ' / ' (inverted/highlighted text in [brackets])."""
        from .oledocr import read_text

        return " / ".join(read_text(self.dz.read_oled()).splitlines())

    # -- steps -----------------------------------------------------------
    def step(self, t: list[str]) -> None:
        v, a = t[0], t[1:]
        dz = self.dz
        if v == "press":
            self.press(a)
        elif v == "hold":
            self.press(a[:-1], float(a[-1]))
        elif v == "down":
            for c in a:
                self._set(c, True)
                self.wait(0.05)
            self.wait(GAP)
        elif v == "up":
            for c in a:
                self._set(c, False)
                self.wait(0.03)
            self.wait(GAP)
        elif v == "turn":
            enc = canon(a[0])
            enc = enc if enc in ENCODERS else enc + "_ENC"
            n = int(a[1])
            for _ in range(abs(n)):
                dz.encoder(ENCODERS[enc], 1 if n > 0 else -1)
                self.wait(0.04)
            self.wait(GAP)
        elif v == "wait":
            self.wait(float(a[0]))
        elif v == "waitlog":
            if not self.wait_log(a[0], float(a[1]) if len(a) > 1 else 10.0):
                self.check(False, f"waitlog /{a[0]}/", "timed out")
        elif v == "idle":
            if not dz.wait_sd_idle(quiet=float(a[0]) if a else 0.5, timeout=60):
                self.check(False, "idle", "SD never went quiet")
        elif v == "load-song":
            dz.load_song(" ".join(a))
        elif v == "type":
            dz.type_text(" ".join(a))
        elif v == "mark":
            self.mark = len(self.log_lines())
        elif v == "expect":
            self.expect(a)
        elif v == "oled":
            self.say(f"oled: {self.oled()}")
        elif v == "oledraw":
            self.say("oledraw:\n" + self.dz.oled_text())
        elif v == "leds":
            self.say(
                "leds: " + " ".join(sorted(k for k, on in dz.read_leds().items() if on))
            )
        elif v == "seg":
            self.say(f"seg: {dz.read_segment()}")
        elif v == "pads":
            self.say("pads:\n" + self.pads())
        elif v == "log":
            for line in self.since_mark()[-int(a[0] if a else 20) :]:
                self.say(f"| {line}")
        elif v == "last":
            self.say(f"last: {self.last(a[0])}")
        elif v == "shot":
            dz.screenshot(
                a[0] if a else str(self.emu.artifacts / f"shot-{self.now():.1f}.png")
            )
        else:
            raise ValueError(f"unknown step {v!r}")
        self.emu.check()

    def expect(self, a: list[str]) -> None:
        kind = a[0]
        if kind == "log":
            ok = any(re.search(a[1], x) for x in self.since_mark())
            self.check(
                ok,
                f"log /{a[1]}/",
                "" if ok else (self.since_mark() or ["(empty)"])[-1],
            )
        elif kind == "nolog":
            m = [x for x in self.since_mark() if re.search(a[1], x)]
            self.check(not m, f"no log /{a[1]}/", m[0] if m else "")
        elif kind == "last":
            rx, pat = a[1], a[2]
            end = self.now() + 2.0
            while True:
                got = self.last(rx) or ""
                if re.search(pat, got) or self.now() >= end:
                    break
                self.wait(0.1)
            self.check(bool(re.search(pat, got)), f"last /{rx}/ ~ /{pat}/", got)
        elif kind == "oled":
            want = re.sub(r"\s+", " ", " ".join(a[1:])).lower()
            got = self.oled()
            self.check(want in got.lower(), f"oled has {want!r}", got)
        elif kind == "led":
            name = canon(a[1])
            on = a[2].lower() in ("on", "1", "true")
            got = self.dz.read_leds().get(name)
            self.check(got == on, f"led {name} {a[2]}", f"{'on' if got else 'off'}")
        else:
            raise ValueError(f"unknown expect {kind!r}")


def boot(r: Runner, timeout: float = 30.0) -> None:
    if not r.wait_log(r"going into main loop", timeout):
        raise Fail("firmware never reached the main loop")
    r.dz.wait_sd_idle(quiet=0.5, timeout=timeout)


def run_one(a) -> int:
    """One emulator run (child process of `run`)."""
    from .session import EmuCrash, EmuSession

    steps = read_script(a.script_text, Path(a.script_base))
    out = f"[{a.tag}] " if a.tag else ""
    t0 = time.monotonic()
    emu = None
    for attempt in range(
        3
    ):  # the gdb port is picked by bind-and-release: retry a lost race
        try:
            emu = EmuSession.start(
                name=a.name,
                elf=a.elf,
                sd=a.sd,
                boot_wait=0,
                extra_qemu=clock_args(a.clock) + tuple(a.qemu_arg or ()),
            )
            break
        except Exception as e:  # noqa: BLE001
            if attempt == 2:
                print(f"{out}RESULT error: start failed: {e}"[:600], flush=True)
                return 3
            time.sleep(0.5)
    r = Runner(emu, out)
    status, err, code = "ok", None, 0
    i = -1
    try:
        if not a.no_boot:
            boot(r)
        r.mark = len(r.log_lines())
        for i, t in enumerate(steps):
            r.step(t)
        if r.fails:
            status, code = f"FAIL {r.fails}", 1
    except EmuCrash as e:
        status, err, code = (
            "CRASH",
            f"{e.summary} at step {i + 1} {' '.join(steps[i]) if i >= 0 else 'boot'}",
            2,
        )
    except Exception as e:  # noqa: BLE001
        status, code = "error", 3
        err = f"{type(e).__name__}: {e} at step {i + 1} {' '.join(steps[i]) if 0 <= i < len(steps) else 'boot'}"
    guest = r.now() if emu.dz.proc.poll() is None else float("nan")
    for c in list(r.held):
        with contextlib.suppress(Exception):
            r._set(c, False)
    res = emu.close(
        status="ok" if code in (0, 1) else ("crash" if code == 2 else "error"),
        error=err,
    )
    if code == 2 or (code == 3 and status == "error"):
        tail = (
            "\n".join(f"{out}| {x}" for x in r.log_lines()[-a.tail :]) if a.tail else ""
        )
        if tail:
            print(tail)
    print(
        f"{out}RESULT {status}{' - ' + err if err else ''}  (guest {guest:.1f}s, wall {time.monotonic() - t0:.1f}s,"
        f" {res.get('artifacts', '')})",
        flush=True,
    )
    return code


def main(argv=None) -> int:
    p = argparse.ArgumentParser(
        prog="emu check",
        description=__doc__,
        formatter_class=argparse.RawDescriptionHelpFormatter,
    )
    p.add_argument("scripts", nargs="*", help="script files")
    p.add_argument(
        "-e", dest="inline", action="append", help="inline steps (';'-separated)"
    )
    p.add_argument(
        "--elf",
        default=None,
        help="ELF, or several comma-separated (runs each script on each)",
    )
    p.add_argument(
        "--sd",
        default="empty",
        help="card: empty | basic | factory | fixture name | image path",
    )
    p.add_argument("--clock", default="fast", choices=sorted(CLOCKS))
    p.add_argument(
        "-j", type=int, default=min(8, os.cpu_count() or 4), help="parallel runs"
    )
    p.add_argument(
        "--name",
        default=None,
        help="name prefix (default: script name); a unique suffix is added",
    )
    p.add_argument(
        "--no-boot",
        action="store_true",
        help="don't wait for the main loop + SD idle first",
    )
    p.add_argument(
        "--tail", type=int, default=15, help="log lines to show after a crash/error"
    )
    p.add_argument(
        "--qemu-arg", action="append", help="extra QEMU argument (repeatable)"
    )
    p.add_argument("--child", action="store_true", help=argparse.SUPPRESS)
    p.add_argument("--script-text", help=argparse.SUPPRESS)
    p.add_argument("--script-base", default=".", help=argparse.SUPPRESS)
    p.add_argument("--tag", default="", help=argparse.SUPPRESS)
    a = p.parse_args(argv)
    if a.child:
        return run_one(a)

    jobs = []  # (label, text, base)
    for s in a.scripts:
        f = Path(s)
        jobs.append((f.stem, f.read_text(), str(f.resolve().parent)))
    for k, text in enumerate(a.inline or []):
        jobs.append((f"e{k}" if len(a.inline) > 1 else "inline", text, os.getcwd()))
    if not jobs:
        p.error("give a script file or -e 'steps'")
    elfs = [os.path.expanduser(e) for e in a.elf.split(",")] if a.elf else [None]
    runs = []
    uniq = f"{os.getpid() % 100000}"
    for label, text, base in jobs:
        for elf in elfs:
            etag = Path(elf).stem if elf else "default"
            tag = "/".join(
                x
                for x in (
                    (label if len(jobs) > 1 else ""),
                    (etag if len(elfs) > 1 else ""),
                )
                if x
            )
            name = f"{a.name or label}-{etag}-{uniq}"[:60]
            cmd = (
                [
                    sys.executable,
                    "-m",
                    "deluge_emu.drive",
                    "--child",
                    "--name",
                    name,
                    "--tag",
                    tag,
                    "--script-text",
                    text,
                    "--script-base",
                    base,
                    "--sd",
                    a.sd,
                    "--clock",
                    a.clock,
                    "--tail",
                    str(a.tail),
                ]
                + (["--elf", elf] if elf else [])
                + (["--no-boot"] if a.no_boot else [])
            )
            for q in a.qemu_arg or ():
                cmd += ["--qemu-arg", q]
            runs.append((tag, cmd))
    if len(runs) == 1:
        return subprocess.run(
            runs[0][1], cwd=str(Path(__file__).resolve().parent.parent), check=False
        ).returncode
    # Parallel: buffer each run's output and print it whole when it finishes.
    pending, active, worst, t0 = list(runs), [], 0, time.monotonic()
    while pending or active:
        while pending and len(active) < a.j:
            tag, cmd = pending.pop(0)
            fh = tempfile.TemporaryFile(mode="w+")  # noqa: SIM115 - closed when the run finishes
            active.append(
                (
                    tag,
                    subprocess.Popen(
                        cmd,
                        stdout=fh,
                        stderr=subprocess.STDOUT,
                        cwd=str(Path(__file__).resolve().parent.parent),
                    ),
                    fh,
                )
            )
        for item in list(active):
            tag, pr, fh = item
            if pr.poll() is not None:
                fh.seek(0)
                sys.stdout.write(fh.read())
                sys.stdout.flush()
                worst = max(worst, pr.returncode)
                active.remove(item)
        time.sleep(0.05)
    print(
        f"ALL {len(runs)} runs done in {time.monotonic() - t0:.1f}s, worst exit {worst}"
    )
    return worst


if __name__ == "__main__":
    sys.exit(main())
