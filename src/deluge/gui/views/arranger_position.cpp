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

// Where the song is: the bar and the time from the start of the song, shown top left on the OLED while the
// arrangement plays, and with the bar in the popup when scrolling the arranger.

#include "gui/ui/ui.h"
#include "gui/views/arranger_view.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "model/model_stack.h"
#include "model/song/song.h"
#include "modulation/automation/auto_param.h"
#include "modulation/params/param.h"
#include "modulation/params/param_node.h"
#include "playback/mode/arrangement.h"
#include "playback/playback_handler.h"
#include <cmath>
#include <cstdio>

using deluge::hid::display::OLED;

namespace {

// Seconds taken by `ticks` ticks while the tempo goes linearly (in ticks) from bpm0 to bpm1. samplesPerTickTimesBPM
// is constant: samples per tick are inversely proportional to the tempo.
double secondsForSpan(double ticks, double bpm0, double bpm1, double samplesPerTickTimesBPM) {
	double samples;
	if (std::abs(bpm1 - bpm0) < 1e-6 * bpm0) {
		samples = ticks * samplesPerTickTimesBPM / bpm0;
	}
	else {
		samples = ticks * samplesPerTickTimesBPM * std::log(bpm1 / bpm0) / (bpm1 - bpm0);
	}
	return samples / kSampleRate;
}

} // namespace

double ArrangerView::secondsAtArrangementPos(int32_t pos) {
	if (pos <= 0) {
		return 0;
	}
	float timePerTick = playbackHandler.getTimePerInternalTickFloat(); // Samples per tick at the current tempo
	double constantTempoSeconds = (double)pos * timePerTick / kSampleRate;

	// Following an external clock, the clock is the tempo: no automation to follow
	if (playbackHandler.isExternalClockActive()) {
		return constantTempoSeconds;
	}

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithThreeMainThings* modelStack = currentSong->setupModelStackWithSongAsTimelineCounter(modelStackMemory);
	ModelStackWithAutoParam* tempo =
	    currentSong->getModelStackWithParam(modelStack, deluge::modulation::params::UNPATCHED_TEMPO);
	if (!tempo || !tempo->autoParam || !tempo->autoParam->isAutomated()) {
		return constantTempoSeconds;
	}

	double bpmNow = playbackHandler.calculateBPM(timePerTick);
	if (!(bpmNow > 0)) {
		return constantTempoSeconds;
	}
	double samplesPerTickTimesBPM = timePerTick * bpmNow;
	AutoParam* param = tempo->autoParam;

	// The tempo, in BPM * 100, just after a position or (fromLeft) just before it
	auto bpmAt = [&](int32_t p, bool fromLeft) {
		int32_t value = param->getValueAtPos(p, tempo, fromLeft);
		return (value > 0) ? value / 100.0 : bpmNow;
	};

	// Between two nodes the tempo is either constant or linear, so the values at both ends of each span give it
	// exactly, whatever the automation does before its first node and after its last
	double seconds = 0;
	int32_t spanStart = 0;
	int32_t numNodes = param->nodes.getNumElements();
	for (int32_t i = 0; i <= numNodes && spanStart < pos; i++) {
		int32_t spanEnd = (i < numNodes) ? std::min(param->nodes.getElement(i)->pos, pos) : pos;
		if (spanEnd > spanStart) {
			seconds += secondsForSpan(spanEnd - spanStart, bpmAt(spanStart, false), bpmAt(spanEnd, true),
			                          samplesPerTickTimesBPM);
			spanStart = spanEnd;
		}
	}
	return seconds;
}

void ArrangerView::formatSongTime(int32_t seconds, char* buffer, int32_t bufferSize) {
	if (seconds >= 3600) {
		snprintf(buffer, bufferSize, "%d:%02d:%02d", (int)(seconds / 3600), (int)(seconds / 60 % 60),
		         (int)(seconds % 60));
	}
	else {
		snprintf(buffer, bufferSize, "%d:%02d", (int)(seconds / 60), (int)(seconds % 60));
	}
}

// Draws, updates or removes the readout top left on the normal arranger screen. Polled from graphicsRoutine: it
// only draws when the text changes (each bar and each second), as every OLED transfer costs.
void ArrangerView::updatePositionReadout(bool force) {
	if (positionScreenGeneration != OLED::getMainImageGeneration() || OLED::isPermanentPopupPresent()) {
		return; // Something else is on the screen
	}

	constexpr int32_t kTop = OLED_MAIN_TOPMOST_PIXEL;
	constexpr int32_t kY = OLED_MAIN_TOPMOST_PIXEL + 3;
	// The tempo top right clears from here (six characters and the metronome); keep a gap before it
	constexpr int32_t kRightLimit = OLED_MAIN_WIDTH_PIXELS - (kTextSpacingX * 6) - (7 + 3) - 4;
	deluge::hid::display::oled_canvas::Canvas& canvas = OLED::main;

	bool show = arrangement.hasPlaybackActive() && !playbackHandler.ticksLeftInCountIn;
	if (!show) {
		if (positionShown) {
			canvas.clearAreaExact(0, kTop, kRightLimit, kY + kTextSpacingY);
			OLED::markChanged();
			positionShown = false;
		}
		return;
	}

	int32_t pos = std::max<int32_t>(arrangement.getLivePos(), 0);
	if (!force && positionShown && pos == positionLastPos) {
		return;
	}
	positionLastPos = pos;

	int32_t bar = pos / (int32_t)currentSong->getBarLength() + 1;
	int32_t second = (int32_t)secondsAtArrangementPos(pos);
	if (!force && positionShown && bar == positionDrawnBar && second == positionDrawnSecond) {
		return;
	}

	char time[12];
	formatSongTime(second, time, sizeof(time));
	char text[32];
	snprintf(text, sizeof(text), "Bar %d  %s", (int)bar, time);
	if (canvas.getStringWidthInPixels(text, kTextSpacingY) > kRightLimit) {
		snprintf(text, sizeof(text), "%d %s", (int)bar, time);
	}

	canvas.clearAreaExact(0, kTop, kRightLimit, kY + kTextSpacingY);
	canvas.drawString(text, 0, kY, kTextSpacingX, kTextSpacingY, 0, kRightLimit);
	OLED::markChanged();
	positionShown = true;
	positionDrawnBar = bar;
	positionDrawnSecond = second;
}

// The scroll popup: the bar, beat and sixteenth at the left edge of the screen, and on the OLED the time there too
void ArrangerView::displayArrangerScrollPos() {
	if (!display->haveOLED()) {
		displayScrollPos();
		return;
	}
	int32_t pos = currentSong->xScroll[NAVIGATION_ARRANGEMENT];
	uint32_t oneBar = currentSong->getBarLength();
	uint32_t bar = pos / oneBar;
	uint32_t withinBar = pos - bar * oneBar;
	uint32_t beat = withinBar / (oneBar >> 2);
	uint32_t sixteenth = (withinBar - beat * (oneBar >> 2)) / (oneBar >> 4);

	char time[12];
	formatSongTime((int32_t)secondsAtArrangementPos(pos), time, sizeof(time));
	char text[40];
	snprintf(text, sizeof(text), "%d : %d : %d\n%s", (int)bar + 1, (int)beat + 1, (int)sixteenth + 1, time);
	display->popupTextTemporary(text);
}
