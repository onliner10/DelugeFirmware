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

#include "gui/ui/skipback_ui.h"
#include "definitions_cxx.hpp"
#include "gui/colour/colour.h"
#include "hid/button.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "lib/printf.h"
#include <algorithm>
#include <cmath>

using namespace deluge;
namespace sb = deluge::skipback;

PLACE_SDRAM_BSS SkipbackUI skipbackUI{};

namespace {
constexpr int64_t kMinZoom = 16; // Frames per column, zoomed all the way in
constexpr int32_t kSilence = 8;

RGB scaled(RGB colour, int32_t amount) { // amount out of 256
	return colour.transform([amount](auto channel) { return (channel * amount) >> 8; });
}

const RGB kRangeColour{0, 150, 255};
const RGB kOutsideColour = RGB::monochrome(70);
const RGB kPlayheadColour = RGB::monochrome(255);
} // namespace

int64_t SkipbackUI::gridFrames() const {
	// A 1/16 note on the song's grid; 10 ms (1 ms with Shift) otherwise
	if (capture.playing) {
		return std::max<int64_t>(
		    std::llround(static_cast<double>(std::max<int64_t>(capture.barLength / 16, 1)) * capture.samplesPerTick),
		    1);
	}
	return Buttons::isShiftButtonPressed() ? kSampleRate / 1000 : kSampleRate / 100;
}

int32_t SkipbackUI::columnOf(int64_t frame) const {
	int64_t const offset = frame - viewStart;
	if (offset < 0) {
		return -1;
	}
	return static_cast<int32_t>(std::min<int64_t>(offset / zoom, kDisplayWidth));
}

bool SkipbackUI::opened() {
	if (!sb::freeze(capture)) {
		return false;
	}
	range = sb::suggest(capture);
	movingEnd = true;
	xEncoderHeld = false;
	playheadColumn = -1;
	wasPreviewing = false;
	fitView();
	rangeChanged();
	return true;
}

/// Zooms so the range takes about two thirds of the screen, and centres it
void SkipbackUI::fitView() {
	int64_t const wanted = std::max<int64_t>(range.length() * 3 / 2, kDisplayWidth * kMinZoom);
	zoom = kMinZoom;
	while (zoom * kDisplayWidth < wanted) {
		zoom <<= 1;
	}
	viewStart = range.first + range.length() / 2 - (zoom * kDisplayWidth) / 2;
}

void SkipbackUI::keepInView(int64_t frame) {
	int32_t const column = columnOf(frame);
	if (column < 0) {
		viewStart = frame - zoom;
	}
	else if (column >= kDisplayWidth) {
		viewStart = frame - zoom * (kDisplayWidth - 1);
	}
}

void SkipbackUI::rangeChanged() {
	numNotes = sb::countNotes(capture, range);
	if (sb::isPreviewing()) {
		sb::startPreview(range); // From the new start, so what's heard is what would be saved
	}
	uiNeedsRendering(this);
	if (display->haveOLED()) {
		renderUIsForOled();
	}
	else {
		displayText();
	}
}

void SkipbackUI::moveMarker(bool end, int64_t frame) {
	int64_t const minLength = capture.playing ? gridFrames() : kMinZoom;
	if (end) {
		range.end = std::max(frame, range.first + minLength);
	}
	else {
		range.first = std::min(frame, range.end - minLength);
	}
	movingEnd = end;
	keepInView(end ? range.end - 1 : range.first);
	rangeChanged();
}

ActionResult SkipbackUI::padAction(int32_t x, int32_t y, int32_t velocity) {
	if (!velocity) {
		return ActionResult::DEALT_WITH;
	}
	if (x >= kDisplayWidth) { // Sidebar: play the range, or stop
		if (sb::isPreviewing()) {
			sb::stopPreview();
		}
		else {
			sb::startPreview(range);
		}
		uiNeedsRendering(this);
		return ActionResult::DEALT_WITH;
	}

	// The nearer marker to the column moves to it - the start to the column's start, the end to its end
	int64_t const columnStart = viewStart + x * zoom;
	int64_t const columnMiddle = columnStart + zoom / 2;
	bool const end = std::abs(columnMiddle - range.end) < std::abs(columnMiddle - range.first);
	int64_t frame = end ? columnStart + zoom : columnStart;
	if (capture.playing) {
		frame = sb::snap(capture, frame, std::max<int64_t>(capture.barLength / 16, 1), 0);
	}
	moveMarker(end, frame);
	return ActionResult::DEALT_WITH;
}

