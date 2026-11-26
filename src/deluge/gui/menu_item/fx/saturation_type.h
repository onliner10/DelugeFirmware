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

#include "dsp/saturation_curves.h"
#include "gui/l10n/l10n.h"
#include "gui/menu_item/selection.h"
#include "gui/ui/sound_editor.h"
#include "hid/display/oled.h"
#include "model/instrument/kit.h"
#include "model/mod_controllable/mod_controllable_audio.h"
#include "model/song/song.h"
#include "processing/sound/sound_drum.h"
#include <cmath>

namespace deluge::gui::menu_item::fx {

/// The saturation's shaping curve. Each option is drawn as its curve: input across, output up.
class SaturationTypeMenu final : public Selection {
public:
	using Selection::Selection;

	void readCurrentValue() override {
		this->setValue(static_cast<int32_t>(soundEditor.currentModControllable->saturationType));
	}
	bool usesAffectEntire() override { return true; }
	void writeCurrentValue() override {
		auto type = static_cast<dsp::SaturationType>(this->getValue());
		if (currentUIMode == UI_MODE_HOLDING_AFFECT_ENTIRE_IN_SOUND_EDITOR && soundEditor.editingKitRow()) {
			Kit* kit = getCurrentKit();
			for (Drum* thisDrum = kit->firstDrum; thisDrum != nullptr; thisDrum = thisDrum->next) {
				if (thisDrum->type == DrumType::SOUND) {
					static_cast<SoundDrum*>(thisDrum)->saturationType = type;
				}
			}
		}
		else {
			soundEditor.currentModControllable->saturationType = type;
		}
	}

	deluge::vector<std::string_view> getOptions(OptType optType) override {
		using enum l10n::String;
		bool s = optType == OptType::SHORT;
		return {
		    l10n::getView(s ? STRING_FOR_SATURATION_CLASSIC_SHORT : STRING_FOR_SATURATION_CLASSIC),
		    l10n::getView(s ? STRING_FOR_SATURATION_SOFT_SHORT : STRING_FOR_SATURATION_SOFT),
		    l10n::getView(s ? STRING_FOR_SATURATION_KNEE_SHORT : STRING_FOR_SATURATION_KNEE),
		    l10n::getView(s ? STRING_FOR_SATURATION_ROUND_SHORT : STRING_FOR_SATURATION_ROUND),
		    l10n::getView(s ? STRING_FOR_SATURATION_HARD_SHORT : STRING_FOR_SATURATION_HARD),
		    l10n::getView(s ? STRING_FOR_SATURATION_CLIP_SHORT : STRING_FOR_SATURATION_CLIP),
		    l10n::getView(s ? STRING_FOR_SATURATION_BASS_SHORT : STRING_FOR_SATURATION_BASS),
		    l10n::getView(s ? STRING_FOR_SATURATION_FOLD_SHORT : STRING_FOR_SATURATION_FOLD),
		    l10n::getView(s ? STRING_FOR_SATURATION_CRUNCH_SHORT : STRING_FOR_SATURATION_CRUNCH),
		};
	}

	// In the Distortion row the curve is the whole slot, with no label: the picture says which type it is, and the
	// title names it while the slot is selected
	[[nodiscard]] bool showColumnLabel() const override { return false; }
	void getColumnLabel(StringBuf& label) override { label.append(l10n::get(l10n::String::STRING_FOR_TYPE)); }

	void renderInHorizontalMenu(const SlotPosition& slot) override {
		constexpr int32_t kMarginX = 3;
		constexpr int32_t kMarginTop = 2;
		constexpr int32_t kMarginBottom = 5; // clear of the selection underline
		drawCurve(static_cast<dsp::SaturationType>(this->getValue()), slot.start_x + kMarginX,
		          slot.start_y + kMarginTop, slot.width - 2 * kMarginX, slot.height - kMarginTop - kMarginBottom);
	}

	// On its own page: the list of types, and the selected one's curve, bigger, at the right
	void drawPixelsForOled() override {
		Selection::drawPixelsForOled();
		constexpr int32_t kWidth = 34;
		constexpr int32_t kHeight = 26;
		constexpr int32_t kX = OLED_MAIN_WIDTH_PIXELS - kWidth - 3;
		constexpr int32_t kY = OLED_MAIN_TOPMOST_PIXEL + 14;
		hid::display::OLED::main.drawRectangleRounded(kX - 2, kY - 2, kX + kWidth + 1, kY + kHeight + 1);
		drawCurve(static_cast<dsp::SaturationType>(this->getValue()), kX, kY, kWidth, kHeight);
	}

private:
	/// The curve alone, filling the box. Drawn over the middle of its range, where the knee is: further out every type
	/// is flat at full scale except FOLD.
	static void drawCurve(dsp::SaturationType type, int32_t x, int32_t y, int32_t width, int32_t height) {
		auto& image = hid::display::OLED::main;
		int32_t midY = y + height / 2;
		int32_t halfHeight = (height - 1) / 2;
		int32_t lastX = 0;
		int32_t lastY = 0;
		for (int32_t i = 0; i < width; i++) {
			constexpr float kRange = 0.6f;
			float u = kRange * (-1.f + 2.f * static_cast<float>(i) / static_cast<float>(width - 1));
			float level = dsp::saturationCurve(type, u) / dsp::saturationCurve(dsp::SaturationType::CLIP, kRange);
			int32_t py = midY - static_cast<int32_t>(std::lround(level * halfHeight));
			if (i > 0) {
				image.drawLine(lastX, lastY, x + i, py);
			}
			lastX = x + i;
			lastY = py;
		}
	}
};

} // namespace deluge::gui::menu_item::fx
