# Emulator bug hunt: findings

Severe bugs (device freezes from `FREEZE_WITH_ERROR`) found by driving the
firmware **only through front-panel input** (buttons, pads, encoders) in
delugemu, on firmware `106814f`. The bugs were found with `./dbt emu fuzz`, then
shrunk with `./dbt emu minimize` and by hand. Each recording replays the exact
input from a fresh boot: `./dbt emu fuzz --replay <file> --probe-every 1`.

The emulator is not cycle-exact, so each recording reproduces the bug with some
probability. The rate is given for each bug below. Short repros in `repros/`
run as `xfail` tests in `./dbt emu test`. Long, state-heavy recordings are in
`repros/long/`, each with the crash report it produced.

---

## 1. Freeze E369 / E410: SAVE pressed while a kit clip is opening from song view (minimal repro)

**Repro: `repros/e369_save_while_opening_kit_clip.json`.** 3/3 on the emulator
build, and 3/3 on the standard `./dbt build debug` firmware.

1. Load `SONGS/MULTISAMPLE.XML` from the `basic` card. It has a kit clip and an
   audio track.
2. Press **SONG** to open session view.
3. Tap pad **x=0, y=3**. This opens the kit clip and starts the zoom-in animation.
4. Within about 0.25 s, press **SAVE**. The device freezes with **E369**.

To narrow it down, I ran variants of this sequence, each 3 times:

- Steps that had no effect: playback running or stopped, holding RECORD,
  turning a knob afterwards.
- Variants that never froze: the pad without SAVE, SAVE without the pad, SAVE
  before the pad, SAVE 0.4 s or more after the pad, or the same steps on the
  default song (a synth clip).

Two freeze codes appeared:

- **E369** (`model/model_stack.h:149`). When the zoom transition finishes,
  `PadLEDs::timerRoutine` calls `changeRootUI(instrumentClipView)`. The call
  chain continues through `InstrumentClipView::opened`, `performActualRender`
  and `ModelStackWithNoteRow::getLoopLength` to `getTimelineCounter()`, which
  finds a null timeline counter.
- **E410**. Seen when a gold knob is turned before the transition fires. The
  knob reaches `View::modEncoderAction`, then
  `GlobalEffectable::getParamFromModEncoder`, and then
  `ParamManager::getUnpatchedParamSetSummary` on a ParamManager that has no
  param collection.

The E369 that the song-swap fuzz campaign found (seed I-902) also had a pad tap
and SAVE in its last steps. It was this bug.

## 2. Freeze E410: input handled while a song is loading or swapping during playback

These were seen in fuzz runs, and no minimal repro has been found. Two runs
froze with E410 inside `LoadSongUI::performLoad`:

- **(a) MOD button during the song-swap wait.**
  Files: `repros/long/e410_mod_button_during_song_swap.*`.
  Call chain: `performLoad` → `yield()` → `readButtonsAndPads` →
  `Sound::modButtonAction` → `Stutterer::endStutter` →
  `ParamManager::getUnpatchedParamSet` → E410.
  The last inputs were Enter on the browser keyboard (the song starts loading
  while the current song plays), then SYNTH, SCALE_MODE and MOD5.
  This run used a build that only reverts #5298, which touches unrelated
  shortcut code. Replaying the recording on clean firmware froze with E410
  again, by path (b).
- **(b) The audio engine renders the old song while it is being destroyed.**
  Files: `repros/long/e410_render_song_being_deleted.*`.
  Call chain: `performLoad` (line 537, `toDelete->~Song()`) →
  `~NoteRowVector` → `AudioEngine::routineWithClusterLoading` →
  `Song::renderAudio` on the same Song object →
  `GlobalEffectableForClip::renderOutput(paramManagerForClip=0)` → E410.

Root-cause notes from the code:

- **Input runs mid-swap.** In task-manager builds, `performLoad` yields to the
  scheduler mid-swap (`load_song_ui.cpp:453` and `:507`). The scheduler runs
  `readButtonsAndPadsOnce`, so button and pad handlers run while
  `currentSong` and `preLoadedSong` are half swapped. In the stopped path
  `currentSong` is null at that point.
- **Mod-controllable not reset.** During playback,
  `view.activeModControllableModelStack` is deliberately not reset
  (`load_song_ui.cpp:349`). The comment there says "it won't get deleted",
  but the old song is deleted after the swap.
- **The swap wait can end early.** The `yield` at `:507` only waits for
  `currentUIMode` to stop being `…_ARMED` or `…_UNARMED`. The
  non-task-manager branch instead waits for `…_NEW_SONG_PLAYING`.
- **Exact trigger for (b) unknown.** A diagnostic build that freezes if
  `toDelete == currentSong` at `swapDone` did not trigger for 12
  single-button candidates.

## 3. Freeze E411: sound editor LPF menu with a kit, audio or song context

One fuzz run on clean firmware.
Files: `repros/long/e411_lpf_menu_wrong_param_manager.*`, card `factory_song`.
The recording is partly minimized, from 323 to 175 gestures. It freezes with
either E411 or the RN01 below.

Call chain: `SoundEditor::renderOLED` → `HorizontalMenu::renderOLED(lpfMenu)`
→ `preparePaging(currentItem=lpfModeMenu)` →
`patched_param::Integer::readCurrentValue(lpfFreqMenu)` →
`ParamManager::getPatchedParamSet` → **E411**.

`lpfModeMenu` is shared between the synth `lpfMenu`, which uses patched params,
and `globalLPFMenu`, which kits (affect entire), audio clips and song view use
(`gui/ui/menus.cpp:728` and the shortcut tables). The synth menu was being drawn
while the editor's ParamManager had no patched set. These attempts did not
crash:

- opening the LPF menu from kit affect-entire;
- toggling AFFECT_ENTIRE while that menu is open.

## 4. Freeze RN01: "Rename drum" entered with no selected drum

Replaying the E411 recording on clean firmware froze at its final gesture with
**RN01**. Call chain: `SoundEditor::buttonAction` (SELECT) →
`enterSubmenu(drumNameEditMenu)` → `RenameDrumUI::getCurrentName`, which finds
`kit->selectedDrum == nullptr` (`gui/ui/rename/rename_drum_ui.cpp:35`).
Files: `repros/long/rn01_rename_drum_without_selected_drum.*`.

---

## Harness validation

I reverted the fix from 106814f (#5298) and replayed KEYBOARD, then
SHIFT+SELECT, then SHIFT+pad(5,7). The harness reported a **wild jump** (the
CPU executing outside firmware code). With the fix in place, the same replay
passes.

## Not reported

- `couldn't find key to delete` from `ordered_resizeable_array_with_multi_word_key`
  is the allocator's tolerated "lost track of empty space" case when internal
  RAM is full. Its debug print passes `uint32_t` keys to `%s`, which prints
  garbage. This only affects Debug builds.
- Audio overload warnings ("direness", "latency high") are expected because
  the emulator runs slower than the hardware. Timing and audio findings need
  hardware confirmation. The ~0.25 s window in bug 1 will also differ on
  hardware.
