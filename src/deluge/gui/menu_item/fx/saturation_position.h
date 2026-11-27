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
#include "gui/menu_item/selection.h"
#include "gui/ui/sound_editor.h"
#include "model/instrument/kit.h"
#include "model/song/song.h"
#include "processing/sound/sound.h"
#include "processing/sound/sound_drum.h"

namespace deluge::gui::menu_item::fx {

/// Whether the sound's saturation (drive, with its type) comes before the filter or after it (and after the amplitude
/// envelope), see Sound::saturationPreFilter
class SaturationPosition final : public Selection {
public:
	using Selection::Selection;
	void readCurrentValue() override { this->setValue(soundEditor.currentSound->saturationPreFilter ? 0 : 1); }

	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		bool const pre = this->getValue() == 0;
		// If affect-entire button held, do whole kit
		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					static_cast<SoundDrum*>(thisDrum)->saturationPreFilter = pre;
				}
			}
		}
		else {
			soundEditor.currentSound->saturationPreFilter = pre;
		}
	}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		return {l10n::getView(l10n::String::STRING_FOR_BEFORE_FILTER),
		        l10n::getView(l10n::String::STRING_FOR_AFTER_FILTER)};
	}
};

} // namespace deluge::gui::menu_item::fx
