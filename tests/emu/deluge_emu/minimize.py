"""Shrink a crashing fuzz recording to a short button sequence.

    ./dbt emu minimize build-emu/artifacts/fz-G-702/events.json --expect E411 --expect RN01

Every candidate is replayed from a fresh boot (`fuzz --replay`) and counts as
reproducing if the run ends in a crash/hang whose summary contains one of the
--expect strings (default: the original crash summary). Steps:

1. tail search: the shortest suffix (last 4, 8, 16, ... gestures) that still
   reproduces - most crashes need only a little state;
2. ddmin: repeatedly try dropping chunks of gestures, keeping any removal that
   still reproduces, halving the chunk size down to single gestures.

Replays run `--jobs` at a time. The emulator isn't cycle-deterministic, so a
candidate is retried `--tries` times before being judged non-reproducing.
The current best sequence is always saved as <out>/best.json.
"""

from __future__ import annotations

import json
import subprocess
import sys
import uuid
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

from . import paths


def _replay(
    rec: dict, elf: str | None, tries: int, expect: list[str], work: Path
) -> bool:
    for _ in range(tries):
        name = f"min-{uuid.uuid4().hex[:8]}"
        f = work / f"{name}.json"
        f.write_text(json.dumps(rec))
        cmd = [
            sys.executable,
            "-m",
            "deluge_emu.cli",
            "fuzz",
            "--replay",
            str(f),
            "--name",
            name,
            "--probe-every",
            "5",
        ]
        if elf:
            cmd += ["--elf", elf]
        proc = subprocess.run(
            cmd,
            cwd=paths.FW_ROOT / "tests" / "emu",
            capture_output=True,
            text=True,
            check=False,
        )
        out = proc.stdout
        f.unlink(missing_ok=True)
        try:
            res = json.loads(out[out.index("{") :])
        except ValueError:
            # Harness failure, not a verdict on the candidate: keep the evidence.
            (work / f"{name}.error.log").write_text(
                out + "\n--- stderr ---\n" + proc.stderr
            )
            continue
        hit = res.get("status") in ("crash", "hang") and any(
            e in (res.get("detail") or "") for e in expect
        )
        if hit:
            return True
        import shutil

        shutil.rmtree(paths.ARTIFACTS_DIR / name, ignore_errors=True)
    return False


def minimize(
    events: Path, expect: list[str], elf: str | None, jobs: int, tries: int, out: Path
) -> dict:
    out = out.resolve()  # replays run with cwd=tests/emu
    rec = json.loads(events.read_text())
    gestures = rec["gestures"]
    out.mkdir(parents=True, exist_ok=True)

    def mk(gs):
        return {
            "seed": rec.get("seed", 0),
            "sd": rec.get("sd"),
            "setup": rec.get("setup"),
            "gestures": gs,
        }

    def test_many(cands):
        with ThreadPoolExecutor(jobs) as pool:
            return list(
                pool.map(lambda gs: _replay(mk(gs), elf, tries, expect, out), cands)
            )

    def save(gs, note):
        (out / "best.json").write_text(json.dumps(mk(gs)))
        print(f"[minimize] {note}: {len(gs)} gestures", flush=True)

    # 1. shortest reproducing tail
    best = gestures
    ks = []
    k = 4
    while k < len(gestures):
        ks.append(k)
        k *= 2
    for i in range(0, len(ks), jobs):
        batch = ks[i : i + jobs]
        results = test_many([gestures[-k:] for k in batch])
        hit = next((k for k, ok in zip(batch, results) if ok), None)
        if hit:
            best = gestures[-hit:]
            break
    save(best, "tail search")

    # 2. ddmin-style chunk removal
    chunk = max(1, len(best) // 2)
    while chunk >= 1:
        changed = True
        while changed:
            changed = False
            starts = list(range(0, len(best), chunk))
            for i in range(0, len(starts), jobs):
                batch = starts[i : i + jobs]
                cands = [best[:s] + best[s + chunk :] for s in batch]
                results = test_many(cands)
                hit = next((c for c, ok in zip(cands, results) if ok and c), None)
                if hit is not None:
                    best = hit
                    save(best, f"chunk {chunk}")
                    changed = True
                    break
        chunk //= 2
    save(best, "done")
    return mk(best)
