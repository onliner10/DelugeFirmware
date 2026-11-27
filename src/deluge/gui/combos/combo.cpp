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

#include "gui/combos/combo.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/ui.h"
#include "hid/buttons.h"
#include "hid/led/indicator_leds.h"
#include "io/debug/log.h"

namespace deluge::gui::combos {

bool isHeld(hid::Button with) {
	if (with == kNoButton) {
		return true;
	}
	if (with == hid::button::SHIFT) {
		return Buttons::isShiftButtonPressed(); // Counts sticky shift too
	}
	return Buttons::isButtonPressed(with);
}

std::optional<Held> currentlyHeld() {
	if (currentUIMode == UI_MODE_HOLDING_SAVE_BUTTON) {
		return Held::SAVE;
	}
	if (currentUIMode == UI_MODE_HOLDING_LOAD_BUTTON) {
		return Held::LOAD;
	}
	if (Buttons::isButtonPressed(hid::button::LEARN)) {
		return Held::LEARN;
	}
	// After learn, so learn + shift + something is a learn shortcut
	if (Buttons::isShiftButtonPressed()) {
		return Held::SHIFT;
	}
	return std::nullopt;
}

// Leaving the held mode is part of using a SAVE / LOAD shortcut
static void endHeldMode(Held held) {
	if (held == Held::SAVE || held == Held::LOAD) {
		currentUIMode = UI_MODE_NONE;
		indicator_leds::setLedState(held == Held::SAVE ? IndicatorLED::SAVE : IndicatorLED::LOAD, false);
	}
}

// An on-release shortcut whose trigger is held down right now
static Shortcut const* pendingRelease = nullptr;

static bool isIn(std::span<const Shortcut> table, Shortcut const* shortcut) {
	return shortcut >= table.data() && shortcut < table.data() + table.size();
}

std::optional<ActionResult> dispatch(std::span<const Shortcut> table, hid::Button b, bool on, bool inCardRoutine) {
	if (!on) {
		// Releases carry on to whoever expects them, apart from the release an on-release shortcut waits for
		Shortcut const* shortcut = pendingRelease;
		if (shortcut == nullptr || shortcut->trigger != b || !isIn(table, shortcut)) {
			return std::nullopt;
		}
		if (inCardRoutine) {
			return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE; // Comes back with the same release
		}
		pendingRelease = nullptr;
		if (shortcut->available != nullptr && !shortcut->available()) {
			return std::nullopt;
		}
		return shortcut->run(b);
	}

	// Pressing anything else while holding an on-release trigger makes it part of another combo. A new press of the
	// trigger itself starts over too, in case its last release went to another UI and never got here.
	if (pendingRelease != nullptr && (pendingRelease->trigger != b || isIn(table, pendingRelease))) {
		pendingRelease = nullptr;
	}

	std::optional<Held> held = currentlyHeld();
	if (!held) {
		return std::nullopt;
	}

	for (Shortcut const& shortcut : table) {
		if (shortcut.held != *held || shortcut.trigger != b) {
			continue;
		}
		if (!isHeld(shortcut.with)) {
			continue;
		}
		if (inCardRoutine) {
			return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
		}
		if (shortcut.available != nullptr && !shortcut.available()) {
			continue; // Let the button do whatever it does on its own
		}
		if (shortcut.onRelease) {
			pendingRelease = &shortcut;
			return std::nullopt; // The press still does what it does
		}
		endHeldMode(*held);
		return shortcut.run(b);
	}
	return std::nullopt;
}

std::string_view inputName(hid::Button input, Action action, char (&buffer)[24]) {
	std::string_view name = buttonName(input);
	std::string_view prefix;
	switch (action) {
	case Action::TURN:
		// Encoders' names say "PUSH"; turning is the other thing you can do with them
		prefix = "TURN ";
		if (name.starts_with("PUSH ")) {
			name.remove_prefix(5);
		}
		break;
	case Action::TAP:
		prefix = "TAP ";
		break;
	case Action::HOLD:
		prefix = "HOLD ";
		break;
	case Action::PRESS:
		return name;
	}
	size_t n = prefix.copy(buffer, sizeof(buffer) - 1);
	n += name.copy(buffer + n, sizeof(buffer) - 1 - n);
	buffer[n] = 0;
	return {buffer, n};
}

#if ENABLE_TEXT_OUTPUT
static bool buttonHeld(hid::Button b) {
	if (b == hid::button::MOD_ENCODER_0 || b == hid::button::MOD_ENCODER_1) {
		return isHeld(hid::button::MOD_ENCODER_0) || isHeld(hid::button::MOD_ENCODER_1); // Either gold knob
	}
	return b == kNoButton || b >= pad::NOTE || isHeld(b); // Pads aren't checked
}

void used(Gesture const& gesture) {
	bool matches = buttonHeld(gesture.hold) && buttonHeld(gesture.hold2);
	D_PRINTLN("COMBO gesture [%s]%s", deluge::l10n::get(gesture.name), matches ? "" : " HOLD MISMATCH");
}
#endif

} // namespace deluge::gui::combos
