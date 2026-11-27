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

#include "definitions_cxx.hpp"
#include "gui/l10n/strings.h"
#include "hid/button.h"
#include <optional>
#include <span>
#include <string_view>

/// Button combos declared as data: one entry says which button is held, what to press, what it's called and what it
/// does. The same table both runs the combo and feeds the command search display, so a shortcut can't exist without its
/// description, or be described without existing.
namespace deluge::gui::combos {

/// The button held down to reach a group of shortcuts
enum class Held : uint8_t {
	SAVE,  // UI_MODE_HOLDING_SAVE_BUTTON. Any press ends it.
	LOAD,  // UI_MODE_HOLDING_LOAD_BUTTON
	LEARN, // The learn button itself (UI_MODE_MIDI_LEARN)
	SHIFT, // Shift (sticky too), Entries say in `available` which UI modes they work in.
};

inline constexpr hid::Button kNoButton = 0; // No real button has code 0

/// Pads by what they're for, to hold or press in a Gesture. No button has these codes.
namespace pad {
inline constexpr hid::Button NOTE = 240;     // A note on the grid
inline constexpr hid::Button AUDITION = 241; // A row's audition pad (right column), a track's in the arranger
inline constexpr hid::Button MUTE = 242;     // A row's or clip's mute / launch pad (left of the audition pads)
inline constexpr hid::Button CLIP = 243;     // A clip in the song view
inline constexpr hid::Button SECTION = 244;  // A section pad in the song view
inline constexpr hid::Button EMPTY = 245;    // An empty slot on the grid
inline constexpr hid::Button GRID = 246;     // Some pad on the grid, the gesture says which
} // namespace pad

struct Shortcut {
	Held held;
	/// Pressed while `held` is held
	hid::Button trigger;
	/// Also held down, e.g. SHIFT for a variant. Entries for the same trigger are tried in order, so put the ones with
	/// `with` first (checked at compile time).
	hid::Button with;
	/// The action, as command search shows it ("Copy notes")
	l10n::String name;
	/// Whether it applies right now (instrument type etc.). Null means always. When false, command search leaves it out
	/// and the press goes on to whatever the button does otherwise.
	bool (*available)();
	ActionResult (*run)(hid::Button trigger);
	/// Other words command search should find it by, lowercase, space separated ("duplicate clone")
	char const* keywords = nullptr;
	/// Runs when the trigger is let go rather than pressed, and only if no other button was pressed meanwhile - for
	/// buttons that are also held for other combos (shift + cross screen, vs shift + cross screen + <>)
	bool onRelease = false;
};

/// What a Gesture does to its input
enum class Action : uint8_t {
	PRESS,
	TAP,  // Press and let go, nothing else meanwhile
	HOLD, // Keep it down
	TURN, // An encoder
};

/// A gesture command search can show but not do, because it needs an amount, a direction or a pad: "hold an
/// audition pad, push and turn ^v". The view's own code handles it where it always did and names the entry it's
/// handling with used(), which in a debug build checks that the buttons the entry says are held really are. So the
/// description lives here once and can't quietly drift from the code.
struct Gesture {
	hid::Button hold;  // kNoButton, a button or a pad::
	hid::Button hold2; // Also held
	hid::Button input; // What's pressed or turned
	Action action;
	l10n::String name;
	/// Whether it makes sense here: clip type, a setting. Never what's held - command search lists with nothing held.
	bool (*applies)() = nullptr;
	char const* keywords = nullptr;
	/// For a plain button press: lets command search do it too, by calling what the view's code calls
	ActionResult (*run)() = nullptr;
	/// For a gesture on one particular pad: which, so command search can draw it on the grid
	int8_t padX = -1;
	int8_t padY = -1;
};

/// Called by the code that handles a gesture. Debug builds log it and check the held buttons; release builds: nothing.
#if ENABLE_TEXT_OUTPUT
void used(Gesture const& gesture);
#else
inline void used(Gesture const&) {
}
#endif

/// "TURN ^v", "TAP SCALE": a gesture's input as command search shows it
std::string_view inputName(hid::Button input, Action action, char (&buffer)[24]);

/// What command search calls a button: the word printed on the panel, or for an encoder, what to do with it
constexpr std::string_view buttonName(hid::Button b) {
	using namespace hid::button;
	switch (b) {
	case X_ENC:
		return "PUSH <>";
	case Y_ENC:
		return "PUSH ^v";
	case MOD_ENCODER_0:
	case MOD_ENCODER_1:
		return "PUSH GOLD KNOB"; // Either: the one for the param it's about
	case SELECT_ENC:
		return "PUSH SELECT";
	case CROSS_SCREEN_EDIT:
		return "CROSS";
	case SCALE_MODE:
		return "SCALE";
	case RECORD:
		return "REC";
	case SHIFT:
		return "SHIFT";
	case SYNTH:
		return "SYNTH";
	case KIT:
		return "KIT";
	case MIDI:
		return "MIDI";
	case CV:
		return "CV";
	case PLAY:
		return "PLAY";
	case BACK:
		return "BACK";
	case TAP_TEMPO:
		return "TAP";
	case SAVE:
		return "SAVE";
	case LOAD:
		return "LOAD";
	case LEARN:
		return "LEARN";
	case SESSION_VIEW:
		return "SONG";
	case CLIP_VIEW:
		return "CLIP";
	case KEYBOARD:
		return "KEYBOARD";
	case AFFECT_ENTIRE:
		return "AFFECT ENTIRE";
	case TRIPLETS:
		return "TRIPLETS";
	case SYNC_SCALING:
		return "SYNC-SCALING";
	case TEMPO_ENC:
		return "PUSH TEMPO";
	case MOD7:
		return "MOD 8";
	case pad::NOTE:
		return "NOTE PAD";
	case pad::AUDITION:
		return "AUDITION PAD";
	case pad::MUTE:
		return "MUTE PAD";
	case pad::CLIP:
		return "CLIP PAD";
	case pad::SECTION:
		return "SECTION PAD";
	case pad::EMPTY:
		return "EMPTY PAD";
	case pad::GRID:
		return "PAD";
	default:
		return "?";
	}
}

/// Compile-time checks for a table: every button has a command search name, every (held, trigger, with) appears once,
/// and no entry is shadowed by an earlier one for the same trigger that doesn't need `with`.
consteval bool isValidTable(std::span<const Shortcut> table) {
	for (size_t i = 0; i < table.size(); i++) {
		if (table[i].trigger == kNoButton || table[i].run == nullptr || buttonName(table[i].trigger) == "?"
		    || (table[i].with != kNoButton && buttonName(table[i].with) == "?")) {
			return false; // Every button in a table needs a command search name
		}
		for (size_t j = i + 1; j < table.size(); j++) {
			if (table[i].held != table[j].held || table[i].trigger != table[j].trigger) {
				continue;
			}
			if (table[i].with == table[j].with) {
				return false; // Duplicate
			}
			if (table[i].with == kNoButton) {
				return false; // Would always win over the more specific entry j
			}
		}
	}
	return true;
}

/// The shortcut group whose button is held right now, if any
std::optional<Held> currentlyHeld();

/// If a press matches an entry, runs it (or, in card routine, asks to be called again) and returns the result.
/// Returns nullopt when nothing matched, so the caller carries on with its own handling.
std::optional<ActionResult> dispatch(std::span<const Shortcut> table, hid::Button b, bool on, bool inCardRoutine);

/// Whether a shortcut's `with` button is down (kNoButton: always)
bool isHeld(hid::Button with);

} // namespace deluge::gui::combos
