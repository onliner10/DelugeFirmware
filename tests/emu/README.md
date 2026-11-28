# Running the firmware without hardware (emulator harness)

`./dbt emu` boots the real firmware ELF in
[delugemu](https://github.com/SynthstromAudible/delugemu), a QEMU machine model
of the Deluge (RZ/A1L SoC, OLED, PIC pad/button matrix, SD card, audio codec).
On top of delugemu's own Python driver, this harness adds:

- **one-command setup**: the emulator is pinned in `scripts/emu/delugemu.lock`
  and built from source into `.emu/`
- **firmware logs**: `D_PRINTLN` output ends up in `serial.log`
- **crash detection**: CPU faults and `FREEZE_WITH_ERROR` are caught by gdb,
  with the source line and a backtrace
- **SD card fixtures** that are built into images automatically
- **artifacts** for every run, so a failure can be debugged from files
- **pytest tests**, **JSON scenarios** and **live sessions** for exploring by hand

## Quick start

```sh
./dbt emu setup          # once: fetch + build delugemu/QEMU (~15 min), install pytest
./dbt emu build          # build build-emu/Debug/deluge.elf (also done by `test`)
./dbt emu test           # run tests/emu (pytest); extra args go to pytest, e.g. -k load
```

Host requirements (Linux): `glib-2.0` + `pixman` dev packages, a C toolchain,
`ninja`, plus `dosfstools` and `mtools` for SD images. macOS needs Homebrew
`glib pixman ninja`. No display or audio device is needed; everything runs
headless. To use your own delugemu checkout, set `DELUGEMU_ROOT=/path/to/delugemu`.

## Ways to drive it

### 0. Quick check: `./dbt emu check` (use this first)

One command boots the firmware, runs a short script of presses and checks,
looks for crashes and stops. It prints one line per check and a `RESULT` line.

```sh
./dbt emu check --elf build-emu/Debug/deluge.elf --sd factory -e "press SONG; wait 1; expect led PLAY off; oled"
./dbt emu check --elf a.elf,b.elf --sd factory my-test.emu   # A/B: each script on each ELF, in parallel
./dbt emu check -h                                           # every step and option
```

- **Guest clock.** The default `--clock fast` runs QEMU with
  `-icount shift=2,sleep=off`, about the speed of a real Deluge. Every `wait`
  is in guest seconds, so a busy host (builds, other emulators) can't drop
  presses or turn taps into long presses. Runs are deterministic: 8 parallel
  runs of the same script give the same result. Use `--clock timectl` for load,
  voice or audio measurements. `--clock rt` is the old host-time behaviour.
- **Parallel and isolated.** Several scripts and/or comma-separated ELFs run as
  a matrix (`-j`, default 8). Each run gets a unique session name, so separate
  agents never clash.
- **Readable screen.** `oled` prints the OLED as text, read with the firmware's
  own fonts, e.g. `SOUND / ARPEGGIATOR > / [RANDOMIZER >]` (brackets mean
  highlighted). `oledraw` prints the pixels.
- **Firmware state.** Use `expect last 'TRACE' 'regex'` to check the newest log
  line matching `TRACE`; it is retried for 2 guest seconds. Add a
  `D_PRINTLN("TRACE ...")` to the firmware to expose the state you need.
- **Exit codes:** 0 ok, 1 a check failed, 2 the firmware crashed, 3 harness
  error. Artifacts (serial.log, crash.txt) are in `build-emu/artifacts/<name>/`.
- A trivial check takes about 3 s. A 19-check UI test takes about 25 s.
- **From a git worktree:** `./dbt` needs a full clone (a `.git` directory), so run
  `python3 scripts/tasks/task-emu.py check ...` instead. Set `DELUGEMU_ROOT` to an
  existing built `.emu/delugemu` so the emulator isn't fetched and built again.

Script files (`*.emu`) use one step per line or `;`-separated steps, with `#`
comments:

```text
press SONG; wait 1                       # tap a button (several names = chord)
press 0,7; wait 0.45                     # pad x,y (y 0 = bottom row)
down 6,7; turn SELECT 1; up 6,7          # hold a pad while turning an encoder
expect last AWTRACE 'A\[BW \]'           # newest AWTRACE log line matches
expect oled 'SECTION 1'
expect nolog 'FREEZE|HardFault'
```

Prefer this over a live session plus many `ctl` calls, and extend
`deluge_emu/drive.py` (new steps) rather than writing per-feature shell
helpers.

### 1. Live session (exploration, one command at a time)

```sh
./dbt emu start --sd basic           # boots in the background, returns when ready
./dbt emu ctl press PLAY             # button / encoder-click by name (ctl controls lists them)
./dbt emu ctl turn SELECT_ENC 3      # encoder detents (negative = anticlockwise)
./dbt emu ctl pad 0 0                # pad x(0-15, 16/17 = sidebar) y(0 = bottom)
./dbt emu ctl chord SHIFT SAVE       # hold several controls together
./dbt emu ctl load-song /SONGS/MULTISAMPLE.XML
./dbt emu ctl type MYSONG            # pad-grid QWERTY (naming/browsing)
./dbt emu ctl oled                   # OLED as '#'/'.' text   (seg = 7-segment)
./dbt emu ctl leds                   # indicator LEDs that are lit
./dbt emu ctl pads                   # pad colours (rrggbb, top row first)
./dbt emu ctl log 30                 # last 30 lines of firmware debug output
./dbt emu ctl shot out.png           # front-panel screenshot
./dbt emu stop                       # final snapshot + result.json
```

Every `ctl` exits with **code 2 and prints `CRASH: ...`** once the firmware has
crashed, so a crash is never silently missed. Several sessions can run side by
side with `--name`.

### 2. Scenario (repro script, no code)

```sh
./dbt emu run --sd basic --script tests/emu/scenarios/load_song_and_play.json --name myrepro
```

A JSON list of steps (`press`, `turn`, `pad`, `wait`, `load_song`, `type`,
`chord`, `assert_led`, `wait_log`, `assert_no_log`, `snapshot`, ... see
`deluge_emu/scenario.py`). Exit code 0 = ok, 1 = assertion failed, 2 = crash,
3 = harness/emulator error (e.g. QEMU died).
Put a scenario in `tests/emu/scenarios/` to make it part of `./dbt emu test`.

### 3. pytest / Python

```python
@pytest.mark.sd("basic")  # card for this test (default "empty")
def test_something(emu):  # fresh boot per test; fails on any crash
    emu.dz.load_song("/SONGS/MULTISAMPLE.XML")
    emu.dz.press("PLAY")
    emu.wait(2)  # crash-aware sleep
    emu.wait_for_log(r"some D_PRINTLN text")
    assert "PLAY" in {k for k, on in emu.dz.read_leds().items() if on}
```

`emu.dz` is delugemu's
[`DelugeSession`](https://github.com/SynthstromAudible/delugemu/blob/main/docs/scripting.md)
(`press/turn/pad/hold/latch/type_text/load_song/read_oled/oled_text/read_leds/read_pads/read_segment/screenshot/wait_sd_idle/vclock_ns/run_until`).
Outside pytest, use `EmuSession.start(name=..., sd=...)` from `deluge_emu` as a
context manager.

## Artifacts

Each run writes `build-emu/artifacts/<name>/` (`pytest/<test>`, `scenario/<name>`, `live`, ...):

| file | contents |
|---|---|
| `result.json` | `status` (ok / failed / crash / error), crash summary, firmware + emulator revs, card |
| `serial.log` | firmware debug output (`D_PRINTLN`, timestamps) and emulator messages |
| `crash.txt` | on crash: kind, error code or faulting PC with **source line**, backtrace, registers |
| `oled.txt`, `screen.png`, `state.json` | final OLED text, panel screenshot, LEDs / 7-seg / pads |
| `failure-*`, `<prefix>-*` | snapshots at a failing scenario step / `snapshot` steps |
| `sd.img` | the card after the run (deleted when the run is ok, unless `DELUGE_EMU_KEEP_SD=1`); inspect with `./dbt emu sd build-emu/artifacts/x/sd.img --ls /SONGS` |
| `gdb.log` | the crash watcher's raw output |

## SD cards

`--sd` / `@pytest.mark.sd(...)` take:

- a fixture under `tests/emu/fixtures/sd/`: `empty`, `basic`. A fixture is a
  folder copied to the card root. Its `links.txt` can pull files in from
  elsewhere in the repo, so binaries aren't duplicated.
- `factory`: the Synthstrom factory card contents (about 1 GB, downloaded once into `.emu/`)
- `none`: no card inserted
- any directory, or an existing `.img`

Images are cached in `build-emu/sd/`. Each run gets its own sparse copy, so the
firmware can save without affecting anything else.
`./dbt emu sd <spec> [--ls DIR | --get CARD_PATH DEST]` builds or inspects one.

## How it works / caveats

- **Firmware flavour.** `./dbt emu build` configures `build-emu/` with
  `-DENABLE_RTT=OFF`, so the debug text goes to the DIN-MIDI UART, which
  delugemu prints. The emulator doesn't model RTT. Otherwise it is the normal
  Debug build. Any ELF or BIN works with `--elf`, but with RTT on you get no logs.
- **Crash watcher.** QEMU starts paused with its gdbstub open. A batch
  `arm-none-eabi-gdb` sets breakpoints on `handle_cpu_fault` and
  `freezeWithError`, then resumes. When one is hit it writes `crash.txt` and
  detaches, and the firmware goes on to its normal fault or freeze screen.
  To use your own debugger instead, pass `--no-watch --qemu-arg=-gdb --qemu-arg=tcp::1234`
  and attach with `toolchain/current/arm-none-eabi-gcc/bin/arm-none-eabi-gdb build-emu/Debug/deluge.elf -ex 'target remote :1234'`.
- **Parallel runs** are fine as long as each uses its own `--name` (artifact
  directory). Each session picks its own QMP socket and gdb port.
- **Hangs** are not detected automatically. Use `wait_for_log` or `wait_until`
  with a timeout, and check `serial.log`.
- **Timing.** By default the guest runs against the host's real-time clock, so
  heavy songs on a slow or loaded host report audio overload ("direness") that
  real hardware wouldn't. `--icount <shift>` gives an instruction-counted
  clock, but input still arrives in wall-clock time. For bit-exact replays see
  delugemu's `vclock_ns`/`run_until` (delugemu `docs/scripting.md`).
- **Fidelity.** Peripherals are mostly instant (the SD card is timed like a
  real card). USB, MIDI and CV behaviour is approximate. Confirm timing-sensitive
  findings on hardware. delugemu's `docs/register-coverage.md` lists what is modelled.
- **Bumping the emulator.** Edit `rev` in `scripts/emu/delugemu.lock`, then
  `./dbt emu setup && ./dbt emu test`.
