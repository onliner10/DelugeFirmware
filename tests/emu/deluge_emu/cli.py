"""`./dbt emu` - run the Deluge firmware in the delugemu emulator.

Subcommands (see `./dbt emu <cmd> -h`):

  setup        fetch + build the pinned emulator, install test deps
  build        build the firmware for the emulator (build-emu/Debug/deluge.elf)
  test         run the emulator test suite (pytest tests/emu)
  run          boot once, optionally run a scenario, write artifacts, exit
  start        boot a live session in the background
  ctl          drive the live session (press/turn/pad/type/load-song/oled/log/...)
  stop         stop the live session (writes result.json)
  status       show live sessions
  sd           build an SD image, list or extract files from one
"""

from __future__ import annotations

import argparse
import json
import subprocess
import sys
import time
from pathlib import Path

from . import build, paths, sdcard

REQUIREMENTS = paths.FW_ROOT / "tests" / "emu" / "requirements.txt"


def _need_emulator() -> None:
    if not paths.qemu_binary().exists():
        sys.exit("emulator not built: run `./dbt emu setup` first")


def _need_elf(elf: str | None, auto_build: bool = True) -> str:
    p = Path(elf) if elf else paths.default_elf()
    if not p.exists():
        if elf or not auto_build:
            sys.exit(f"{p} not found")
        build.build_firmware()
    return str(p)


def _start_kwargs(a) -> dict:
    return {
        "elf": _need_elf(a.elf),
        "sd": a.sd,
        "icount": a.icount,
        "watch_crashes": not a.no_watch,
        "boot_wait": a.boot_wait,
        "extra_qemu": tuple(a.qemu_arg or ()),
    }


def _add_launch_args(p: argparse.ArgumentParser) -> None:
    p.add_argument(
        "--elf",
        help="firmware ELF (default: build-emu/Debug/deluge.elf, built if missing)",
    )
    p.add_argument(
        "--sd",
        default="empty",
        help=f"card: fixture name ({', '.join(sdcard.list_fixtures())}), 'factory', 'none', "
        "a directory or an .img (default: empty)",
    )
    p.add_argument(
        "--icount", help="QEMU -icount shift (e.g. 'auto' or 4); default real-time"
    )
    p.add_argument(
        "--boot-wait",
        type=float,
        default=6.0,
        help="seconds to let it boot (default 6)",
    )
    p.add_argument(
        "--no-watch",
        action="store_true",
        help="no gdb crash watcher (frees the gdbstub)",
    )
    p.add_argument(
        "--qemu-arg", action="append", help="extra raw QEMU argument (repeatable)"
    )


# --------------------------------------------------------------------------


def cmd_setup(a) -> int:
    build.setup_emulator(force=a.force)
    build.log("installing Python test dependencies")
    subprocess.run(
        [sys.executable, "-m", "pip", "install", "-q", "-r", str(REQUIREMENTS)],
        check=True,
    )
    problems = build.check_host_tools()
    for p in problems:
        build.log(f"WARNING: {p}")
    build.log("done. Next: ./dbt emu build && ./dbt emu test")
    return 0


def cmd_build(a) -> int:
    build.build_firmware(a.config)
    return 0


def cmd_test(a) -> int:
    _need_emulator()
    if not a.no_build:
        build.build_firmware()
    cmd = [
        sys.executable,
        "-m",
        "pytest",
        str(paths.FW_ROOT / "tests" / "emu"),
    ] + a.pytest_args
    return subprocess.run(cmd, cwd=paths.FW_ROOT, check=False).returncode


def cmd_run(a) -> int:
    _need_emulator()
    from .session import EmuCrash, EmuSession

    kw = _start_kwargs(a)
    result = {}
    rc = 0
    try:
        with EmuSession.start(name=a.name, **kw) as emu:
            if a.script:
                from .scenario import run_steps

                run_steps(emu, json.loads(Path(a.script).read_text()))
            if a.duration:
                emu.wait(a.duration)
            emu.check()
    except EmuCrash as e:
        print(f"CRASH: {e.summary}", file=sys.stderr)
        rc = 2
    except AssertionError as e:
        print(f"FAILED: {e}", file=sys.stderr)
        rc = 1
    except Exception as e:  # noqa: BLE001 - emulator died, bad SD spec, QMP error...
        print(f"ERROR: {type(e).__name__}: {e}", file=sys.stderr)
        rc = 3
    result_path = paths.ARTIFACTS_DIR / a.name / "result.json"
    if result_path.exists():
        result = json.loads(result_path.read_text())
    print(
        json.dumps(
            {k: result.get(k) for k in ("status", "crash", "error", "artifacts")},
            indent=2,
        )
    )
    return rc


