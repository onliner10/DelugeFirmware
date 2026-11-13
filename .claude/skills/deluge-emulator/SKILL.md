---
name: deluge-emulator
description: Run the Deluge firmware in the delugemu emulator to reproduce bugs, test changes, or explore behaviour without hardware. Use when asked to run/test the firmware, reproduce a crash or UI bug, check that a change works on the device, or hunt for bugs.
---

# Running the Deluge firmware in the emulator

Full reference: `tests/emu/README.md`. This skill is the workflow.

## 0. Make sure it's set up

```sh
test -x .emu/delugemu/qemu/build/qemu-system-arm || ./dbt emu setup   # ~15 min, once
./dbt emu build                                                       # after every source change
```

`./dbt emu build` builds `build-emu/Debug/deluge.elf`, a Debug build with RTT
off so debug logs reach `serial.log`. Don't use `build/Debug/deluge.elf` from
`./dbt build`: it runs, but you get no logs.

## 1. Explore interactively

```sh
./dbt emu start --sd basic            # or --sd empty | none | factory | <dir> | <img>
./dbt emu ctl controls                # names for press/chord/turn
./dbt emu ctl press PLAY
./dbt emu ctl oled                    # look at the screen after every step
./dbt emu ctl leds
./dbt emu ctl log 40                  # firmware D_PRINTLN output
./dbt emu stop
```

Reading the OLED: `ctl oled` prints 48 rows of `#`/`.`. Read the large glyphs
as text. If unsure, `ctl shot /tmp/x.png` and view the image.

Pads: `pad X Y`, where X is 0-15 for the main grid and 16/17 for the audition
and mute columns, and Y=0 is the bottom row. `pads` prints colours with the top
row first.

## 2. Turn it into a repro

Write the steps as JSON (verbs: `press`, `turn`, `pad`, `chord`, `type`,
`load_song`, `wait`, `wait_log`, `assert_led`, `assert_log`, `assert_no_log`,
`snapshot`, ...), then:

```sh
./dbt emu run --sd basic --script repro.json --name repro
cat build-emu/artifacts/repro/result.json
```

Exit codes: 0 ok, 1 assertion failed, 2 firmware crashed.

## 3. When it crashes

`build-emu/artifacts/<name>/crash.txt` has:
- `kind=freeze`: the `freezeWithError` code (e.g. `"B001"`) and a full backtrace.
  Grep the source for the code: `grep -rn '"B001"' src/`.
- `kind=cpu_fault`: the exception type, `faulting_pc` with its **source line**,
  and `caller_lr` (the call site). The backtrace is from the abort handler, so
  trust faulting_pc/caller_lr instead.

The end of `serial.log` usually shows what the firmware was doing just before.

## 4. Fix and verify

Change the code, `./dbt emu build`, rerun the same scenario. Save it under
`tests/emu/scenarios/` (set `"sd"` in the JSON) so `./dbt emu test` keeps it
fixed. Then run the whole suite: `./dbt emu test`.

## Bug-hunting tips

- Look for crashes under unusual input sequences: rapid view switching,
  shift/hold combos (`chord`), encoder turns during song load, saving with odd
  names (`type`), loading and playing different songs (`--sd factory` has many).
- `assert_no_log` with a regex for warnings you care about turns log noise into
  a check.
- The emulator runs in real time and is slower than hardware. Audio overload
  warnings ("direness", "latency high") are expected here and aren't bugs by
  themselves.
- Don't claim a timing or audio-quality bug from the emulator alone. Say it
  needs hardware confirmation.
