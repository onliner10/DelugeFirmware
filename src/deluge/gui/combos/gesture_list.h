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

#pragma once

#include "definitions.h"
#include "gui/combos/combo.h"

/// Every gesture command search can show, one namespace per view (gui/combos/tables.h lists them per view).
/// The code that handles one calls used() with it, e.g. used(gesture::instrumentClip::TransposeClip).
// clang-format off
namespace deluge::gui::combos::gesture {

namespace gbtn = deluge::hid::button;

// Where a gesture makes sense (gesture_list.cpp)
bool applies_kit();
bool applies_notkit();
bool applies_synth();
bool applies_midi();
bool applies_notsynth();
bool applies_notmidi();
bool applies_notcv();
bool applies_instrument();
bool applies_audio();
bool applies_dx7();
bool applies_quantize();
bool applies_randomizer();
bool applies_kitrandom();
bool applies_sticky();
bool applies_overlay();
bool applies_emudisplay();
bool applies_finetempo();
bool applies_trimstart();
bool applies_loopads();
bool applies_rows();
bool applies_grid();
bool applies_isolayout();
bool applies_chordkb();
bool applies_chordlib();
bool applies_drumkb();
bool applies_columns();
bool applies_restartx();
bool applies_restartcross();
bool applies_fromscreenx();
bool applies_fromstartx();
bool applies_fromscreencross();
bool applies_selection();
bool applies_noselection();
bool applies_fromstartcross();

namespace global { // Mostly handled in hid/buttons.cpp
extern Gesture const Play;
extern Gesture const RestartPlay;
extern Gesture const RestartPlayCross;
extern Gesture const PlayFromScreen;
extern Gesture const PlayFromScreenCross;
extern Gesture const PlayFromStart;
extern Gesture const PlayFromStartCross;
extern Gesture const PlayRecordOutput;
extern Gesture const RecordOutput;
extern Gesture const Record;
extern Gesture const ClearTempoAutomation;
extern Gesture const ShowSwing;
extern Gesture const ShowTempo;
extern Gesture const SwingInterval;
extern Gesture const NudgeSync;
extern Gesture const SwingAmount;
extern Gesture const ClockOutScale;
extern Gesture const Tempo;
extern Gesture const FineTempo;
extern Gesture const PadBrightness;
extern Gesture const PadRefresh;
extern Gesture const ThresholdRecording;
extern Gesture const SaveSkipback;
extern Gesture const SwapDisplay;
extern Gesture const ExitMenus;
extern Gesture const StickyShift;
} // namespace global

namespace view { // Mostly handled in gui/views/view.cpp
extern Gesture const TapTempo;
extern Gesture const MidiLearn;
extern Gesture const SaveSong;
extern Gesture const LoadSong;
extern Gesture const SyncScaling;
extern Gesture const Undo;
extern Gesture const KnobParam;
extern Gesture const ParamLockNotes;
extern Gesture const KnobButton;
extern Gesture const LaunchNow;
extern Gesture const SoloClip;
extern Gesture const ArmClip;
} // namespace view

namespace timeline { // Mostly handled in gui/views/timeline_view.cpp
extern Gesture const ShowZoom;
extern Gesture const Triplets;
extern Gesture const Zoom;
extern Gesture const Scroll;
} // namespace timeline

namespace clip { // Mostly handled in gui/views/clip_view.cpp
extern Gesture const ClipLength;
extern Gesture const ShiftContents;
extern Gesture const ShortcutOverview;
} // namespace clip

namespace instrumentClip { // Mostly handled in gui/views/instrument_clip_view.cpp
extern Gesture const SongView;
extern Gesture const SessionMacros;
extern Gesture const RunMacro;
extern Gesture const AutomationView;
extern Gesture const KeyboardView;
extern Gesture const CrossScreen;
extern Gesture const RecordToRow;
extern Gesture const BrowseRowSample;
extern Gesture const BrowseOscSample;
extern Gesture const ToKit;
extern Gesture const ToSynth;
extern Gesture const ToMidi;
extern Gesture const ToCv;
extern Gesture const NewFmSynth;
extern Gesture const SynthRow;
extern Gesture const MidiRow;
extern Gesture const GateRow;
extern Gesture const SaveRow;
extern Gesture const DeleteRow;
extern Gesture const MoveKitRowHere;
extern Gesture const NoteEditor;
extern Gesture const RowEditor;
extern Gesture const ScaleMode;
extern Gesture const ShowScale;
extern Gesture const RootNote;
extern Gesture const RandomizeDrum;
extern Gesture const RandomizeAllDrums;
extern Gesture const RandomizeHeldDrums;
extern Gesture const VelocityEditor;
extern Gesture const DragKitRow;
extern Gesture const MuteRow;
extern Gesture const SilentAudition;
extern Gesture const ResampleRow;
extern Gesture const AddKitRow;
extern Gesture const NoteLength;
extern Gesture const RowPitch;
extern Gesture const RowProbability;
extern Gesture const NoteProbability;
extern Gesture const BrowsePresets;
extern Gesture const KnobMidiCc;
extern Gesture const MoveCcAutomation;
extern Gesture const TransposeScreen;
extern Gesture const TransposeInOctave;
extern Gesture const Euclidean;
extern Gesture const NoteRepeat;
extern Gesture const TransposeOctave;
extern Gesture const TransposeClip;
extern Gesture const NudgeSemitone;
extern Gesture const ClipColour;
extern Gesture const RowColour;
extern Gesture const MoveNotes;
extern Gesture const NoteVelocity;
extern Gesture const NudgeNotes;
extern Gesture const RowLength;
extern Gesture const RotateRow;
extern Gesture const Quantize;
extern Gesture const DrumParam;
extern Gesture const ClearClip;
extern Gesture const SoundMenu;
extern Gesture const AffectEntire;
extern Gesture const SelectNotes;
extern Gesture const SelectRows;
extern Gesture const MoveSelection;
extern Gesture const DuplicateSelection;
extern Gesture const TransposeSelection;
extern Gesture const SelectionSemitone;
extern Gesture const SelectionOctave;
extern Gesture const SelectionVelocity;
extern Gesture const DeleteSelection;
extern Gesture const EndSelection;
} // namespace instrumentClip

namespace automation { // Mostly handled in gui/views/automation_view.cpp
extern Gesture const Overview;
extern Gesture const SongView;
extern Gesture const KeyboardView;
extern Gesture const ClipView;
extern Gesture const LoopView;
extern Gesture const CopyAutomation;
extern Gesture const PasteAutomation;
extern Gesture const DoubleClip;
extern Gesture const ClearAutomation;
extern Gesture const SoundMenu;
extern Gesture const FoldVelocity;
extern Gesture const Interpolation;
extern Gesture const PadSelection;
extern Gesture const VelocityEditor;
extern Gesture const PickParam;
extern Gesture const SilentAudition;
extern Gesture const ShiftAutomation;
extern Gesture const RowLength;
extern Gesture const NoteVelocity;
extern Gesture const TransposeOctave;
extern Gesture const ClipColour;
extern Gesture const StepValue;
extern Gesture const ParamValue;
extern Gesture const CopyKnobAutomation;
extern Gesture const PasteKnobAutomation;
extern Gesture const DeleteKnobAutomation;
extern Gesture const FastParamSelect;
extern Gesture const SelectParam;
} // namespace automation

namespace keyboard { // Mostly handled in gui/ui/keyboard/keyboard_screen.cpp
extern Gesture const SelectScale;
extern Gesture const ScaleMode;
extern Gesture const RootNote;
extern Gesture const CycleScales;
extern Gesture const RootNoteTurn;
extern Gesture const BackToClip;
extern Gesture const Layout;
extern Gesture const SongView;
extern Gesture const ReturnTarget;
extern Gesture const ToKit;
extern Gesture const ToSynth;
extern Gesture const ToMidi;
extern Gesture const ToCv;
extern Gesture const NewSynth;
extern Gesture const NewKit;
extern Gesture const BrowseDrumSample;
extern Gesture const Resample;
extern Gesture const Colours;
extern Gesture const RowInterval;
extern Gesture const ChordMode;
extern Gesture const DrumZoom;
extern Gesture const DrumZoomX;
extern Gesture const ChordVoicing;
extern Gesture const ColumnFunction;
extern Gesture const ColumnSetting;
extern Gesture const ChordMemory;
} // namespace keyboard

namespace audioClip { // Mostly handled in gui/views/audio_clip_view.cpp
extern Gesture const SongView;
extern Gesture const RunMacro;
extern Gesture const AutomationView;
extern Gesture const LengthToSample;
extern Gesture const SoundMenu;
extern Gesture const ClearClip;
extern Gesture const GrabTempo;
extern Gesture const EndPoint;
extern Gesture const TrimStart;
extern Gesture const MoveToTrack;
extern Gesture const OutputMode;
extern Gesture const LengthNoStretch;
extern Gesture const ClipColour;
} // namespace audioClip

namespace session { // Mostly handled in gui/views/session_view.cpp
extern Gesture const EnterClip;
extern Gesture const RecordArrangement;
extern Gesture const ClipToArranger;
extern Gesture const ArrangerView;
extern Gesture const AffectEntire;
extern Gesture const RecordArming;
extern Gesture const ExportStems;
extern Gesture const DeleteClip;
extern Gesture const SectionRepeats;
extern Gesture const ClipSettings;
extern Gesture const ClipSettingsRow;
extern Gesture const ClipToAudio;
extern Gesture const SongFx;
extern Gesture const ClipType;
extern Gesture const ClipPreset;
extern Gesture const PerformanceView;
extern Gesture const ShowScale;
extern Gesture const Overdub;
extern Gesture const GrabTempo;
extern Gesture const NewClip;
extern Gesture const CloneClip;
extern Gesture const LaunchSectionNow;
extern Gesture const ArmSection;
extern Gesture const ChangeSection;
extern Gesture const SectionRepeatsTurn;
extern Gesture const ClipPresetTurn;
extern Gesture const Layout;
extern Gesture const ArmNextSection;
extern Gesture const TransposeSong;
extern Gesture const MoveSection;
extern Gesture const ClipColour;
extern Gesture const TrackColour;
extern Gesture const DragClip;
extern Gesture const InsertEmptyPad;
extern Gesture const ReorderTrack;
extern Gesture const SoloClip;
extern Gesture const LaunchClipNow;
extern Gesture const ArmClipGrid;
} // namespace session

namespace arranger { // Mostly handled in gui/views/arranger_view.cpp
extern Gesture const SongView;
extern Gesture const ClipToSession;
extern Gesture const ArrangerAutomation;
extern Gesture const AffectEntire;
extern Gesture const FollowPlayhead;
extern Gesture const ExportMixdown;
extern Gesture const RecordArming;
extern Gesture const DeleteTrack;
extern Gesture const TrackToAudio;
extern Gesture const SongFx;
extern Gesture const TrackType;
extern Gesture const TrackPreset;
extern Gesture const ClearArrangement;
extern Gesture const PerformanceView;
extern Gesture const RenameTrack;
extern Gesture const SoloTrack;
extern Gesture const MuteTrack;
extern Gesture const MakeUnique;
extern Gesture const MakeSection;
extern Gesture const InstanceLength;
extern Gesture const CopyInstance;
extern Gesture const InstanceClip;
extern Gesture const DragInstance;
extern Gesture const TrackPresetTurn;
extern Gesture const InsertTime;
extern Gesture const TransposeSong;
extern Gesture const MoveTrack;
extern Gesture const PlayFromPad;
extern Gesture const SwitchToArrangement;
} // namespace arranger

} // namespace deluge::gui::combos::gesture
// clang-format on