def cmd_start(a) -> int:
    _need_emulator()
    from . import live

    st = live.start(a.name, _start_kwargs(a))
    if st.get("status") != "running":
        print(f"failed to start: {st.get('error')}", file=sys.stderr)
        return 1
    print(f"live session {a.name!r} running; artifacts: {st['artifacts']}")
    print(f"QMP socket: {st['qmp_sock']}  (drive with ./dbt emu ctl ...)")
    return 0


def cmd_stop(a) -> int:
    from . import live

    st = live.stop(a.name)
    if st is None:
        print(f"no live session {a.name!r}")
        return 1
    result = st.get("result")
    if result:
        out = {k: result.get(k) for k in ("status", "crash", "artifacts")}
    else:
        out = {
            "status": "stopped (no result.json: supervisor exited early)",
            "crash": st.get("crash"),
            "artifacts": st.get("artifacts"),
        }
    print(json.dumps(out, indent=2))
    return 0


def cmd_status(a) -> int:
    from . import live

    paths.LIVE_DIR.mkdir(parents=True, exist_ok=True)
    found = False
    for f in sorted(paths.LIVE_DIR.glob("*.json")):
        st = live.read_state(f.stem) or {}
        alive = live._pid_alive(st.get("supervisor_pid"))
        status = st.get("status") if alive or st.get("status") != "running" else "dead"
        print(
            f"{f.stem}: {status}  crash={st.get('crash')}  artifacts={st.get('artifacts')}"
        )
        found = True
    if not found:
        print("no live sessions")
    return 0


def cmd_ctl(a) -> int:
    from . import live
    from .session import ANSI, NONPRINT
    from .watcher import summarize

    dz, st = live.attach(a.name)
    out = None
    try:
        v, args = a.verb, a.args
        if v == "press":
            dz.press(args[0], int(args[1]) if len(args) > 1 else 120)
        elif v == "chord":  # hold several controls at once, e.g. chord SHIFT SAVE
            with dz.latch(args):
                dz.wait(0.2)
        elif v == "turn":
            dz.turn(args[0], int(args[1]))
        elif v == "pad":
            dz.pad(int(args[0]), int(args[1]), int(args[2]) if len(args) > 2 else 120)
        elif v == "type":
            dz.type_text(" ".join(args))
        elif v == "load-song":
            dz.load_song(" ".join(args))
        elif v == "wait":
            dz.wait(float(args[0]))
        elif (
            v == "events"
        ):  # raw events, as in fuzz events.json: [["pad",0,6,1],["wait",0.1],...]
            for ev in json.loads(" ".join(args)):
                if ev[0] == "wait":
                    dz.wait(ev[1])
                elif ev[0] == "button":
                    dz.button(ev[1], ev[2], bool(ev[3]))
                elif ev[0] == "pad":
                    dz.pad_event(ev[1], ev[2], bool(ev[3]))
                elif ev[0] == "encoder":
                    dz.encoder(ev[1], ev[2])
                else:
                    sys.exit(f"bad event {ev!r}")
        elif v == "oled":
            out = dz.oled_text()
        elif v == "seg":
            out = dz.read_segment()
        elif v == "leds":
            out = " ".join(sorted(k for k, on in dz.read_leds().items() if on))
        elif v == "pads":
            grid = dz.read_pads()
            out = "\n".join(
                " ".join(
                    "{:02x}{:02x}{:02x}".format(*grid[x][y]) for x in range(len(grid))
                )
                for y in reversed(range(len(grid[0])))
            )
        elif v == "shot":
            path = (
                args[0]
                if args
                else str(Path(st["artifacts"]) / f"shot-{int(time.time())}.png")
            )
            out = dz.screenshot(path)
        elif v == "log":
            n = int(args[0]) if args else 40
            text = (Path(st["artifacts"]) / "serial.log").read_text(encoding="latin-1")
            out = "\n".join(NONPRINT.sub("", ANSI.sub("", text)).splitlines()[-n:])
        elif v == "controls":
            from delugemu import Control

            out = " ".join(c.name for c in Control)
        else:
            sys.exit(f"unknown ctl verb {v!r}")
    finally:
        dz.q.close()
    if out is not None:
        print(out)
    report = live.crash_report(st)
    if report:
        print(
            f"CRASH: {summarize(report)} (see {Path(st['artifacts']) / 'crash.txt'})",
            file=sys.stderr,
        )
        return 2
    return 0


