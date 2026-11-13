# Emulator bug hunt: findings

Severe bugs (device freezes: `FREEZE_WITH_ERROR`) found by driving the firmware
**only through front-panel input** (buttons, pads, encoders) in delugemu, on
the firmware at `106814f` built with `./dbt emu build` (Debug, RTT off).
Found with `./dbt emu fuzz`; every report below comes with a recording that
replays the exact input from a fresh boot (`./dbt emu fuzz --replay <file>`).

The emulator is not cycle-exact, so a recording reproduces the crash with some
probability, not every time; rates are given per bug. Short repros live in
`repros/` and run as `xfail` tests in `./dbt emu test`; long, state-heavy
recordings live in `repros/long/` with the crash report each produced.

All four share one theme: **input handled while the firmware is in the middle
of a state transition** (song load/swap, sound-editor context), reaching code
that assumes a consistent state and freezing on its sanity check.

---

## 1. E369: freeze when pressing SONG / RECORD / SAVE while a song loads during playback

**Repro (4 gestures, ~2/3 of replays):** `repros/e369_load_during_playback.json`

1. Card: `basic` fixture. Start playback (PLAY).
2. Press LOAD, then SELECT to load the highlighted song (`MULTISAMPLE.XML`).
3. Immediately press SONG (session view).
4. Hold RECORD; press CV; tap pad x=0,y=3; press SAVE; release RECORD.
5. Turn the lower gold knob 4 detents clockwise. Freezes with **E369**.

Each step is required: every 3-gesture variant tried (dropping the knob turn,
reducing step 4 to RECORD+CV, RECORD+pad, RECORD+SAVE or CV alone, dropping
step 3) ran clean 0/3.

Crash: a pad-LED transition finishes and switches the root UI to the clip view
while the clip it points at is gone:
`PadLEDs::timerRoutine` → `changeRootUI(instrumentClipView)` →
`InstrumentClipView::opened` → `performActualRender` →
`ModelStackWithNoteRow::getLoopLength` → `getTimelineCounter()` → E369
(`model/model_stack.h:149`, null timeline counter).

## 2. E410: freeze from input handled during a song load/swap

Two independent runs froze with E410 on two different paths, both inside
`LoadSongUI::performLoad`:

- **(a) MOD button while the loaded song waits to swap in**
  (`repros/long/e410_mod_button_during_song_swap.*`, 908 gestures).
  `performLoad` → `yield()` (task scheduler) → `readButtonsAndPads` →
  `Sound::modButtonAction` → `Stutterer::endStutter` →
  `ParamManager::getUnpatchedParamSet` → E410. The last gestures were: Enter on
  the browser keyboard (song starts loading while the current song plays), SYNTH,
  SCALE_MODE, then MOD5.
- **(b) the audio engine renders the old song while it is being destroyed**
  (`repros/long/e410_render_song_being_deleted.*`, 838 gestures, on clean
  firmware). `performLoad` (line 537, `toDelete->~Song()`) → `~InstrumentClip`
  → `~NoteRowVector` → `AudioEngine::routineWithClusterLoading` →
  `Song::renderAudio` **on the same Song being destroyed** → `AudioOutput`
  → `GlobalEffectableForClip::renderOutput(paramManagerForClip=0)` → E410.

Root-cause notes (from the code): with `USE_TASK_MANAGER`, `performLoad` calls
`yield()`/`yieldWithTimeout()` while the song swap is in progress
(`load_song_ui.cpp:453`, `:507`). The scheduler then runs every task,
including `readButtonsAndPadsOnce`, so arbitrary button/pad handlers run while
`currentSong`/`preLoadedSong` are mid-swap (or `currentSong` is null in the
not-playing path). Also, the `yield` at `:507` waits only for `currentUIMode`
to stop being `..._ARMED`/`..._UNARMED`; the non-task-manager branch waits
for `UI_MODE_LOADING_SONG_NEW_SONG_PLAYING` specifically. If some input changes
`currentUIMode` during the wait, the swap can be skipped and execution reaches
`swapDone`, which destroys `toDelete`, the song that is still current. (A
diagnostic build checking `toDelete == currentSong` there did not trigger for
12 simple single-button candidates, so the exact trigger for (b) is not
pinned down yet.)

Reproducibility: timing-dependent. Run (a) happened on a build with only the
#5298 fix reverted (sound-editor shortcut code, not on this path). Replaying
its recording on clean firmware froze with E410 again, via path (b). Replaying
(b)'s own recording once more ran clean, so expect several attempts.

## 3. E411: freeze in the sound editor's LPF menu (kit / global context)

`repros/long/e411_lpf_menu_wrong_param_manager.*` (323 gestures, card
`factory_song`, clean firmware).

`doAnyPendingUIRendering` → `SoundEditor::renderOLED` →
`HorizontalMenu::renderOLED(lpfMenu)` → `preparePaging(currentItem=lpfModeMenu)`
→ `patched_param::Integer::readCurrentValue(lpfFreqMenu)` →
`ParamManager::getPatchedParamSet` → **E411**.

`lpfModeMenu` is shared between the synth `lpfMenu` (patched params) and the
global `globalLPFMenu` used for kits (affect entire), audio clips and song
view (`gui/ui/menus.cpp:728`, shortcut tables). The synth `lpfMenu` was being
rendered while the sound editor's ParamManager had no patched param set
(i.e. a kit-global/audio/song context). The final gestures pressed KIT and
navigated menus with SELECT after SAVE+KIT. Opening the LPF menu from kit
affect-entire on its own, and toggling AFFECT_ENTIRE with it open, did not
crash.

## 4. RN01: "Rename drum" entered with no selected drum

`repros/long/rn01_rename_drum_without_selected_drum.*`: replaying the E411
recording on clean firmware froze at the same final gesture with **RN01**
instead: `SoundEditor::buttonAction` (SELECT) →
`enterSubmenu(drumNameEditMenu)` → `EditName::beginSession` →
`RenameDrumUI::getCurrentName` → `kit->selectedDrum == nullptr` → RN01
(`gui/ui/rename/rename_drum_ui.cpp:35`).

---

## Harness validation

Reverting the fix of 106814f (#5298, "sound editor crash when switching from
settings menu") and replaying KEYBOARD, SHIFT+SELECT, SHIFT+pad(5,7) is reported
as a **wild jump** (CPU executing outside firmware code); with the fix it
passes. So the harness detects that known, button-reachable bug class.

## Not reported (not severe / not bugs)

- `ordered_resizeable_array_with_multi_word_key.cpp: couldn't find key to delete`
  is the allocator's tolerated "lost track of empty space" case under full
  internal RAM. Its debug print passes `uint32_t` keys to `%s` (garbage
  output, Debug builds only).
- Audio overload warnings ("direness", "latency high") are expected in the
  emulator, which runs slower than hardware.
