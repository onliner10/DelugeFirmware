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

#include "gui/ui/ui.h"
#include "processing/skipback/skipback.h"

/// The skipback screen, opened by Record + Select: what skipback kept (processing/skipback/skipback.h), frozen, as a
/// waveform across the pads, oldest on the left. A range is chosen for you - the last phrase played, in whole bars if
/// the transport was playing - and shown bright between a green start and a red end.
///
/// - Main pad: moves the nearer of start and end to that column
/// - Select turn: nudges the marker last moved (a 1/16 note when on the song's grid, else 10 ms; 1 ms with Shift)
/// - Audition pad (right column): plays the range, in place of the output; again to stop
/// - Horizontal encoder: scrolls; with Shift or pressed, zooms
/// - Select press (or Record + Select again): saves the range - WAV, audio clip, and clips of its notes
/// - Back: leaves without saving
class SkipbackUI final : public UI {
public:
	SkipbackUI() { oledShowsUIUnderneath = false; }

	bool opened() override;
	ActionResult padAction(int32_t x, int32_t y, int32_t velocity) override;
	ActionResult buttonAction(deluge::hid::Button b, bool on, bool inCardRoutine) override;
	ActionResult horizontalEncoderAction(int32_t offset) override;
	ActionResult verticalEncoderAction(int32_t offset, bool inCardRoutine) override { return ActionResult::DEALT_WITH; }
	void selectEncoderAction(int8_t offset) override;
	void graphicsRoutine() override;
	ActionResult exitUI() override;

	bool renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth] = nullptr,
	                    uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth] = nullptr,
	                    bool drawUndefinedArea = true) override;
	bool renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth] = nullptr,
	                   uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth] = nullptr) override;
	void renderOLED(deluge::hid::display::oled_canvas::Canvas& canvas) override;

	UIType getUIType() override { return UIType::SKIPBACK; }

	/// Saves the range and leaves. Record + Select calls it too, from hid/buttons.cpp.
	void saveAndExit();

private:
	void exit();
	void fitView();
	void keepInView(int64_t frame);
	void moveMarker(bool end, int64_t frame);
	void rangeChanged();
	void displayText();
	[[nodiscard]] int64_t gridFrames() const;
	[[nodiscard]] int32_t columnOf(int64_t frame) const;

	deluge::skipback::Capture capture{};
	deluge::skipback::Range range{};
	int64_t viewStart = 0; ///< Frame at the left edge of column 0
	int64_t zoom = 1;      ///< Frames per column
	bool movingEnd = true; ///< Which marker Select nudges: the one last moved
	bool xEncoderHeld = false;
	int32_t numNotes = 0; ///< In the range
	int32_t playheadColumn = -1;
	bool wasPreviewing = false;
};

extern SkipbackUI skipbackUI;