def cmd_fuzz(a) -> int:
    _need_emulator()
    from . import fuzz

    replay = setup = None
    sd, seed = a.sd, a.seed
    if a.replay:
        rec = json.loads(Path(a.replay).read_text())
        replay, setup = rec["gestures"], rec.get("setup")
        sd, seed = (
            (a.sd if a.sd != "empty" else rec.get("sd", sd)),
            rec.get("seed", seed),
        )
        if a.first:
            replay = replay[: a.first]
    elif a.setup:
        setup = json.loads(Path(a.setup).read_text())["steps"]
    name = a.name or (f"fuzz-{seed}" if not a.replay else f"replay-{seed}")
    out = fuzz.run(
        name,
        sd=sd,
        seed=seed,
        actions=a.actions,
        elf=_need_elf(a.elf),
        probe_every=a.probe_every,
        replay=replay,
        avoid=set(a.avoid or ()),
        setup=setup,
        weights=json.loads(a.weights) if a.weights else None,
        anchors=json.loads(a.anchors) if a.anchors else None,
    )
    print(json.dumps(out, indent=2))
    return {"ok": 0, "crash": 2, "hang": 4}.get(out["status"], 3)


def cmd_minimize(a) -> int:
    _need_emulator()
    from . import minimize

    events = Path(a.events)
    expect = a.expect
    if not expect:
        fz = events.parent / "fuzz.json"
        detail = json.loads(fz.read_text()).get("detail", "") if fz.exists() else ""
        expect = [detail.split("(")[-1].rstrip(")")] if detail else []
    if not expect:
        sys.exit("give --expect (e.g. an error code like E411)")
    out = (
        Path(a.out) if a.out else paths.ARTIFACTS_DIR / f"minimize-{events.parent.name}"
    )
    best = minimize.minimize(events, expect, a.elf, a.jobs, a.tries, out)
    print(
        json.dumps(
            {"gestures": len(best["gestures"]), "best": str(out / "best.json")},
            indent=2,
        )
    )
    return 0


def cmd_sd(a) -> int:
    img = sdcard.build_image(a.spec)
    if img is None:
        print("no card")
        return 0
    if a.ls is not None:
        print(sdcard.listdir(img, a.ls))
    elif a.get:
        print(sdcard.extract(img, a.get[0], Path(a.get[1])))
    else:
        print(img)
    return 0


# --------------------------------------------------------------------------


def parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="dbt emu",
        description="Run the Deluge firmware without hardware, in the delugemu emulator. "
        "See tests/emu/README.md.",
    )
    sub = p.add_subparsers(dest="cmd", required=True)

    s = sub.add_parser(
        "setup", help="fetch + build the pinned emulator, install test deps"
    )
    s.add_argument("--force", action="store_true", help="rebuild even if up to date")
    s.set_defaults(fn=cmd_setup)

    s = sub.add_parser(
        "build", help="build the emulator firmware (RTT off, logs on serial)"
    )
    s.add_argument(
        "config",
        nargs="?",
        default="Debug",
        choices=["Debug", "RelWithDebInfo", "Release"],
    )
    s.set_defaults(fn=cmd_build)

    s = sub.add_parser(
        "test", help="run the emulator test suite (extra args go to pytest)"
    )
    s.add_argument(
        "--no-build", action="store_true", help="don't rebuild the firmware first"
    )
    s.add_argument("pytest_args", nargs=argparse.REMAINDER)
    s.set_defaults(fn=cmd_test)

    s = sub.add_parser(
        "run", help="boot once (+ optional scenario), write artifacts, exit"
    )
    _add_launch_args(s)
    s.add_argument(
        "--name", default="run", help="artifacts subdirectory (default: run)"
    )
    s.add_argument("--script", help="JSON scenario to run (see tests/emu/scenarios/)")
    s.add_argument(
        "--duration", type=float, default=0, help="extra seconds to keep running"
    )
    s.set_defaults(fn=cmd_run)

    s = sub.add_parser("start", help="boot a live session in the background")
    _add_launch_args(s)
    s.add_argument("--name", default="live", help="session name (default: live)")
    s.set_defaults(fn=cmd_start)

    s = sub.add_parser("ctl", help="drive the live session")
    s.add_argument("--name", default="live")
    s.add_argument(
        "verb",
        help="press NAME [ms] | chord NAME NAME.. | turn ENC N | pad X Y [ms] | type TEXT | "
        "load-song PATH | wait S | events JSON | oled | seg | leds | pads | shot [PATH] | log [N] | controls",
    )
    s.add_argument("args", nargs="*")
    s.set_defaults(fn=cmd_ctl)

    s = sub.add_parser("stop", help="stop the live session")
    s.add_argument("--name", default="live")
    s.set_defaults(fn=cmd_stop)

    s = sub.add_parser("status", help="list live sessions")
    s.set_defaults(fn=cmd_status)

    s = sub.add_parser("fuzz", help="random front-panel input until a crash or hang")
    s.add_argument("--elf")
    s.add_argument("--sd", default="empty", help="card spec (default: empty)")
    s.add_argument("--seed", type=int, default=1)
    s.add_argument(
        "--actions", type=int, default=300, help="gestures to run (default 300)"
    )
    s.add_argument(
        "--probe-every",
        type=int,
        default=20,
        help="hang probe interval in gestures (0 = off)",
    )
    s.add_argument("--avoid", action="append", help="button never to tap (repeatable)")
    s.add_argument(
        "--setup", help="scenario JSON whose steps run first (e.g. load a song)"
    )
    s.add_argument(
        "--weights", help='gesture mix as JSON, e.g. \'{"pad": 50, "hold_pad": 30}\''
    )
    s.add_argument(
        "--anchors",
        help='anchor macros as JSON, e.g. \'[["LOAD", "SELECT_ENC"]]\' (see fuzz.ANCHORS)',
    )
    s.add_argument("--replay", help="events.json from an earlier run to replay")
    s.add_argument("--first", type=int, help="with --replay: only the first N gestures")
    s.add_argument(
        "--name", help="artifacts name (default fuzz-<seed> / replay-<seed>)"
    )
    s.set_defaults(fn=cmd_fuzz)

    s = sub.add_parser(
        "minimize", help="shrink a crashing fuzz events.json to a short repro"
    )
    s.add_argument("events", help="events.json of a crashing fuzz run")
    s.add_argument(
        "--expect",
        action="append",
        help="crash summary substring that counts (repeatable)",
    )
    s.add_argument("--elf")
    s.add_argument("--jobs", type=int, default=3, help="parallel replays (default 3)")
    s.add_argument(
        "--tries",
        type=int,
        default=2,
        help="replays before judging a candidate (default 2)",
    )
    s.add_argument(
        "--out", help="output dir (default build-emu/artifacts/minimize-<run>)"
    )
    s.set_defaults(fn=cmd_minimize)

    s = sub.add_parser("sd", help="build an SD image (prints its path), or inspect one")
    s.add_argument(
        "spec", nargs="?", default="basic", help="fixture name, 'factory', dir or .img"
    )
    s.add_argument("--ls", metavar="CARD_DIR", help="list a directory on the card")
    s.add_argument(
        "--get", nargs=2, metavar=("CARD_PATH", "DEST"), help="copy a file off the card"
    )
    s.set_defaults(fn=cmd_sd)
    return p


def main(argv=None) -> int:
    a = parser().parse_args(argv)
    try:
        return a.fn(a) or 0
    except (RuntimeError, FileNotFoundError, TimeoutError) as e:
        print(f"error: {e}", file=sys.stderr)
        return 3


if __name__ == "__main__":
    sys.exit(main())
