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
#include "gui/l10n/l10n.h"
#include "gui/menu_item/toggle.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/display.h"
#include "processing/sound/sound.h"

namespace deluge::gui::menu_item::filter {

/// One filter for the mix of a synth's voices instead of one per voice - see Sound::setSharedFilter()
class SharedFilter final : public Toggle {
public:
	using Toggle::Toggle;

	void readCurrentValue() override { this->setValue(soundEditor.currentSound->hasSharedFilter()); }

	void writeCurrentValue() override {
		if (!soundEditor.currentSound->setSharedFilter(this->getValue())) {
			this->setValue(false);
			display->displayPopup(l10n::get(l10n::String::STRING_FOR_ERROR_INSUFFICIENT_RAM));
		}
	}

	// Only does anything with more than one voice sounding at once
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		auto* sound = static_cast<Sound*>(modControllable);
		return sound->polyphonic == PolyphonyMode::POLY;
	}
};

} // namespace deluge::gui::menu_item::filter