void SkipbackUI::selectEncoderAction(int8_t offset) {
	int64_t const step = gridFrames();
	int64_t frame = (movingEnd ? range.end : range.first) + offset * step;
	if (capture.playing) {
		frame = sb::snap(capture, frame, std::max<int64_t>(capture.barLength / 16, 1), 0);
	}
	moveMarker(movingEnd, frame);
}

ActionResult SkipbackUI::horizontalEncoderAction(int32_t offset) {
	if (xEncoderHeld || Buttons::isShiftButtonPressed()) {
		// Zoom, keeping the middle where it is
		int64_t const middle = viewStart + zoom * kDisplayWidth / 2;
		int64_t const maxZoom = std::max<int64_t>(kMinZoom, (capture.numFrames * 2) / kDisplayWidth);
		zoom = (offset > 0) ? std::min(zoom << 1, std::max(maxZoom, zoom)) : std::max(zoom >> 1, kMinZoom);
		viewStart = middle - zoom * kDisplayWidth / 2;
	}
	else {
		viewStart += offset * zoom;
	}
	uiNeedsRendering(this, 0xFFFFFFFF, 0);
	return ActionResult::DEALT_WITH;
}

ActionResult SkipbackUI::buttonAction(hid::Button b, bool on, bool inCardRoutine) {
	using namespace hid::button;

	if (b == X_ENC) {
		xEncoderHeld = on;
		return ActionResult::DEALT_WITH;
	}
	// The transport keeps working, and Shift is looked at by the global handler
	if (b == PLAY || b == SHIFT) {
		return ActionResult::NOT_DEALT_WITH;
	}
	if (!on) {
		return ActionResult::DEALT_WITH;
	}
	if (inCardRoutine) {
		return ActionResult::REMIND_ME_OUTSIDE_CARD_ROUTINE;
	}
	if (b == SELECT_ENC) {
		saveAndExit();
	}
	else if (b == BACK) {
		exit();
	}
	// Anything else would act on the view underneath - Record included, which would start recording
	return ActionResult::DEALT_WITH;
}

ActionResult SkipbackUI::exitUI() {
	exit();
	return ActionResult::DEALT_WITH;
}

void SkipbackUI::exit() {
	sb::stopPreview();
	sb::unfreeze();
	close();
}

void SkipbackUI::saveAndExit() {
	sb::Saved const saved = sb::save(capture, range);
	sb::unfreeze();
	close();

	char text[48];
	bool const oled = display->haveOLED();
	if (saved.fileNumber >= 0) {
		if (oled) {
			snprintf(text, sizeof(text), saved.numNoteClips ? "Saved REC%05ld + %ld note clip%s" : "Saved REC%05ld",
			         static_cast<long>(saved.fileNumber), static_cast<long>(saved.numNoteClips),
			         saved.numNoteClips == 1 ? "" : "s");
		}
		else {
			snprintf(text, sizeof(text), "%ld", static_cast<long>(saved.fileNumber));
		}
	}
	else if (saved.numNoteClips != 0) {
		snprintf(text, sizeof(text), oled ? "%ld note clip%s" : "CLIP", static_cast<long>(saved.numNoteClips),
		         saved.numNoteClips == 1 ? "" : "s");
	}
	else {
		snprintf(text, sizeof(text), "%s", oled ? "Nothing in the range" : "NONE");
	}
	display->displayPopup(text);
}

void SkipbackUI::graphicsRoutine() {
	bool const previewing = sb::isPreviewing();
	int32_t const column = previewing ? columnOf(sb::previewFrame()) : -1;
	if (column != playheadColumn || previewing != wasPreviewing) {
		playheadColumn = column;
		if (previewing != wasPreviewing && display->haveOLED()) {
			renderUIsForOled(); // "AUD:play" / "AUD:stop"
		}
		wasPreviewing = previewing;
		uiNeedsRendering(this);
	}
}

