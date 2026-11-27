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

#include "gui/combos/combo.h"
#include <span>

/// Each view's gestures, defined in the file whose code handles them. Command search lists the ones of the view it
/// was opened from.
namespace deluge::gui::combos {

using Gestures = std::span<Gesture const* const>;

Gestures globalGestures();         // hid/buttons.cpp, hid/encoder_input.cpp, playback: everywhere
Gestures viewGestures();           // gui/views/view.cpp: every view
Gestures timelineGestures();       // gui/views/timeline_view.cpp: clip, song and arranger views
Gestures clipGestures();           // gui/views/clip_view.cpp: instrument and audio clip views
Gestures instrumentClipGestures(); // gui/views/instrument_clip_view.cpp and the instrument clip minder
Gestures automationGestures();     // gui/views/automation_view.cpp
Gestures keyboardGestures();       // gui/ui/keyboard
Gestures audioClipGestures();      // gui/views/audio_clip_view.cpp
Gestures sessionGestures();        // gui/views/session_view.cpp
Gestures arrangerGestures();       // gui/views/arranger_view.cpp

/// Compile-time checks for a gesture table: every input has a command search name
consteval bool isValidGestureTable(std::span<Gesture const* const> table) {
	for (Gesture const* gesture : table) {
		if (gesture == nullptr || gesture->input == kNoButton || buttonName(gesture->input) == "?"
		    || (gesture->hold != kNoButton && buttonName(gesture->hold) == "?")
		    || (gesture->hold2 != kNoButton && buttonName(gesture->hold2) == "?")) {
			return false;
		}
	}
	return true;
}

} // namespace deluge::gui::combos
