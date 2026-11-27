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
#include "gui/ui/qwerty_ui.h"
#include <array>
#include <string_view>

class MenuItem;

/// Command search (Shift + keyboard button): type on the pad keyboard to find an action, see its button combo, turn
/// select to go through the matches and press select or enter to do it. Lists the combo tables and the Shift + pad
/// parameter shortcuts, so whatever has a shortcut can be found here.
class CommandSearchUI final : public QwertyUI {
public:
	CommandSearchUI();
	bool opened() override;
	void displayText(bool blinkImmediately = false) override;
	void renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) override;
	bool getGreyoutColsAndRows(uint32_t* cols, uint32_t* rows) override;
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	void selectEncoderAction(int8_t offset) override;
	ActionResult verticalEncoderAction(int32_t offset, bool inCardRoutine) override;
	UIType getUIType() override { return UIType::COMMAND_SEARCH; }

	/// Whether it can open here: OLED, and the view on screen is one with a list
	static bool canOpen();

protected:
	void enterKeyPress() override;

private:
	using Shortcut = deluge::gui::combos::Shortcut;

	/// One thing command search can find: a button combo, or a Shift + pad shortcut to a parameter
	struct Result {
		Shortcut const* combo = nullptr;
		MenuItem* param = nullptr;
		deluge::gui::combos::Gesture const* gesture = nullptr;
		int8_t x = 0; // The param's shortcut pad
		int8_t y = 0;
		bool secondLayer = false; // Pad pressed twice
		bool operator==(Result const& other) const = default;
		bool empty() const { return combo == nullptr && param == nullptr && gesture == nullptr; }
	};

	/// Something that applies to this clip, found when command search opens
	struct Candidate {
		Result result;
		char name[28];
		std::string_view keywords;
	};

	static constexpr size_t kMaxResults = 400;
	static constexpr size_t kNumRecent = 4;

	void collectCandidates();
	void collectParams();
	size_t comboText(Result const& result, char* text, size_t size);
	void addCandidate(Result const& result, std::string_view name, std::string_view keywords);
	void search();
	void runSelected();
	void exit();
	void drawCombo(deluge::hid::display::oled_canvas::Canvas& canvas, Result const& result, int32_t centreY);

	std::array<Candidate, kMaxResults> candidates_{};
	size_t numCandidates_ = 0;
	std::array<uint16_t, kMaxResults> results_{}; // Indices into candidates_, best match first
	std::array<int32_t, kMaxResults> scores_{};
	size_t numResults_ = 0;
	size_t selected_ = 0;
	std::array<Result, kNumRecent> recent_{}; // Most recently run first
};

extern CommandSearchUI commandSearchUI;