bool SkipbackUI::renderMainPads(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
                                uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth], bool drawUndefinedArea) {
	if (!image) {
		return true;
	}
	int32_t const startColumn = columnOf(range.first);
	int32_t const endColumn = columnOf(range.end - 1);
	float const scale = (capture.maxPeak > 0) ? (kDisplayHeight / 2) / static_cast<float>(capture.maxPeak) : 0;

	for (int32_t x = 0; x < kDisplayWidth; x++) {
		int64_t const first = viewStart + x * zoom;
		int64_t const end = first + zoom;
		bool const inRange = end > range.first && first < range.end;
		bool const kept = end > 0 && first < capture.numFrames;

		RGB colour = inRange ? kRangeColour : kOutsideColour;
		int32_t background = 0; // Out of 256
		if (x == playheadColumn) {
			colour = kPlayheadColour;
			background = 40;
		}
		else if (x == startColumn) {
			colour = gui::colours::green;
			background = movingEnd ? 30 : 70;
		}
		else if (x == endColumn) {
			colour = gui::colours::red;
			background = movingEnd ? 70 : 30;
		}

		// Half the column's height, in rows, for its loudest frame - anything above silence shows at least a dot
		int32_t const loudest = kept ? sb::peak(first, end) : 0;
		float height = loudest * scale;
		if (loudest > kSilence) {
			height = std::max(height, 0.3f);
		}
		for (int32_t y = 0; y < kDisplayHeight; y++) {
			// Rows 3 and 4 are the middle, then outwards
			float const fromMiddle = std::abs(y - (kDisplayHeight - 1) / 2.0f) - 0.5f;
			int32_t amount = static_cast<int32_t>(std::clamp(height - fromMiddle, 0.0f, 1.0f) * 256);
			if (!kept && inRange && fromMiddle < 0.5f) {
				amount = 24; // Silence the range reaches into, past what's kept, to stay on the bars
			}
			amount = std::max(amount, background);
			image[y][x] = scaled(colour, amount);
		}
	}
	return true;
}

bool SkipbackUI::renderSidebar(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth],
                               uint8_t occupancyMask[][kDisplayWidth + kSideBarWidth]) {
	if (!image) {
		return true;
	}
	RGB const play = sb::isPreviewing() ? gui::colours::green : scaled(gui::colours::green, 60);
	for (int32_t y = 0; y < kDisplayHeight; y++) {
		image[y][kDisplayWidth] = gui::colours::black;
		image[y][kDisplayWidth + 1] = play;
	}
	return true;
}

void SkipbackUI::renderOLED(hid::display::oled_canvas::Canvas& canvas) {
	canvas.drawScreenTitle("Skipback");

	char line[32];
	int32_t yPixel = OLED_MAIN_TOPMOST_PIXEL + 15; // Three lines fit under the title
	double const seconds = static_cast<double>(range.length()) / kSampleRate;
	if (capture.playing) {
		int64_t const sixteenth = std::max<int64_t>(capture.barLength / 16, 1);
		auto const ticks = static_cast<int64_t>(std::llround(range.length() / capture.samplesPerTick));
		int64_t const bars = ticks / capture.barLength;
		int64_t const rest = (ticks % capture.barLength + sixteenth / 2) / sixteenth;
		if (rest == 0) {
			snprintf(line, sizeof(line), "%ld bar%s  %.2fs", static_cast<long>(bars), bars == 1 ? "" : "s", seconds);
		}
		else {
			snprintf(line, sizeof(line), "%ld bar %ld/16  %.2fs", static_cast<long>(bars), static_cast<long>(rest),
			         seconds);
		}
	}
	else {
		snprintf(line, sizeof(line), "%.2f seconds", seconds);
	}
	canvas.drawString(line, 0, yPixel, kTextSpacingX, kTextSizeYUpdated);
	yPixel += kTextSpacingY;

	if (numNotes != 0) {
		snprintf(line, sizeof(line), "%ld note%s", static_cast<long>(numNotes), numNotes == 1 ? "" : "s");
		canvas.drawString(line, 0, yPixel, kTextSpacingX, kTextSizeYUpdated);
	}
	yPixel += kTextSpacingY;

	// The audition pads play and stop the range; Back leaves, as everywhere
	canvas.drawString(sb::isPreviewing() ? "AUD:stop SEL:save" : "AUD:play SEL:save", 0, yPixel, kTextSpacingX,
	                  kTextSizeYUpdated);
}

void SkipbackUI::displayText() {
	char text[8];
	if (capture.playing) {
		auto const ticks = static_cast<int64_t>(std::llround(range.length() / capture.samplesPerTick));
		snprintf(text, sizeof(text), "%ldB", static_cast<long>((ticks + capture.barLength / 2) / capture.barLength));
	}
	else {
		snprintf(text, sizeof(text), "%.1fS", static_cast<double>(range.length()) / kSampleRate);
	}
	display->setText(text);
}
