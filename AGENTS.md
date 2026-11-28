# Agent notes for DelugeFirmware

Bare-metal C++ firmware for the Synthstrom Deluge (Renesas RZ/A1L, Cortex-A9).
Everything goes through `./dbt` (it bootstraps its own toolchain into `toolchain/`).

- Build: `./dbt build debug` (or `release`) -> `build/Debug/deluge.elf`
- Host unit tests: `./dbt test`
- Format: `./dbt format`

## Running the firmware without hardware

Use the emulator harness: [tests/emu/README.md](tests/emu/README.md). In short:

```sh
./dbt emu setup                      # once (~15 min): builds the pinned delugemu/QEMU
./dbt emu test                       # builds build-emu/Debug/deluge.elf, runs tests/emu
./dbt emu check --elf X.elf --sd factory -e "press SONG; wait 1; expect led PLAY off; oled"
                                     # fastest way to check a change: one call, guest clock, parallel-safe
./dbt emu start --sd basic           # live session; then:
./dbt emu ctl press PLAY | turn SELECT_ENC 2 | pad 3 4 | load-song /SONGS/X.XML
./dbt emu ctl oled | leds | log 30 | shot out.png
./dbt emu stop
./dbt emu run --script repro.json    # scripted repro: exit 0 ok, 1 assertion, 2 crash, 3 error
```

- Crashes (CPU faults, `FREEZE_WITH_ERROR`) are caught. Read
  `build-emu/artifacts/<run>/crash.txt` for the faulting source line and
  backtrace, and `serial.log` for `D_PRINTLN` output.
- To check a change, use `./dbt emu check` first (see tests/emu/README.md, section 0):
  ~3 s per check, deterministic under host load, `--elf a,b` for A/B, `oled` as text.
- Don't hand-roll QEMU/QMP commands; use `./dbt emu` or `deluge_emu.EmuSession`.
- Emulator timing is approximate. Confirm timing-sensitive findings on
  hardware before claiming them.
- When you find a bug, save the repro as a scenario in `tests/emu/scenarios/`
  so it becomes a regression test.
