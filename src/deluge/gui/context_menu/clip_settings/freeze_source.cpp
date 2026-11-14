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

#include "gui/context_menu/clip_settings/freeze_source.h"
#include "gui/context_menu/clip_settings/clip_settings.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/ui.h"
#include "hid/display/display.h"
#include "model/clip/clip_freezer.h"
#include "model/clip/instrument_clip.h"

namespace deluge::gui::context_menu::clip_settings {

PLACE_SDRAM_BSS FreezeSourceMenu freezeSource{};

char const* FreezeSourceMenu::getTitle() {
	return l10n::get(l10n::String::STRING_FOR_FREEZE_FROM);
}

std::span<char const*> FreezeSourceMenu::getOptions() {
	using enum l10n::String;
	static char const* options[] = {
	    l10n::get(STRING_FOR_STEREO_INPUT),   l10n::get(STRING_FOR_LEFT_INPUT), l10n::get(STRING_FOR_RIGHT_INPUT),
	    l10n::get(STRING_FOR_BALANCED_INPUT), l10n::get(STRING_FOR_USB_INPUT),
	};
	static_assert(std::size(options) == kInputs.size());
	return {options, kInputs.size()};
}

bool FreezeSourceMenu::setupAndCheckAvailability() {
	currentOption = 0;
	for (size_t i = 0; i < kInputs.size(); i++) {
		if (kInputs[i] == clipFreezer.lastInputChannel) {
			currentOption = static_cast<int32_t>(i);
		}
	}
	if (display->haveOLED()) {
		scrollPos = currentOption;
	}
	return true;
}

bool FreezeSourceMenu::acceptCurrentOption() {
	InstrumentClip* clipToFreeze = clip;
	AudioInputChannel input = kInputs[currentOption];
	// Back to the view the menus were opened over, before freezing shows its progress there
	closeUI(&clipSettings);
	clipFreezer.start(clipToFreeze, input);
	return true; // Already closed
}

} // namespace deluge::gui::context_menu::clip_settings
