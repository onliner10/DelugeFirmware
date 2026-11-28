/*
 * Copyright © 2026 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

// Where gestures make sense, for command search (gesture_list.h). Asked with nothing held.
#include "gui/combos/gesture_list.h"
#include "gui/combos/tables.h"
#include "gui/ui/keyboard/keyboard_screen.h"
#include "gui/ui/root_ui.h"
#include "gui/ui/ui.h"
#include "gui/views/arranger_view.h"
#include "gui/views/instrument_clip_view.h"
#include "gui/views/session_view.h"
#include "model/clip/clip.h"
#include "model/clip/instrument_clip.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "storage/flash_storage.h"

namespace deluge::gui::combos::gesture {

namespace {
bool keyboardLayoutIs(KeyboardLayoutType layout) {
	Clip* clip = getCurrentClip();
	return clip != nullptr && clip->type == ClipType::INSTRUMENT
	       && static_cast<InstrumentClip*>(clip)->keyboardState.currentLayout == layout;
}

// Whether plain PLAY starts from the screen (playback_handler.cpp), so the restart shortcut starts from the start
bool playStartsFromScreen() {
	return runtimeFeatureSettings.get(RuntimeFeatureSettingType::AlternativePlaybackStartBehaviour)
	           == RuntimeFeatureStateToggle::On
	       || (getRootUI() == &arrangerView && currentSong->arrangerAutoScrollModeActive);
}
} // namespace

bool applies_kit() {
	return getCurrentOutputType() == OutputType::KIT;
}
bool applies_notkit() {
	return getCurrentOutputType() != OutputType::KIT;
}
bool applies_synth() {
	return getCurrentOutputType() == OutputType::SYNTH;
}
bool applies_midi() {
	return getCurrentOutputType() == OutputType::MIDI_OUT;
}
bool applies_notsynth() {
	return getCurrentOutputType() != OutputType::SYNTH;
}
bool applies_notmidi() {
	return getCurrentOutputType() != OutputType::MIDI_OUT;
}
bool applies_notcv() {
	return getCurrentOutputType() != OutputType::CV;
}
bool applies_instrument() {
	return getCurrentClip() != nullptr && getCurrentClip()->type == ClipType::INSTRUMENT;
}
bool applies_audio() {
	return getCurrentClip() != nullptr && getCurrentClip()->type == ClipType::AUDIO;
}
bool applies_dx7() {
	return runtimeFeatureSettings.get(RuntimeFeatureSettingType::EnableDX7Engine) == RuntimeFeatureStateToggle::On;
}
bool applies_quantize() {
	return runtimeFeatureSettings.get(RuntimeFeatureSettingType::Quantize) == RuntimeFeatureStateToggle::On;
}
bool applies_randomizer() {
	return runtimeFeatureSettings.get(RuntimeFeatureSettingType::DrumRandomizer) == RuntimeFeatureStateToggle::On;
}
bool applies_kitrandom() {
	return getCurrentOutputType() == OutputType::KIT
	       && runtimeFeatureSettings.get(RuntimeFeatureSettingType::DrumRandomizer) == RuntimeFeatureStateToggle::On;
}
bool applies_sticky() {
	return runtimeFeatureSettings.get(RuntimeFeatureSettingType::ShiftIsSticky) == RuntimeFeatureStateToggle::On;
}
bool applies_skipback() {
	return runtimeFeatureSettings.get(RuntimeFeatureSettingType::Skipback) != 0;
}
bool applies_overlay() {
	return runtimeFeatureSettings.get(RuntimeFeatureSettingType::ShortcutOverlay)
	       != static_cast<uint32_t>(RuntimeFeatureStateShortcutOverlay::Off);
}
bool applies_emudisplay() {
	return runtimeFeatureSettings.get(RuntimeFeatureSettingType::EmulatedDisplay)
	       != RuntimeFeatureStateEmulatedDisplay::Hardware;
}
bool applies_finetempo() {
	return runtimeFeatureSettings.get(RuntimeFeatureSettingType::FineTempoKnob) == RuntimeFeatureStateToggle::On;
}
bool applies_trimstart() {
	return runtimeFeatureSettings.get(RuntimeFeatureSettingType::TrimFromStartOfAudioClip)
	       == RuntimeFeatureStateToggle::On;
}
bool applies_loopads() {
	return runtimeFeatureSettings.get(RuntimeFeatureSettingType::EnableGridViewLoopPads)
	       == RuntimeFeatureStateToggle::On;
}
bool applies_rows() {
	return getRootUI() == &sessionView && currentSong->sessionLayout == SessionLayoutType::SessionLayoutTypeRows;
}
bool applies_grid() {
	return getRootUI() == &sessionView && currentSong->sessionLayout == SessionLayoutType::SessionLayoutTypeGrid;
}
bool applies_isolayout() {
	return keyboardLayoutIs(KeyboardLayoutType::KeyboardLayoutTypeIsomorphic)
	       || keyboardLayoutIs(KeyboardLayoutType::KeyboardLayoutTypeInKey);
}
bool applies_chordkb() {
	return keyboardLayoutIs(KeyboardLayoutType::KeyboardLayoutTypeChord);
}
bool applies_chordlib() {
	return keyboardLayoutIs(KeyboardLayoutType::KeyboardLayoutTypeChordLibrary);
}
bool applies_drumkb() {
	return keyboardLayoutIs(KeyboardLayoutType::KeyboardLayoutTypeDrums);
}
bool applies_columns() {
	return !keyboardLayoutIs(KeyboardLayoutType::KeyboardLayoutTypeDrums)
	       && !keyboardLayoutIs(KeyboardLayoutType::KeyboardLayoutTypeNorns);
}
bool applies_restartx() {
	return playbackHandler.playbackState && !FlashStorage::accessibilityShortcuts;
}
bool applies_restartcross() {
	return playbackHandler.playbackState && FlashStorage::accessibilityShortcuts;
}
bool applies_fromscreenx() {
	return !playbackHandler.playbackState && !FlashStorage::accessibilityShortcuts && !playStartsFromScreen();
}
bool applies_fromstartx() {
	return !playbackHandler.playbackState && !FlashStorage::accessibilityShortcuts && playStartsFromScreen();
}
bool applies_fromscreencross() {
	return !playbackHandler.playbackState && FlashStorage::accessibilityShortcuts && !playStartsFromScreen();
}
bool applies_selection() {
	return getRootUI() == &instrumentClipView && instrumentClipView.hasNoteSelection();
}
bool applies_noselection() {
	return !(getRootUI() == &instrumentClipView && instrumentClipView.hasNoteSelection());
}
bool applies_fromstartcross() {
	return !playbackHandler.playbackState && FlashStorage::accessibilityShortcuts && playStartsFromScreen();
}

// clang-format off
namespace global {
PLACE_SDRAM_RODATA constexpr Gesture Play{.input = gbtn::PLAY, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_PLAY_STOP, .keywords = "start transport"};
PLACE_SDRAM_RODATA constexpr Gesture RestartPlay{.hold = gbtn::X_ENC, .input = gbtn::PLAY, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_RESTART_FROM_SCREEN, .applies = applies_restartx, .keywords = "playback here selection"};
PLACE_SDRAM_RODATA constexpr Gesture RestartPlayCross{.hold = gbtn::CROSS_SCREEN_EDIT, .input = gbtn::PLAY, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_RESTART_FROM_SCREEN, .applies = applies_restartcross, .keywords = "playback here selection"};
PLACE_SDRAM_RODATA constexpr Gesture PlayFromScreen{.hold = gbtn::X_ENC, .input = gbtn::PLAY, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_PLAY_FROM_SCREEN, .applies = applies_fromscreenx, .keywords = "here selection position start"};
PLACE_SDRAM_RODATA constexpr Gesture PlayFromScreenCross{.hold = gbtn::CROSS_SCREEN_EDIT, .input = gbtn::PLAY, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_PLAY_FROM_SCREEN, .applies = applies_fromscreencross, .keywords = "here selection position start"};
PLACE_SDRAM_RODATA constexpr Gesture PlayFromStart{.hold = gbtn::X_ENC, .input = gbtn::PLAY, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_PLAY_FROM_START, .applies = applies_fromstartx, .keywords = "beginning song zero"};
PLACE_SDRAM_RODATA constexpr Gesture PlayFromStartCross{.hold = gbtn::CROSS_SCREEN_EDIT, .input = gbtn::PLAY, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_PLAY_FROM_START, .applies = applies_fromstartcross, .keywords = "beginning song zero"};
PLACE_SDRAM_RODATA constexpr Gesture PlayRecordOutput{.hold = gbtn::RECORD, .input = gbtn::PLAY, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_PLAY_AND_RESAMPLE, .keywords = "record output bounce"};
PLACE_SDRAM_RODATA constexpr Gesture RecordOutput{.hold = gbtn::SHIFT, .input = gbtn::RECORD, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_RESAMPLE, .keywords = "record output bounce"};
PLACE_SDRAM_RODATA constexpr Gesture Record{.input = gbtn::RECORD, .action = Action::TAP, .name = l10n::String::STRING_FOR_GESTURE_RECORD, .keywords = "arm overdub"};
PLACE_SDRAM_RODATA constexpr Gesture ClearTempoAutomation{.hold = gbtn::SHIFT, .input = gbtn::TEMPO_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CLEAR_TEMPO_AUTOMATION, .keywords = "bpm"};
PLACE_SDRAM_RODATA constexpr Gesture ShowSwing{.hold = gbtn::TAP_TEMPO, .input = gbtn::TEMPO_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SHOW_SWING_INTERVAL, .keywords = "groove"};
PLACE_SDRAM_RODATA constexpr Gesture ShowTempo{.input = gbtn::TEMPO_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SHOW_TEMPO, .keywords = "bpm"};
PLACE_SDRAM_RODATA constexpr Gesture SwingInterval{.hold = gbtn::TAP_TEMPO, .input = gbtn::TEMPO_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_SWING_INTERVAL, .keywords = "groove"};
PLACE_SDRAM_RODATA constexpr Gesture NudgeSync{.hold = gbtn::X_ENC, .input = gbtn::TEMPO_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_NUDGE_CLOCK_SYNC, .keywords = "phase external"};
PLACE_SDRAM_RODATA constexpr Gesture SwingAmount{.hold = gbtn::SHIFT, .input = gbtn::TEMPO_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_SWING_AMOUNT, .keywords = "groove shuffle"};
PLACE_SDRAM_RODATA constexpr Gesture ClockOutScale{.hold = gbtn::LEARN, .input = gbtn::TEMPO_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_CLOCK_OUT_SCALE, .keywords = "midi sync ppqn"};
PLACE_SDRAM_RODATA constexpr Gesture Tempo{.input = gbtn::TEMPO_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_TEMPO, .keywords = "bpm speed"};
PLACE_SDRAM_RODATA constexpr Gesture FineTempo{.hold = gbtn::TEMPO_ENC, .input = gbtn::TEMPO_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_FINE_TEMPO, .applies = applies_finetempo, .keywords = "bpm"};
PLACE_SDRAM_RODATA constexpr Gesture PadBrightness{.hold = gbtn::SHIFT, .hold2 = gbtn::LEARN, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_PAD_BRIGHTNESS, .keywords = "dim led"};
PLACE_SDRAM_RODATA constexpr Gesture PadRefresh{.hold = gbtn::CLIP_VIEW, .input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_PAD_REFRESH_RATE, .keywords = "led"};
PLACE_SDRAM_RODATA constexpr Gesture ThresholdRecording{.hold = gbtn::RECORD, .input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_THRESHOLD_RECORDING, .keywords = "level trigger"};
PLACE_SDRAM_RODATA constexpr Gesture SaveSkipback{.hold = gbtn::RECORD, .input = gbtn::SELECT_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SAVE_SKIPBACK, .applies = applies_skipback, .keywords = "capture retrospective sample midi"};
PLACE_SDRAM_RODATA constexpr Gesture SwapDisplay{.hold = gbtn::SHIFT, .hold2 = gbtn::LEARN, .input = gbtn::AFFECT_ENTIRE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SWAP_DISPLAY_TYPE, .applies = applies_emudisplay, .keywords = "oled 7seg"};
PLACE_SDRAM_RODATA constexpr Gesture ExitMenus{.input = gbtn::BACK, .action = Action::HOLD, .name = l10n::String::STRING_FOR_GESTURE_EXIT_ALL_MENUS, .keywords = "leave"};
PLACE_SDRAM_RODATA constexpr Gesture StickyShift{.input = gbtn::SHIFT, .action = Action::TAP, .name = l10n::String::STRING_FOR_GESTURE_STICKY_SHIFT, .applies = applies_sticky, .keywords = "latch"};
} // namespace global
namespace view {
PLACE_SDRAM_RODATA constexpr Gesture TapTempo{.input = gbtn::TAP_TEMPO, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_TAP_TEMPO, .keywords = "bpm"};
PLACE_SDRAM_RODATA constexpr Gesture MidiLearn{.input = gbtn::LEARN, .action = Action::HOLD, .name = l10n::String::STRING_FOR_GESTURE_MIDI_LEARN, .keywords = "controller map"};
PLACE_SDRAM_RODATA constexpr Gesture SaveSong{.input = gbtn::SAVE, .action = Action::TAP, .name = l10n::String::STRING_FOR_GESTURE_SAVE_SONG, .keywords = "project"};
PLACE_SDRAM_RODATA constexpr Gesture LoadSong{.input = gbtn::LOAD, .action = Action::TAP, .name = l10n::String::STRING_FOR_GESTURE_LOAD_SONG, .keywords = "open project"};
PLACE_SDRAM_RODATA constexpr Gesture SyncScaling{.input = gbtn::SYNC_SCALING, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SYNC_SCALING, .keywords = "length"};
PLACE_SDRAM_RODATA constexpr Gesture Undo{.input = gbtn::BACK, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_UNDO, .keywords = "revert"};
PLACE_SDRAM_RODATA constexpr Gesture KnobParam{.input = gbtn::MOD_ENCODER_0, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_EDIT_KNOB_PARAM, .keywords = "gold"};
PLACE_SDRAM_RODATA constexpr Gesture ParamLockNotes{.hold = pad::NOTE, .input = gbtn::MOD_ENCODER_0, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_PARAM_LOCK_NOTES, .keywords = "automate step note"};
PLACE_SDRAM_RODATA constexpr Gesture KnobButton{.input = gbtn::MOD_ENCODER_0, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_KNOB_BUTTON_ACTION, .keywords = "gold stutter mode"};
PLACE_SDRAM_RODATA constexpr Gesture LaunchNow{.hold = gbtn::SHIFT, .input = pad::MUTE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_LAUNCH_CLIP_NOW, .applies = applies_rows, .keywords = "immediate unquantized"};
PLACE_SDRAM_RODATA constexpr Gesture SoloClip{.hold = gbtn::X_ENC, .input = pad::MUTE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SOLO_CLIP, .applies = applies_rows};
PLACE_SDRAM_RODATA constexpr Gesture ArmClip{.hold = gbtn::RECORD, .input = pad::MUTE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_ARM_FOR_RECORDING, .applies = applies_rows, .keywords = "record"};
} // namespace view
namespace timeline {
PLACE_SDRAM_RODATA constexpr Gesture ShowZoom{.input = gbtn::X_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SHOW_ZOOM_LEVEL};
PLACE_SDRAM_RODATA constexpr Gesture Triplets{.input = gbtn::TRIPLETS, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_TRIPLETS_VIEW, .keywords = "swing 3"};
PLACE_SDRAM_RODATA constexpr Gesture Zoom{.hold = gbtn::X_ENC, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_ZOOM, .keywords = "horizontal"};
PLACE_SDRAM_RODATA constexpr Gesture Scroll{.input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_SCROLL, .applies = applies_noselection, .keywords = "horizontal"};
} // namespace timeline
namespace clip {
PLACE_SDRAM_RODATA constexpr Gesture ClipLength{.hold = gbtn::SHIFT, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_CLIP_LENGTH, .applies = applies_noselection, .keywords = "bars loop"};
PLACE_SDRAM_RODATA constexpr Gesture ShiftContents{.hold = gbtn::Y_ENC, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_SHIFT_CLIP_CONTENTS, .keywords = "move rotate notes"};
PLACE_SDRAM_RODATA constexpr Gesture ShortcutOverview{.input = gbtn::SHIFT, .action = Action::HOLD, .name = l10n::String::STRING_FOR_GESTURE_SHORTCUT_OVERVIEW, .applies = applies_overlay, .keywords = "help map"};
} // namespace clip
namespace instrumentClip {
PLACE_SDRAM_RODATA constexpr Gesture SongView{.input = gbtn::SESSION_VIEW, .action = Action::TAP, .name = l10n::String::STRING_FOR_GESTURE_SONG_VIEW, .keywords = "session arranger"};
PLACE_SDRAM_RODATA constexpr Gesture SessionMacros{.input = gbtn::SESSION_VIEW, .action = Action::HOLD, .name = l10n::String::STRING_FOR_GESTURE_SESSION_MACROS, .keywords = "sidebar"};
PLACE_SDRAM_RODATA constexpr Gesture RunMacro{.hold = gbtn::SESSION_VIEW, .input = pad::MUTE, .action = Action::TAP, .name = l10n::String::STRING_FOR_GESTURE_RUN_SESSION_MACRO, .keywords = "sidebar"};
PLACE_SDRAM_RODATA constexpr Gesture AutomationView{.input = gbtn::CLIP_VIEW, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_AUTOMATION_VIEW, .keywords = "param"};
PLACE_SDRAM_RODATA constexpr Gesture KeyboardView{.input = gbtn::KEYBOARD, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_KEYBOARD_VIEW, .keywords = "isomorphic notes"};
PLACE_SDRAM_RODATA constexpr Gesture CrossScreen{.input = gbtn::CROSS_SCREEN_EDIT, .action = Action::TAP, .name = l10n::String::STRING_FOR_GESTURE_CROSS_SCREEN_EDITING, .keywords = "wrap"};
PLACE_SDRAM_RODATA constexpr Gesture RecordToRow{.hold = pad::AUDITION, .input = gbtn::RECORD, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_RECORD_SAMPLE_TO_ROW, .applies = applies_kit, .keywords = "drum audio"};
PLACE_SDRAM_RODATA constexpr Gesture BrowseRowSample{.hold = pad::AUDITION, .input = gbtn::LOAD, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_BROWSE_ROW_SAMPLE, .applies = applies_kit, .keywords = "drum file"};
PLACE_SDRAM_RODATA constexpr Gesture BrowseOscSample{.hold = pad::AUDITION, .input = gbtn::LOAD, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_BROWSE_OSC_SAMPLE, .applies = applies_synth, .keywords = "file"};
PLACE_SDRAM_RODATA constexpr Gesture ToKit{.input = gbtn::KIT, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CHANGE_TO_KIT, .keywords = "drums type"};
PLACE_SDRAM_RODATA constexpr Gesture ToSynth{.input = gbtn::SYNTH, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CHANGE_TO_SYNTH, .keywords = "type"};
PLACE_SDRAM_RODATA constexpr Gesture ToMidi{.input = gbtn::MIDI, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CHANGE_TO_MIDI, .keywords = "type"};
PLACE_SDRAM_RODATA constexpr Gesture ToCv{.input = gbtn::CV, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CHANGE_TO_CV, .keywords = "gate type"};
PLACE_SDRAM_RODATA constexpr Gesture NewFmSynth{.hold = gbtn::MOD7, .input = gbtn::SYNTH, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_NEW_FM_SYNTH, .applies = applies_dx7, .keywords = "dx7 create"};
PLACE_SDRAM_RODATA constexpr Gesture SynthRow{.hold = pad::AUDITION, .input = gbtn::SYNTH, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SYNTH_KIT_ROW, .applies = applies_kit, .keywords = "drum preset"};
PLACE_SDRAM_RODATA constexpr Gesture MidiRow{.hold = pad::AUDITION, .input = gbtn::MIDI, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_MIDI_KIT_ROW, .applies = applies_kit, .keywords = "drum"};
PLACE_SDRAM_RODATA constexpr Gesture GateRow{.hold = pad::AUDITION, .input = gbtn::CV, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_GATE_KIT_ROW, .applies = applies_kit, .keywords = "drum cv"};
PLACE_SDRAM_RODATA constexpr Gesture SaveRow{.hold = pad::AUDITION, .input = gbtn::SAVE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SAVE_KIT_ROW, .applies = applies_kit, .keywords = "drum preset"};
PLACE_SDRAM_RODATA constexpr Gesture DeleteRow{.hold = pad::NOTE, .input = gbtn::SAVE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_DELETE_KIT_ROW, .applies = applies_kit, .keywords = "drum remove"};
PLACE_SDRAM_RODATA constexpr Gesture MoveKitRowHere{.hold = pad::AUDITION, .hold2 = pad::AUDITION, .input = gbtn::Y_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_MOVE_KIT_ROW_HERE, .applies = applies_kit, .keywords = "reorder drum"};
PLACE_SDRAM_RODATA constexpr Gesture NoteEditor{.hold = pad::NOTE, .input = gbtn::SELECT_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_NOTE_EDITOR, .keywords = "menu velocity"};
PLACE_SDRAM_RODATA constexpr Gesture RowEditor{.hold = pad::AUDITION, .input = gbtn::SELECT_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_ROW_EDITOR, .keywords = "menu note row"};
PLACE_SDRAM_RODATA constexpr Gesture ScaleMode{.input = gbtn::SCALE_MODE, .action = Action::TAP, .name = l10n::String::STRING_FOR_GESTURE_SCALE_MODE, .applies = applies_notkit, .keywords = "key"};
PLACE_SDRAM_RODATA constexpr Gesture ShowScale{.input = gbtn::SCALE_MODE, .action = Action::HOLD, .name = l10n::String::STRING_FOR_GESTURE_SHOW_SCALE, .applies = applies_notkit, .keywords = "key root"};
PLACE_SDRAM_RODATA constexpr Gesture RootNote{.hold = pad::AUDITION, .input = gbtn::SCALE_MODE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SET_ROOT_NOTE, .applies = applies_notkit, .keywords = "key scale"};
PLACE_SDRAM_RODATA constexpr Gesture RandomizeDrum{.hold = gbtn::LOAD, .input = pad::GRID, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_RANDOMIZE_DRUM_SAMPLE, .applies = applies_kitrandom, .keywords = "random kit", .padX = 15, .padY = 2};
PLACE_SDRAM_RODATA constexpr Gesture RandomizeAllDrums{.hold = gbtn::LOAD, .hold2 = gbtn::AFFECT_ENTIRE, .input = pad::GRID, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_RANDOMIZE_ALL_DRUMS, .applies = applies_kitrandom, .keywords = "random kit", .padX = 15, .padY = 2};
PLACE_SDRAM_RODATA constexpr Gesture RandomizeHeldDrums{.hold = gbtn::LOAD, .hold2 = pad::AUDITION, .input = pad::GRID, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_RANDOMIZE_HELD_ROWS, .applies = applies_kitrandom, .keywords = "random kit", .padX = 15, .padY = 2};
PLACE_SDRAM_RODATA constexpr Gesture VelocityEditor{.hold = pad::AUDITION, .input = pad::GRID, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_NOTE_VELOCITY_EDITOR, .keywords = "automation", .padX = 15, .padY = 1};
PLACE_SDRAM_RODATA constexpr Gesture DragKitRow{.hold = pad::AUDITION, .hold2 = pad::MUTE, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_MOVE_KIT_ROW, .applies = applies_kit, .keywords = "reorder drum drag"};
PLACE_SDRAM_RODATA constexpr Gesture MuteRow{.input = pad::MUTE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_MUTE_ROW, .keywords = "silence"};
PLACE_SDRAM_RODATA constexpr Gesture SilentAudition{.hold = gbtn::SHIFT, .input = pad::AUDITION, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SILENT_AUDITION, .keywords = "select row quiet"};
PLACE_SDRAM_RODATA constexpr Gesture ResampleRow{.hold = gbtn::RECORD, .input = pad::AUDITION, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_RESAMPLE_FROM_ROW, .keywords = "record output"};
PLACE_SDRAM_RODATA constexpr Gesture AddKitRow{.input = pad::AUDITION, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_ADD_KIT_ROW, .applies = applies_kit, .keywords = "empty drum new"};
PLACE_SDRAM_RODATA constexpr Gesture NoteLength{.hold = pad::NOTE, .input = pad::GRID, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_NOTE_LENGTH, .keywords = "tie sustain"};
PLACE_SDRAM_RODATA constexpr Gesture RowPitch{.hold = pad::AUDITION, .hold2 = gbtn::SELECT_ENC, .input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_ROW_NOTE_PITCH, .keywords = "drum note"};
PLACE_SDRAM_RODATA constexpr Gesture RowProbability{.hold = pad::AUDITION, .input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_ROW_PROBABILITY, .keywords = "iterance chance"};
PLACE_SDRAM_RODATA constexpr Gesture NoteProbability{.hold = pad::NOTE, .input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_NOTE_PROBABILITY, .keywords = "iterance chance"};
PLACE_SDRAM_RODATA constexpr Gesture BrowsePresets{.input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_BROWSE_PRESETS, .applies = applies_noselection, .keywords = "sound patch instrument"};
PLACE_SDRAM_RODATA constexpr Gesture KnobMidiCc{.hold = gbtn::MOD_ENCODER_0, .input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_KNOB_MIDI_CC, .applies = applies_midi, .keywords = "gold controller"};
PLACE_SDRAM_RODATA constexpr Gesture MoveCcAutomation{.hold = gbtn::MOD_ENCODER_0, .hold2 = gbtn::SELECT_ENC, .input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_MOVE_CC_AUTOMATION, .applies = applies_midi, .keywords = "gold controller"};
PLACE_SDRAM_RODATA constexpr Gesture TransposeScreen{.hold = gbtn::X_ENC, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_TRANSPOSE_SCREEN, .applies = applies_notkit, .keywords = "scale step"};
PLACE_SDRAM_RODATA constexpr Gesture TransposeInOctave{.hold = gbtn::X_ENC, .hold2 = gbtn::Y_ENC, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_TRANSPOSE_IN_OCTAVE, .applies = applies_notkit, .keywords = "wrap"};
PLACE_SDRAM_RODATA constexpr Gesture Euclidean{.hold = pad::AUDITION, .hold2 = gbtn::Y_ENC, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_EUCLIDEAN_RHYTHM, .keywords = "events steps"};
PLACE_SDRAM_RODATA constexpr Gesture NoteRepeat{.hold = pad::NOTE, .hold2 = gbtn::Y_ENC, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_NOTE_REPEAT, .keywords = "ratchet roll"};
PLACE_SDRAM_RODATA constexpr Gesture TransposeOctave{.hold = gbtn::Y_ENC, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_TRANSPOSE_OCTAVE, .applies = applies_notkit, .keywords = "pitch"};
PLACE_SDRAM_RODATA constexpr Gesture TransposeClip{.hold = gbtn::SHIFT, .hold2 = gbtn::Y_ENC, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_TRANSPOSE_CLIP, .applies = applies_notkit, .keywords = "semitone pitch key"};
PLACE_SDRAM_RODATA constexpr Gesture NudgeSemitone{.hold = pad::NOTE, .hold2 = gbtn::SHIFT, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_MOVE_NOTES_BY_SEMITONE, .applies = applies_notkit, .keywords = "accidental pitch"};
PLACE_SDRAM_RODATA constexpr Gesture ClipColour{.hold = gbtn::SHIFT, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_CLIP_COLOUR, .applies = applies_noselection, .keywords = "color hue"};
PLACE_SDRAM_RODATA constexpr Gesture RowColour{.hold = pad::AUDITION, .hold2 = gbtn::SHIFT, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_ROW_COLOUR, .applies = applies_kit, .keywords = "color hue"};
PLACE_SDRAM_RODATA constexpr Gesture MoveNotes{.hold = pad::NOTE, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_MOVE_NOTES_UP_DOWN, .keywords = "pitch"};
PLACE_SDRAM_RODATA constexpr Gesture NoteVelocity{.hold = pad::NOTE, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_NOTE_VELOCITY, .keywords = "loudness"};
PLACE_SDRAM_RODATA constexpr Gesture NudgeNotes{.hold = pad::NOTE, .hold2 = gbtn::X_ENC, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_NUDGE_NOTES, .keywords = "timing shift"};
PLACE_SDRAM_RODATA constexpr Gesture RowLength{.hold = pad::AUDITION, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_ROW_LENGTH, .keywords = "polymeter loop"};
PLACE_SDRAM_RODATA constexpr Gesture RotateRow{.hold = pad::AUDITION, .hold2 = gbtn::X_ENC, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_ROTATE_ROW, .keywords = "shift"};
PLACE_SDRAM_RODATA constexpr Gesture Quantize{.hold = pad::AUDITION, .input = gbtn::TEMPO_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_QUANTIZE_HUMANIZE, .applies = applies_quantize, .keywords = "timing"};
PLACE_SDRAM_RODATA constexpr Gesture DrumParam{.hold = pad::AUDITION, .input = gbtn::MOD_ENCODER_0, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_MIDI_GATE_DRUM_PARAM, .applies = applies_kit, .keywords = "knob"};
PLACE_SDRAM_RODATA constexpr Gesture ClearClip{.hold = gbtn::X_ENC, .input = gbtn::BACK, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CLEAR_CLIP, .keywords = "delete notes erase"};
PLACE_SDRAM_RODATA constexpr Gesture SoundMenu{.input = gbtn::SELECT_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SOUND_MENU, .keywords = "editor settings"};
PLACE_SDRAM_RODATA constexpr Gesture AffectEntire{.input = gbtn::AFFECT_ENTIRE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_AFFECT_ENTIRE, .applies = applies_kit, .keywords = "whole kit"};
PLACE_SDRAM_RODATA constexpr Gesture SelectNotes{.hold = gbtn::LEARN, .input = pad::GRID, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SELECT_NOTES, .keywords = "range region area two pads"};
PLACE_SDRAM_RODATA constexpr Gesture SelectRows{.hold = gbtn::LEARN, .input = pad::AUDITION, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_NARROW_SELECTED_ROWS, .keywords = "range selection"};
PLACE_SDRAM_RODATA constexpr Gesture MoveSelection{.input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_MOVE_SELECTION, .applies = applies_selection, .keywords = "notes range shift"};
PLACE_SDRAM_RODATA constexpr Gesture DuplicateSelection{.hold = gbtn::SHIFT, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_DUPLICATE_SELECTION, .applies = applies_selection, .keywords = "notes range copy"};
PLACE_SDRAM_RODATA constexpr Gesture TransposeSelection{.input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_TRANSPOSE_SELECTION, .applies = applies_selection, .keywords = "notes range scale step"};
PLACE_SDRAM_RODATA constexpr Gesture SelectionSemitone{.hold = gbtn::SHIFT, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_SELECTION_BY_SEMITONE, .applies = applies_selection, .keywords = "transpose notes range"};
PLACE_SDRAM_RODATA constexpr Gesture SelectionOctave{.hold = gbtn::Y_ENC, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_SELECTION_BY_OCTAVE, .applies = applies_selection, .keywords = "transpose notes range"};
PLACE_SDRAM_RODATA constexpr Gesture SelectionVelocity{.input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_SELECTION_VELOCITY, .applies = applies_selection, .keywords = "notes range loudness"};
PLACE_SDRAM_RODATA constexpr Gesture DeleteSelection{.hold = gbtn::SHIFT, .input = gbtn::SAVE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_DELETE_SELECTED_NOTES, .applies = applies_selection, .keywords = "range erase"};
PLACE_SDRAM_RODATA constexpr Gesture EndSelection{.input = gbtn::LEARN, .action = Action::TAP, .name = l10n::String::STRING_FOR_GESTURE_END_SELECTION, .applies = applies_selection, .keywords = "deselect range"};
} // namespace instrumentClip
namespace automation {
PLACE_SDRAM_RODATA constexpr Gesture Overview{.hold = gbtn::SHIFT, .input = gbtn::CLIP_VIEW, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_AUTOMATION_OVERVIEW, .keywords = "params"};
PLACE_SDRAM_RODATA constexpr Gesture SongView{.input = gbtn::SESSION_VIEW, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SONG_VIEW, .keywords = "session arranger"};
PLACE_SDRAM_RODATA constexpr Gesture KeyboardView{.input = gbtn::KEYBOARD, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_KEYBOARD_VIEW};
PLACE_SDRAM_RODATA constexpr Gesture ClipView{.input = gbtn::CLIP_VIEW, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_BACK_TO_CLIP_VIEW, .keywords = "exit"};
PLACE_SDRAM_RODATA constexpr Gesture LoopView{.hold = gbtn::SHIFT, .input = gbtn::CROSS_SCREEN_EDIT, .action = Action::TAP, .name = l10n::String::STRING_FOR_GESTURE_LOOP_VIEW, .applies = applies_instrument, .keywords = "play window bar"};
PLACE_SDRAM_RODATA constexpr Gesture CopyAutomation{.hold = gbtn::LEARN, .input = gbtn::X_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_COPY_AUTOMATION, .keywords = "param"};
PLACE_SDRAM_RODATA constexpr Gesture PasteAutomation{.hold = gbtn::LEARN, .hold2 = gbtn::SHIFT, .input = gbtn::X_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_PASTE_AUTOMATION, .keywords = "param"};
PLACE_SDRAM_RODATA constexpr Gesture DoubleClip{.hold = gbtn::SHIFT, .input = gbtn::X_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_DOUBLE_CLIP, .applies = applies_instrument, .keywords = "multiply length"};
PLACE_SDRAM_RODATA constexpr Gesture ClearAutomation{.hold = gbtn::X_ENC, .input = gbtn::BACK, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CLEAR_AUTOMATION, .keywords = "delete param"};
PLACE_SDRAM_RODATA constexpr Gesture SoundMenu{.input = gbtn::SELECT_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SOUND_MENU, .keywords = "editor"};
PLACE_SDRAM_RODATA constexpr Gesture FoldVelocity{.input = gbtn::AFFECT_ENTIRE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_FOLD_VELOCITY_EDITOR, .keywords = "note"};
PLACE_SDRAM_RODATA constexpr Gesture Interpolation{.hold = gbtn::SHIFT, .input = pad::GRID, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_INTERPOLATION, .keywords = "smooth ramp", .padX = 0, .padY = 6};
PLACE_SDRAM_RODATA constexpr Gesture PadSelection{.hold = gbtn::SHIFT, .input = pad::GRID, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_PAD_SELECTION_MODE, .keywords = "step", .padX = 0, .padY = 7};
PLACE_SDRAM_RODATA constexpr Gesture VelocityEditor{.hold = pad::AUDITION, .input = pad::GRID, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_NOTE_VELOCITY_EDITOR, .applies = applies_instrument, .padX = 15, .padY = 1};
PLACE_SDRAM_RODATA constexpr Gesture PickParam{.hold = gbtn::SHIFT, .input = pad::GRID, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_PICK_PARAM_TO_AUTOMATE, .keywords = "shortcut"};
PLACE_SDRAM_RODATA constexpr Gesture SilentAudition{.hold = gbtn::SHIFT, .input = pad::AUDITION, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SILENT_AUDITION};
PLACE_SDRAM_RODATA constexpr Gesture ShiftAutomation{.hold = gbtn::Y_ENC, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_SHIFT_AUTOMATION, .keywords = "move rotate"};
PLACE_SDRAM_RODATA constexpr Gesture RowLength{.hold = pad::AUDITION, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_ROW_LENGTH};
PLACE_SDRAM_RODATA constexpr Gesture NoteVelocity{.hold = pad::NOTE, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_NOTE_VELOCITY};
PLACE_SDRAM_RODATA constexpr Gesture TransposeOctave{.hold = gbtn::Y_ENC, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_TRANSPOSE_OCTAVE, .applies = applies_instrument, .keywords = "pitch"};
PLACE_SDRAM_RODATA constexpr Gesture ClipColour{.hold = gbtn::SHIFT, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_CLIP_COLOUR, .keywords = "color"};
PLACE_SDRAM_RODATA constexpr Gesture StepValue{.hold = pad::GRID, .input = gbtn::MOD_ENCODER_0, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_SET_STEP_VALUE, .keywords = "automation gold"};
PLACE_SDRAM_RODATA constexpr Gesture ParamValue{.input = gbtn::MOD_ENCODER_0, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_SET_VALUE_RECORD, .keywords = "automation gold"};
PLACE_SDRAM_RODATA constexpr Gesture CopyKnobAutomation{.hold = gbtn::LEARN, .input = gbtn::MOD_ENCODER_0, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_COPY_KNOB_AUTOMATION, .keywords = "gold"};
PLACE_SDRAM_RODATA constexpr Gesture PasteKnobAutomation{.hold = gbtn::LEARN, .hold2 = gbtn::SHIFT, .input = gbtn::MOD_ENCODER_0, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_PASTE_KNOB_AUTOMATION, .keywords = "gold"};
PLACE_SDRAM_RODATA constexpr Gesture DeleteKnobAutomation{.hold = gbtn::SHIFT, .input = gbtn::MOD_ENCODER_0, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_DELETE_KNOB_AUTOMATION, .keywords = "gold clear"};
PLACE_SDRAM_RODATA constexpr Gesture FastParamSelect{.hold = gbtn::SHIFT, .input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_FAST_PARAM_SELECT};
PLACE_SDRAM_RODATA constexpr Gesture SelectParam{.input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_SELECT_PARAM, .keywords = "automation"};
} // namespace automation
namespace keyboard {
PLACE_SDRAM_RODATA constexpr Gesture SelectScale{.hold = gbtn::SHIFT, .input = gbtn::SCALE_MODE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SELECT_SCALE, .applies = applies_notkit, .keywords = "key"};
PLACE_SDRAM_RODATA constexpr Gesture ScaleMode{.input = gbtn::SCALE_MODE, .action = Action::TAP, .name = l10n::String::STRING_FOR_GESTURE_SCALE_MODE, .applies = applies_notkit, .keywords = "key"};
PLACE_SDRAM_RODATA constexpr Gesture RootNote{.hold = pad::NOTE, .input = gbtn::SCALE_MODE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SET_ROOT_NOTE, .applies = applies_notkit, .keywords = "key"};
PLACE_SDRAM_RODATA constexpr Gesture CycleScales{.hold = gbtn::SCALE_MODE, .input = gbtn::SELECT_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CYCLE_SCALES, .applies = applies_notkit, .keywords = "key"};
PLACE_SDRAM_RODATA constexpr Gesture RootNoteTurn{.hold = gbtn::SCALE_MODE, .input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_CHANGE_ROOT_NOTE, .applies = applies_notkit, .keywords = "key"};
PLACE_SDRAM_RODATA constexpr Gesture BackToClip{.input = gbtn::KEYBOARD, .action = Action::TAP, .name = l10n::String::STRING_FOR_GESTURE_BACK_TO_CLIP, .keywords = "exit"};
PLACE_SDRAM_RODATA constexpr Gesture Layout{.hold = gbtn::KEYBOARD, .input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_KEYBOARD_LAYOUT, .keywords = "isomorphic piano chord"};
PLACE_SDRAM_RODATA constexpr Gesture SongView{.input = gbtn::SESSION_VIEW, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SONG_VIEW, .keywords = "session arranger"};
PLACE_SDRAM_RODATA constexpr Gesture ReturnTarget{.input = gbtn::CLIP_VIEW, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_RETURN_TO_AUTOMATION, .keywords = "clip view"};
PLACE_SDRAM_RODATA constexpr Gesture ToKit{.input = gbtn::KIT, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CHANGE_TO_KIT, .keywords = "type"};
PLACE_SDRAM_RODATA constexpr Gesture ToSynth{.input = gbtn::SYNTH, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CHANGE_TO_SYNTH, .keywords = "type"};
PLACE_SDRAM_RODATA constexpr Gesture ToMidi{.input = gbtn::MIDI, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CHANGE_TO_MIDI, .keywords = "type"};
PLACE_SDRAM_RODATA constexpr Gesture ToCv{.input = gbtn::CV, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CHANGE_TO_CV, .keywords = "type"};
PLACE_SDRAM_RODATA constexpr Gesture NewSynth{.hold = gbtn::SHIFT, .input = gbtn::SYNTH, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_NEW_SYNTH, .keywords = "create"};
PLACE_SDRAM_RODATA constexpr Gesture NewKit{.hold = gbtn::SHIFT, .input = gbtn::KIT, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_NEW_KIT, .keywords = "create drums"};
PLACE_SDRAM_RODATA constexpr Gesture BrowseDrumSample{.hold = pad::NOTE, .input = gbtn::LOAD, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_BROWSE_DRUM_SAMPLE, .applies = applies_kit, .keywords = "file"};
PLACE_SDRAM_RODATA constexpr Gesture Resample{.hold = gbtn::RECORD, .input = pad::NOTE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_RESAMPLE, .keywords = "record output"};
PLACE_SDRAM_RODATA constexpr Gesture Colours{.hold = gbtn::SHIFT, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_PAD_COLOURS, .keywords = "color hue"};
PLACE_SDRAM_RODATA constexpr Gesture RowInterval{.hold = gbtn::SHIFT, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_ROW_INTERVAL, .applies = applies_isolayout, .keywords = "step isomorphic in key"};
PLACE_SDRAM_RODATA constexpr Gesture ChordMode{.hold = gbtn::SHIFT, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_CHORD_ROWS_COLUMNS, .applies = applies_chordkb, .keywords = "mode layout"};
PLACE_SDRAM_RODATA constexpr Gesture DrumZoom{.hold = gbtn::SHIFT, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_ZOOM_DRUM_PADS, .applies = applies_drumkb, .keywords = "size velocity"};
PLACE_SDRAM_RODATA constexpr Gesture DrumZoomX{.hold = gbtn::X_ENC, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_ZOOM_DRUM_PADS, .applies = applies_drumkb, .keywords = "size velocity"};
PLACE_SDRAM_RODATA constexpr Gesture ChordVoicing{.hold = gbtn::X_ENC, .hold2 = pad::NOTE, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_CHORD_VOICING, .applies = applies_chordlib, .keywords = "library"};
PLACE_SDRAM_RODATA constexpr Gesture ColumnFunction{.hold = pad::GRID, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_SIDEBAR_FUNCTION, .applies = applies_columns, .keywords = "controls"};
PLACE_SDRAM_RODATA constexpr Gesture ColumnSetting{.hold = pad::GRID, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_SIDEBAR_COLUMN_SETTING, .applies = applies_columns, .keywords = "velocity mod min max scale"};
PLACE_SDRAM_RODATA constexpr Gesture ChordMemory{.input = pad::GRID, .action = Action::TAP, .name = l10n::String::STRING_FOR_GESTURE_CHORD_MEMORY, .applies = applies_columns, .keywords = "sidebar column shift overwrite"};
} // namespace keyboard
namespace audioClip {
PLACE_SDRAM_RODATA constexpr Gesture SongView{.input = gbtn::SESSION_VIEW, .action = Action::TAP, .name = l10n::String::STRING_FOR_GESTURE_SONG_VIEW, .keywords = "session arranger"};
PLACE_SDRAM_RODATA constexpr Gesture RunMacro{.hold = gbtn::SESSION_VIEW, .input = pad::MUTE, .action = Action::TAP, .name = l10n::String::STRING_FOR_GESTURE_RUN_SESSION_MACRO, .keywords = "sidebar"};
PLACE_SDRAM_RODATA constexpr Gesture AutomationView{.input = gbtn::CLIP_VIEW, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_AUTOMATION_VIEW, .keywords = "param"};
PLACE_SDRAM_RODATA constexpr Gesture LengthToSample{.hold = gbtn::Y_ENC, .input = gbtn::X_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_LENGTH_TO_SAMPLE, .keywords = "remove timestretch"};
PLACE_SDRAM_RODATA constexpr Gesture SoundMenu{.input = gbtn::SELECT_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SOUND_MENU, .keywords = "editor"};
PLACE_SDRAM_RODATA constexpr Gesture ClearClip{.hold = gbtn::X_ENC, .input = gbtn::BACK, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CLEAR_CLIP, .keywords = "delete sample"};
PLACE_SDRAM_RODATA constexpr Gesture GrabTempo{.hold = gbtn::TEMPO_ENC, .input = pad::GRID, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_TEMPO_FROM_CLIP, .keywords = "grab bpm"};
PLACE_SDRAM_RODATA constexpr Gesture EndPoint{.input = pad::GRID, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SET_END_POINT, .keywords = "marker trim"};
PLACE_SDRAM_RODATA constexpr Gesture TrimStart{.input = pad::GRID, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_TRIM_START, .applies = applies_trimstart, .keywords = "marker"};
PLACE_SDRAM_RODATA constexpr Gesture MoveToTrack{.hold = gbtn::SHIFT, .input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_MOVE_TO_AUDIO_TRACK};
PLACE_SDRAM_RODATA constexpr Gesture OutputMode{.input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_AUDIO_OUTPUT_MODE, .keywords = "monitor"};
PLACE_SDRAM_RODATA constexpr Gesture LengthNoStretch{.hold = gbtn::SHIFT, .hold2 = gbtn::X_ENC, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_LENGTH_NO_TIMESTRETCH, .keywords = "crop"};
PLACE_SDRAM_RODATA constexpr Gesture ClipColour{.hold = gbtn::SHIFT, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_CLIP_COLOUR, .keywords = "color"};
} // namespace audioClip
namespace session {
PLACE_SDRAM_RODATA constexpr Gesture EnterClip{.input = gbtn::CLIP_VIEW, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_ENTER_CLIP, .keywords = "edit"};
PLACE_SDRAM_RODATA constexpr Gesture RecordArrangement{.hold = gbtn::RECORD, .input = gbtn::SESSION_VIEW, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_RECORD_ARRANGEMENT, .keywords = "arranger"};
PLACE_SDRAM_RODATA constexpr Gesture ClipToArranger{.hold = pad::CLIP, .input = gbtn::SESSION_VIEW, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_COPY_CLIP_TO_ARRANGER, .applies = applies_rows};
PLACE_SDRAM_RODATA constexpr Gesture ArrangerView{.input = gbtn::SESSION_VIEW, .action = Action::TAP, .name = l10n::String::STRING_FOR_GESTURE_ARRANGER_VIEW, .keywords = "timeline"};
PLACE_SDRAM_RODATA constexpr Gesture AffectEntire{.input = gbtn::AFFECT_ENTIRE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_AFFECT_ENTIRE, .keywords = "song knobs"};
PLACE_SDRAM_RODATA constexpr Gesture RecordArming{.input = gbtn::RECORD, .action = Action::HOLD, .name = l10n::String::STRING_FOR_GESTURE_RECORD_ARMING, .keywords = "arm"};
PLACE_SDRAM_RODATA constexpr Gesture ExportStems{.hold = gbtn::SAVE, .input = gbtn::RECORD, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_EXPORT_CLIP_STEMS, .keywords = "bounce render"};
PLACE_SDRAM_RODATA constexpr Gesture DeleteClip{.hold = pad::CLIP, .input = gbtn::SAVE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_DELETE_CLIP, .keywords = "remove"};
PLACE_SDRAM_RODATA constexpr Gesture SectionRepeats{.hold = pad::SECTION, .input = gbtn::SELECT_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SECTION_REPEATS, .keywords = "loop count infinite"};
PLACE_SDRAM_RODATA constexpr Gesture ClipSettings{.hold = pad::CLIP, .input = gbtn::SELECT_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CLIP_SETTINGS, .applies = applies_grid, .keywords = "menu"};
PLACE_SDRAM_RODATA constexpr Gesture ClipSettingsRow{.hold = pad::MUTE, .input = gbtn::SELECT_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CLIP_SETTINGS, .applies = applies_rows, .keywords = "menu"};
PLACE_SDRAM_RODATA constexpr Gesture ClipToAudio{.hold = pad::CLIP, .input = gbtn::SELECT_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_EMPTY_CLIP_TO_AUDIO, .applies = applies_rows, .keywords = "convert audio track"};
PLACE_SDRAM_RODATA constexpr Gesture SongFx{.input = gbtn::SELECT_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SONG_FX_MENU, .keywords = "master global"};
PLACE_SDRAM_RODATA constexpr Gesture ClipType{.hold = pad::CLIP, .input = gbtn::SYNTH, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CHANGE_CLIP_TYPE, .keywords = "kit midi cv"};
PLACE_SDRAM_RODATA constexpr Gesture ClipPreset{.hold = pad::CLIP, .hold2 = gbtn::LOAD, .input = gbtn::SYNTH, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_LOAD_PRESET_INTO_CLIP, .keywords = "kit"};
PLACE_SDRAM_RODATA constexpr Gesture PerformanceView{.input = gbtn::KEYBOARD, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_PERFORMANCE_VIEW, .keywords = "fx"};
PLACE_SDRAM_RODATA constexpr Gesture ShowScale{.input = gbtn::Y_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SHOW_SCALE, .keywords = "key root"};
PLACE_SDRAM_RODATA constexpr Gesture Overdub{.hold = gbtn::RECORD, .input = pad::EMPTY, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_OVERDUB_CLIP, .applies = applies_rows, .keywords = "record loop row below"};
PLACE_SDRAM_RODATA constexpr Gesture GrabTempo{.hold = gbtn::TEMPO_ENC, .input = pad::CLIP, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_TEMPO_FROM_CLIP, .applies = applies_rows, .keywords = "bpm"};
PLACE_SDRAM_RODATA constexpr Gesture NewClip{.input = pad::EMPTY, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_NEW_CLIP, .keywords = "create"};
PLACE_SDRAM_RODATA constexpr Gesture CloneClip{.hold = pad::CLIP, .input = pad::EMPTY, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_COPY_CLIP, .keywords = "clone duplicate"};
PLACE_SDRAM_RODATA constexpr Gesture LaunchSectionNow{.hold = gbtn::SHIFT, .input = pad::SECTION, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_LAUNCH_SECTION_NOW, .applies = applies_grid, .keywords = "immediate"};
PLACE_SDRAM_RODATA constexpr Gesture ArmSection{.input = pad::SECTION, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_ARM_SECTION, .keywords = "launch"};
PLACE_SDRAM_RODATA constexpr Gesture ChangeSection{.hold = gbtn::SHIFT, .input = pad::SECTION, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CHANGE_CLIP_SECTION, .applies = applies_rows, .keywords = "group"};
PLACE_SDRAM_RODATA constexpr Gesture SectionRepeatsTurn{.hold = pad::SECTION, .input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_CHANGE_SECTION_REPEATS};
PLACE_SDRAM_RODATA constexpr Gesture ClipPresetTurn{.hold = pad::CLIP, .input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_BROWSE_CLIP_PRESET, .keywords = "sound"};
PLACE_SDRAM_RODATA constexpr Gesture Layout{.hold = gbtn::SESSION_VIEW, .input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_ROWS_GRID_LAYOUT, .keywords = "view"};
PLACE_SDRAM_RODATA constexpr Gesture ArmNextSection{.input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_ARM_NEXT_SECTION, .keywords = "launch"};
PLACE_SDRAM_RODATA constexpr Gesture TransposeSong{.hold = gbtn::Y_ENC, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_TRANSPOSE_SONG, .keywords = "pitch key"};
PLACE_SDRAM_RODATA constexpr Gesture MoveSection{.hold = pad::SECTION, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_MOVE_SECTION, .applies = applies_grid, .keywords = "reorder"};
PLACE_SDRAM_RODATA constexpr Gesture ClipColour{.hold = pad::CLIP, .hold2 = gbtn::SHIFT, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_CLIP_COLOUR, .applies = applies_rows, .keywords = "color"};
PLACE_SDRAM_RODATA constexpr Gesture TrackColour{.hold = pad::CLIP, .hold2 = gbtn::Y_ENC, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_TRACK_COLOUR, .applies = applies_grid, .keywords = "color hue column"};
PLACE_SDRAM_RODATA constexpr Gesture DragClip{.hold = pad::CLIP, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_DRAG_CLIP, .keywords = "move"};
PLACE_SDRAM_RODATA constexpr Gesture InsertEmptyPad{.hold = pad::CLIP, .hold2 = gbtn::SHIFT, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_INSERT_EMPTY_PAD, .applies = applies_grid, .keywords = "insert gap push down make room"};
PLACE_SDRAM_RODATA constexpr Gesture ReorderTrack{.hold = pad::CLIP, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_REORDER_TRACK, .applies = applies_grid, .keywords = "move"};
PLACE_SDRAM_RODATA constexpr Gesture SoloClip{.hold = gbtn::X_ENC, .input = pad::CLIP, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SOLO_CLIP, .applies = applies_grid};
PLACE_SDRAM_RODATA constexpr Gesture LaunchClipNow{.hold = gbtn::SHIFT, .input = pad::CLIP, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_LAUNCH_CLIP_NOW, .applies = applies_grid, .keywords = "immediate"};
PLACE_SDRAM_RODATA constexpr Gesture ArmClipGrid{.hold = gbtn::RECORD, .input = pad::CLIP, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_ARM_FOR_RECORDING, .applies = applies_grid, .keywords = "record"};
} // namespace session
namespace arranger {
PLACE_SDRAM_RODATA constexpr Gesture SongView{.input = gbtn::SESSION_VIEW, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SONG_VIEW, .keywords = "session"};
PLACE_SDRAM_RODATA constexpr Gesture ClipToSession{.hold = pad::CLIP, .input = gbtn::SESSION_VIEW, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_MOVE_CLIP_TO_SESSION};
PLACE_SDRAM_RODATA constexpr Gesture ArrangerAutomation{.input = gbtn::CLIP_VIEW, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_ARRANGER_AUTOMATION, .keywords = "param"};
PLACE_SDRAM_RODATA constexpr Gesture AffectEntire{.input = gbtn::AFFECT_ENTIRE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_AFFECT_ENTIRE, .keywords = "song knobs"};
PLACE_SDRAM_RODATA constexpr Gesture FollowPlayhead{.input = gbtn::CROSS_SCREEN_EDIT, .action = Action::TAP, .name = l10n::String::STRING_FOR_GESTURE_FOLLOW_PLAYHEAD, .keywords = "auto scroll"};
PLACE_SDRAM_RODATA constexpr Gesture ExportMixdown{.hold = gbtn::SAVE, .input = gbtn::RECORD, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_EXPORT_MIXDOWN_STEMS, .keywords = "bounce render"};
PLACE_SDRAM_RODATA constexpr Gesture RecordArming{.input = gbtn::RECORD, .action = Action::HOLD, .name = l10n::String::STRING_FOR_GESTURE_RECORD_ARMING, .keywords = "arm"};
PLACE_SDRAM_RODATA constexpr Gesture DeleteTrack{.hold = pad::AUDITION, .input = gbtn::SAVE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_DELETE_TRACK, .keywords = "remove"};
PLACE_SDRAM_RODATA constexpr Gesture TrackToAudio{.hold = pad::AUDITION, .input = gbtn::SELECT_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CONVERT_TRACK_TO_AUDIO};
PLACE_SDRAM_RODATA constexpr Gesture SongFx{.input = gbtn::SELECT_ENC, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SONG_FX_MENU, .keywords = "master global"};
PLACE_SDRAM_RODATA constexpr Gesture TrackType{.hold = pad::AUDITION, .input = gbtn::SYNTH, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CHANGE_TRACK_TYPE, .keywords = "kit midi cv"};
PLACE_SDRAM_RODATA constexpr Gesture TrackPreset{.hold = pad::AUDITION, .hold2 = gbtn::LOAD, .input = gbtn::SYNTH, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_LOAD_PRESET_INTO_TRACK, .keywords = "kit"};
PLACE_SDRAM_RODATA constexpr Gesture ClearArrangement{.hold = gbtn::X_ENC, .input = gbtn::BACK, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CLEAR_ARRANGEMENT, .keywords = "delete"};
PLACE_SDRAM_RODATA constexpr Gesture PerformanceView{.input = gbtn::KEYBOARD, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_PERFORMANCE_VIEW, .keywords = "fx"};
PLACE_SDRAM_RODATA constexpr Gesture RenameTrack{.hold = pad::AUDITION, .input = pad::GRID, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_RENAME_TRACK, .keywords = "name", .padX = 11, .padY = 5};
PLACE_SDRAM_RODATA constexpr Gesture SoloTrack{.hold = gbtn::X_ENC, .input = pad::MUTE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SOLO_TRACK};
PLACE_SDRAM_RODATA constexpr Gesture MuteTrack{.input = pad::MUTE, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_MUTE_TRACK};
PLACE_SDRAM_RODATA constexpr Gesture MakeUnique{.hold = gbtn::SHIFT, .input = pad::CLIP, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_MAKE_CLIP_UNIQUE, .keywords = "white clone instance"};
PLACE_SDRAM_RODATA constexpr Gesture MakeSection{.hold = gbtn::SHIFT, .input = pad::CLIP, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_MAKE_CLIP_SECTION, .keywords = "white unique colour session"};
PLACE_SDRAM_RODATA constexpr Gesture InstanceLength{.hold = pad::CLIP, .input = pad::GRID, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_CLIP_INSTANCE_LENGTH, .keywords = "stretch shorten start trim"};
PLACE_SDRAM_RODATA constexpr Gesture CopyInstance{.hold = pad::CLIP, .hold2 = gbtn::SHIFT, .input = pad::GRID, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_COPY_CLIP_INSTANCE, .keywords = "duplicate clone"};
PLACE_SDRAM_RODATA constexpr Gesture InstanceClip{.hold = pad::CLIP, .input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_CHANGE_INSTANCE_CLIP};
PLACE_SDRAM_RODATA constexpr Gesture DragInstance{.hold = pad::CLIP, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_DRAG_CLIP_INSTANCE, .keywords = "move position time"};
PLACE_SDRAM_RODATA constexpr Gesture TrackPresetTurn{.hold = pad::AUDITION, .input = gbtn::SELECT_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_BROWSE_TRACK_PRESET, .keywords = "sound"};
PLACE_SDRAM_RODATA constexpr Gesture InsertTime{.hold = gbtn::SHIFT, .input = gbtn::X_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_INSERT_DELETE_TIME, .keywords = "space"};
PLACE_SDRAM_RODATA constexpr Gesture TransposeSong{.hold = gbtn::Y_ENC, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_TRANSPOSE_SONG, .keywords = "pitch key"};
PLACE_SDRAM_RODATA constexpr Gesture MoveTrack{.hold = pad::AUDITION, .input = gbtn::Y_ENC, .action = Action::TURN, .name = l10n::String::STRING_FOR_GESTURE_MOVE_TRACK, .keywords = "reorder"};
PLACE_SDRAM_RODATA constexpr Gesture PlayFromPad{.hold = pad::CLIP, .input = gbtn::PLAY, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_PLAY_FROM_PAD, .keywords = "selection position start here"};
PLACE_SDRAM_RODATA constexpr Gesture SwitchToArrangement{.hold = gbtn::X_ENC, .input = gbtn::PLAY, .action = Action::PRESS, .name = l10n::String::STRING_FOR_GESTURE_SWITCH_TO_ARRANGEMENT, .keywords = "session playing"};
} // namespace arranger
// clang-format on

// Every input has a command search name
constexpr Gesture const* kAll[] = {
    &global::Play,
    &global::RestartPlay,
    &global::RestartPlayCross,
    &global::PlayFromScreen,
    &global::PlayFromScreenCross,
    &global::PlayFromStart,
    &global::PlayFromStartCross,
    &global::PlayRecordOutput,
    &global::RecordOutput,
    &global::Record,
    &global::ClearTempoAutomation,
    &global::ShowSwing,
    &global::ShowTempo,
    &global::SwingInterval,
    &global::NudgeSync,
    &global::SwingAmount,
    &global::ClockOutScale,
    &global::Tempo,
    &global::FineTempo,
    &global::PadBrightness,
    &global::PadRefresh,
    &global::ThresholdRecording,
    &global::SaveSkipback,
    &global::SwapDisplay,
    &global::ExitMenus,
    &global::StickyShift,
    &view::TapTempo,
    &view::MidiLearn,
    &view::SaveSong,
    &view::LoadSong,
    &view::SyncScaling,
    &view::Undo,
    &view::KnobParam,
    &view::ParamLockNotes,
    &view::KnobButton,
    &view::LaunchNow,
    &view::SoloClip,
    &view::ArmClip,
    &timeline::ShowZoom,
    &timeline::Triplets,
    &timeline::Zoom,
    &timeline::Scroll,
    &clip::ClipLength,
    &clip::ShiftContents,
    &clip::ShortcutOverview,
    &instrumentClip::SongView,
    &instrumentClip::SessionMacros,
    &instrumentClip::RunMacro,
    &instrumentClip::AutomationView,
    &instrumentClip::KeyboardView,
    &instrumentClip::CrossScreen,
    &instrumentClip::RecordToRow,
    &instrumentClip::BrowseRowSample,
    &instrumentClip::BrowseOscSample,
    &instrumentClip::ToKit,
    &instrumentClip::ToSynth,
    &instrumentClip::ToMidi,
    &instrumentClip::ToCv,
    &instrumentClip::NewFmSynth,
    &instrumentClip::SynthRow,
    &instrumentClip::MidiRow,
    &instrumentClip::GateRow,
    &instrumentClip::SaveRow,
    &instrumentClip::DeleteRow,
    &instrumentClip::MoveKitRowHere,
    &instrumentClip::NoteEditor,
    &instrumentClip::RowEditor,
    &instrumentClip::ScaleMode,
    &instrumentClip::ShowScale,
    &instrumentClip::RootNote,
    &instrumentClip::RandomizeDrum,
    &instrumentClip::RandomizeAllDrums,
    &instrumentClip::RandomizeHeldDrums,
    &instrumentClip::VelocityEditor,
    &instrumentClip::DragKitRow,
    &instrumentClip::MuteRow,
    &instrumentClip::SilentAudition,
    &instrumentClip::ResampleRow,
    &instrumentClip::AddKitRow,
    &instrumentClip::NoteLength,
    &instrumentClip::RowPitch,
    &instrumentClip::RowProbability,
    &instrumentClip::NoteProbability,
    &instrumentClip::BrowsePresets,
    &instrumentClip::KnobMidiCc,
    &instrumentClip::MoveCcAutomation,
    &instrumentClip::TransposeScreen,
    &instrumentClip::TransposeInOctave,
    &instrumentClip::Euclidean,
    &instrumentClip::NoteRepeat,
    &instrumentClip::TransposeOctave,
    &instrumentClip::TransposeClip,
    &instrumentClip::NudgeSemitone,
    &instrumentClip::ClipColour,
    &instrumentClip::RowColour,
    &instrumentClip::MoveNotes,
    &instrumentClip::NoteVelocity,
    &instrumentClip::NudgeNotes,
    &instrumentClip::RowLength,
    &instrumentClip::RotateRow,
    &instrumentClip::Quantize,
    &instrumentClip::DrumParam,
    &instrumentClip::ClearClip,
    &instrumentClip::SoundMenu,
    &instrumentClip::AffectEntire,
    &instrumentClip::SelectNotes,
    &instrumentClip::SelectRows,
    &instrumentClip::MoveSelection,
    &instrumentClip::DuplicateSelection,
    &instrumentClip::TransposeSelection,
    &instrumentClip::SelectionSemitone,
    &instrumentClip::SelectionOctave,
    &instrumentClip::SelectionVelocity,
    &instrumentClip::DeleteSelection,
    &instrumentClip::EndSelection,
    &automation::Overview,
    &automation::SongView,
    &automation::KeyboardView,
    &automation::ClipView,
    &automation::LoopView,
    &automation::CopyAutomation,
    &automation::PasteAutomation,
    &automation::DoubleClip,
    &automation::ClearAutomation,
    &automation::SoundMenu,
    &automation::FoldVelocity,
    &automation::Interpolation,
    &automation::PadSelection,
    &automation::VelocityEditor,
    &automation::PickParam,
    &automation::SilentAudition,
    &automation::ShiftAutomation,
    &automation::RowLength,
    &automation::NoteVelocity,
    &automation::TransposeOctave,
    &automation::ClipColour,
    &automation::StepValue,
    &automation::ParamValue,
    &automation::CopyKnobAutomation,
    &automation::PasteKnobAutomation,
    &automation::DeleteKnobAutomation,
    &automation::FastParamSelect,
    &automation::SelectParam,
    &keyboard::SelectScale,
    &keyboard::ScaleMode,
    &keyboard::RootNote,
    &keyboard::CycleScales,
    &keyboard::RootNoteTurn,
    &keyboard::BackToClip,
    &keyboard::Layout,
    &keyboard::SongView,
    &keyboard::ReturnTarget,
    &keyboard::ToKit,
    &keyboard::ToSynth,
    &keyboard::ToMidi,
    &keyboard::ToCv,
    &keyboard::NewSynth,
    &keyboard::NewKit,
    &keyboard::BrowseDrumSample,
    &keyboard::Resample,
    &keyboard::Colours,
    &keyboard::RowInterval,
    &keyboard::ChordMode,
    &keyboard::DrumZoom,
    &keyboard::DrumZoomX,
    &keyboard::ChordVoicing,
    &keyboard::ColumnFunction,
    &keyboard::ColumnSetting,
    &keyboard::ChordMemory,
    &audioClip::SongView,
    &audioClip::RunMacro,
    &audioClip::AutomationView,
    &audioClip::LengthToSample,
    &audioClip::SoundMenu,
    &audioClip::ClearClip,
    &audioClip::GrabTempo,
    &audioClip::EndPoint,
    &audioClip::TrimStart,
    &audioClip::MoveToTrack,
    &audioClip::OutputMode,
    &audioClip::LengthNoStretch,
    &audioClip::ClipColour,
    &session::EnterClip,
    &session::RecordArrangement,
    &session::ClipToArranger,
    &session::ArrangerView,
    &session::AffectEntire,
    &session::RecordArming,
    &session::ExportStems,
    &session::DeleteClip,
    &session::SectionRepeats,
    &session::ClipSettings,
    &session::ClipSettingsRow,
    &session::ClipToAudio,
    &session::SongFx,
    &session::ClipType,
    &session::ClipPreset,
    &session::PerformanceView,
    &session::ShowScale,
    &session::Overdub,
    &session::GrabTempo,
    &session::NewClip,
    &session::CloneClip,
    &session::LaunchSectionNow,
    &session::ArmSection,
    &session::ChangeSection,
    &session::SectionRepeatsTurn,
    &session::ClipPresetTurn,
    &session::Layout,
    &session::ArmNextSection,
    &session::TransposeSong,
    &session::MoveSection,
    &session::ClipColour,
    &session::TrackColour,
    &session::DragClip,
    &session::InsertEmptyPad,
    &session::ReorderTrack,
    &session::SoloClip,
    &session::LaunchClipNow,
    &session::ArmClipGrid,
    &arranger::SongView,
    &arranger::ClipToSession,
    &arranger::ArrangerAutomation,
    &arranger::AffectEntire,
    &arranger::FollowPlayhead,
    &arranger::ExportMixdown,
    &arranger::RecordArming,
    &arranger::DeleteTrack,
    &arranger::TrackToAudio,
    &arranger::SongFx,
    &arranger::TrackType,
    &arranger::TrackPreset,
    &arranger::ClearArrangement,
    &arranger::PerformanceView,
    &arranger::RenameTrack,
    &arranger::SoloTrack,
    &arranger::MuteTrack,
    &arranger::MakeUnique,
    &arranger::MakeSection,
    &arranger::InstanceLength,
    &arranger::CopyInstance,
    &arranger::InstanceClip,
    &arranger::DragInstance,
    &arranger::TrackPresetTurn,
    &arranger::InsertTime,
    &arranger::TransposeSong,
    &arranger::MoveTrack,
    &arranger::PlayFromPad,
    &arranger::SwitchToArrangement,
};
static_assert(isValidGestureTable(kAll));

} // namespace deluge::gui::combos::gesture
