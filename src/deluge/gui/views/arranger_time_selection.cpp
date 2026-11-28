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

// Time selection in the arranger, like cutting and splicing tape. Holding Learn, a pad selects its column on every
// track and a second pad spans the range between them. The selection stays after Learn is released and scrolls with
// the song; Learn + a pad later moves the nearer edge to that pad (or extends to it), so ranges longer than the screen
// are selected by scrolling or zooming in between. A tap on Learn alone, or on any pad, ends it.
//
// With a selection:
//   Shift + Save          cuts the selected time out, and the rest of the song closes up behind it
//   Shift + <> right      inserts that much empty time where the selection is (one selection length per detent)
//   Shift + <> left       cuts, like Shift + Save
//   <> held + Back        clears what's in the selection and leaves the gap
//
// Clip instances crossing an edge of the selection are cut exactly at that edge. A clip plays its loop from where its
// instance starts, so a piece that would have to start in the middle of the clip's loop can't be made without a
// modified copy of the clip. Until that exists, such an edit is refused, with a popup naming the track, and nothing
// changes. A white (arrangement-only) clip belongs to one instance, so it isn't split in two either.

#include "definitions_cxx.hpp"
#include "gui/combos/gesture_list.h"
#include "gui/ui_timer_manager.h"
#include "gui/views/arranger_view.h"
#include "gui/views/view.h"
#include "hid/buttons.h"
#include "hid/display/display.h"
#include "hid/display/oled.h"
#include "hid/led/indicator_leds.h"
#include "hid/led/pad_leds.h"
#include "model/action/action.h"
#include "model/action/action_logger.h"
#include "model/clip/clip.h"
#include "model/clip/clip_instance.h"
#include "model/output.h"
#include "model/song/song.h"
#include "playback/mode/arrangement.h"
#include "playback/playback_handler.h"
#include "storage/flash_storage.h"
#include <algorithm>
#include <cstdio>

namespace {

// Whether a clip instance's content, from delta ticks in, is what its clip plays from the start of its loop - so a
// piece of the instance can start there and sound the same
bool startsOnLoop(ClipInstance const* clipInstance, int32_t delta) {
	if (clipInstance->clip == nullptr || clipInstance->clip->loopLength <= 0) {
		return true;
	}
	return delta % clipInstance->clip->loopLength == 0;
}

bool isWhite(ClipInstance const* clipInstance) {
	return clipInstance->clip != nullptr && clipInstance->clip->isArrangementOnlyClip();
}

// A refusal explains itself, so it stays up long enough to read
void showRefusal(char const* text) {
	display->displayPopup(text);
	if (display->haveOLED()) {
		uiTimerManager.setTimer(TimerName::DISPLAY, 2500);
	}
}

char const* outputName(Output* output) {
	return output->name.isEmpty() ? "a track" : output->name.get();
}

} // namespace

std::optional<ActionResult> ArrangerView::timeSelectionPadAction(int32_t x, int32_t y, int32_t velocity) {
	if (x >= kDisplayWidth || y < 0 || y >= kDisplayHeight) {
		return std::nullopt;
	}

	// The release of a press the selection took
	if (!velocity) {
		if (timeSelectionPadsHeld[y] & (1u << x)) {
			timeSelectionPadsHeld[y] &= ~(1u << x);
			return ActionResult::DEALT_WITH;
		}
		return std::nullopt;
	}

	// A press the selection doesn't take must not have its release swallowed
	timeSelectionPadsHeld[y] &= ~(1u << x);

	if (getCurrentUI() != this) {
		return std::nullopt;
	}

	bool learnHeld = currentUIMode == UI_MODE_MIDI_LEARN && Buttons::isButtonPressed(deluge::hid::button::LEARN);
	if (learnHeld) {
		timeSelectionPadPress(x);
	}
	// Any pad pressed outside a selection gesture lets go of the selection, and does nothing else
	else if (timeSelectionActive && currentUIMode == UI_MODE_NONE && !Buttons::isShiftButtonPressed()) {
		deluge::gui::combos::used(deluge::gui::combos::gesture::arranger::EndTimeSelection);
		clearTimeSelection();
	}
	else {
		return std::nullopt;
	}

	timeSelectionPadsHeld[y] |= (1u << x);
	return ActionResult::DEALT_WITH;
}

void ArrangerView::timeSelectionPadPress(int32_t x) {
	int32_t xScroll = currentSong->xScroll[NAVIGATION_ARRANGEMENT];
	int32_t squareStart = std::max(getPosFromSquare(x, xScroll), 0_i32);
	int32_t squareEnd = getPosFromSquare(x + 1, xScroll);
	if (squareEnd <= squareStart) {
		return;
	}
	deluge::gui::combos::used(deluge::gui::combos::gesture::arranger::SelectTime);
	timeSelectionGesture.used = true;

	if (!timeSelectionGesture.hasAnchor) {
		timeSelectionGesture.hasAnchor = true;
		timeSelectionGesture.anchorStart = squareStart;
		timeSelectionGesture.anchorEnd = squareEnd;

		if (!timeSelectionActive) {
			timeSelectionStart = squareStart;
			timeSelectionEnd = squareEnd;
		}
		// With a selection already there, this pad becomes its new edge: outside it, the selection grows to it;
		// inside, the nearer edge moves to it
		else if (squareStart < timeSelectionStart) {
			timeSelectionStart = squareStart;
		}
		else if (squareEnd > timeSelectionEnd) {
			timeSelectionEnd = squareEnd;
		}
		else if (squareStart - timeSelectionStart <= timeSelectionEnd - squareEnd) {
			timeSelectionStart = squareStart;
		}
		else {
			timeSelectionEnd = squareEnd;
		}
	}

	// Another pad in the same hold of Learn: everything between the two
	else {
		timeSelectionStart = std::min(squareStart, timeSelectionGesture.anchorStart);
		timeSelectionEnd = std::max(squareEnd, timeSelectionGesture.anchorEnd);
	}

	timeSelectionActive = true;
	popupTimeSelection();
	uiNeedsRendering(this, 0xFFFFFFFF, 0);
}

void ArrangerView::timeSelectionLearnButton(bool on) {
	if (on) {
		timeSelectionGesture.used = false;
		timeSelectionGesture.hasAnchor = false;
	}
	// A tap on Learn alone ends the selection
	else if (!timeSelectionGesture.used && currentUIMode == UI_MODE_MIDI_LEARN && timeSelectionActive) {
		deluge::gui::combos::used(deluge::gui::combos::gesture::arranger::EndTimeSelection);
		clearTimeSelection();
	}
}

void ArrangerView::clearTimeSelection() {
	bool wasActive = timeSelectionActive;
	timeSelectionActive = false;
	timeSelectionGesture.hasAnchor = false;
	if (!wasActive) {
		return;
	}
	if (currentUIMode != UI_MODE_MIDI_LEARN) {
		indicator_leds::setLedState(IndicatorLED::LEARN, false);
	}
	if (getCurrentUI() == this) {
		uiNeedsRendering(this, 0xFFFFFFFF, 0);
		renderDisplay();
	}
}

// Returns whether the edit was done
bool ArrangerView::editSelectedTime(TimeEdit edit) {
	if (arrangement.hasPlaybackActive() || playbackHandler.recording != RecordingMode::OFF) {
		indicator_leds::indicateAlertOnLed(IndicatorLED::PLAY);
		showRefusal(display->haveOLED() ? "Stop playback first" : "STOP");
		return false;
	}

	int32_t length = timeSelectionEnd - timeSelectionStart;
	if (length <= 0) {
		return false;
	}

	if (edit == TimeEdit::INSERT && static_cast<int32_t>(getMaxLength()) > kMaxSequenceLength - length) {
		showRefusal(display->haveOLED() ? "Song would be too long" : "LONG");
		return false;
	}

	// Check every track before changing anything, so a refused edit leaves the song as it was and no undo step
	Output* blockingOutput = nullptr;
	if (timeEditBlockedBy(edit, &blockingOutput)) {
		if (blockingOutput != nullptr) {
			char buffer[64];
			if (display->haveOLED()) {
				snprintf(buffer, sizeof(buffer), "Can't %s the clip on %s mid-loop",
				         (edit == TimeEdit::INSERT) ? "split" : "cut", outputName(blockingOutput));
			}
			else {
				snprintf(buffer, sizeof(buffer), "LOOP");
			}
			showRefusal(buffer);
		}
		return false;
	}

	ActionType actionType = (edit == TimeEdit::CUT)      ? ActionType::ARRANGEMENT_TIME_CONTRACT
	                        : (edit == TimeEdit::INSERT) ? ActionType::ARRANGEMENT_TIME_EXPAND
	                                                     : ActionType::CLIP_INSTANCE_EDIT;
	Action* action = actionLogger.getNewAction(actionType, ActionAddition::NOT_ALLOWED);

	// Nothing may keep pointing into the instance arrays we're about to change
	lastInteractedClipInstance = nullptr;
	view.setActiveModControllableTimelineCounter(currentSong);

	for (Output* output = currentSong->firstOutput; output != nullptr; output = output->next) {
		applyTimeEdit(edit, output, action);
	}

	// Song-level automation
	switch (edit) {
	case TimeEdit::CUT:
		shiftAutomationHorizontally(timeSelectionStart, -1, -length, action);
		break;
	case TimeEdit::INSERT:
		shiftAutomationHorizontally(timeSelectionStart, 1, length, action);
		break;
	case TimeEdit::CLEAR:
		// Unless automation is only ever cleared in automation view: out, then back in as a flat stretch
		if (!FlashStorage::automationClear) {
			shiftAutomationHorizontally(timeSelectionStart, -1, -length, action);
			shiftAutomationHorizontally(timeSelectionStart, 1, length, action);
		}
		break;
	}

	// One edit, one undo step: nothing that follows (like Shift + <> without a selection) may add itself to it
	actionLogger.closeAction(actionType);

	// Keep the last touched position on the same music
	if (lastInteractedPos >= timeSelectionStart) {
		if (edit == TimeEdit::INSERT) {
			lastInteractedPos += length;
		}
		else if (edit == TimeEdit::CUT) {
			lastInteractedPos = std::max(lastInteractedPos - length, timeSelectionStart);
		}
	}

	char lengthText[24];
	int32_t barLength = currentSong->getBarLength();
	int32_t beatLength = barLength / 4;
	if (length % barLength == 0) {
		int32_t bars = length / barLength;
		snprintf(lengthText, sizeof(lengthText), "%d bar%s", static_cast<int>(bars), bars == 1 ? "" : "s");
	}
	else if (beatLength > 0 && length % beatLength == 0) {
		int32_t beats = length / beatLength;
		snprintf(lengthText, sizeof(lengthText), "%d beat%s", static_cast<int>(beats), beats == 1 ? "" : "s");
	}
	else {
		snprintf(lengthText, sizeof(lengthText), "%d ticks", static_cast<int>(length));
	}
	char buffer[40];
	if (display->haveOLED()) {
		char const* verb = (edit == TimeEdit::CUT) ? "Cut out" : (edit == TimeEdit::INSERT) ? "Inserted" : "Cleared";
		snprintf(buffer, sizeof(buffer), "%s %s", verb, lengthText);
	}
	else {
		snprintf(buffer, sizeof(buffer), (edit == TimeEdit::CUT) ? "CUT" : (edit == TimeEdit::INSERT) ? "GAP" : "CLR");
	}
	display->displayPopup(buffer);

	uiNeedsRendering(this, 0xFFFFFFFF, 0);
	return true;
}

// Whether some clip instance can't be cut exactly at the selection's edges. Allocates what a split will need.
bool ArrangerView::timeEditBlockedBy(TimeEdit edit, Output** blockingOutput) {
	int32_t start = timeSelectionStart;
	int32_t end = timeSelectionEnd;
	int32_t length = end - start;

	for (Output* output = currentSong->firstOutput; output != nullptr; output = output->next) {
		bool needsSplit = false;

		for (int32_t i = 0; i < output->clipInstances.getNumElements(); i++) {
			ClipInstance* clipInstance = output->clipInstances.getElement(i);
			int32_t pos = clipInstance->pos;
			int32_t instanceEnd = pos + clipInstance->length;

			bool ok = true;
			if (edit == TimeEdit::INSERT) {
				// Only one straddling the insertion point needs anything: its right part moves on, as its own instance
				if (pos < start && instanceEnd > start) {
					ok = startsOnLoop(clipInstance, start - pos) && !isWhite(clipInstance);
					needsSplit = true;
				}
			}
			else if (instanceEnd <= start || pos >= end) {
				// Before the selection, or after it (moving or not): fine
			}
			else if (pos >= start && instanceEnd > end) {
				// Starts inside, ends after: what's left starts where the selection ends in it
				ok = startsOnLoop(clipInstance, end - pos);
			}
			else if (pos < start && instanceEnd > end) {
				// Spans the whole selection. Cutting the selection out of it keeps one instance if the cut is a
				// whole number of its loops; otherwise it splits, the second part starting where the selection ended
				if (edit == TimeEdit::CUT && startsOnLoop(clipInstance, length)) {}
				else {
					ok = startsOnLoop(clipInstance, end - pos) && !isWhite(clipInstance);
					needsSplit = true;
				}
			}
			// Otherwise it's inside the selection, or ends in it: deleted or trimmed, always fine

			if (!ok) {
				*blockingOutput = output;
				return true;
			}
		}

		if (needsSplit && !output->clipInstances.ensureEnoughSpaceAllocated(1)) {
			display->displayError(Error::INSUFFICIENT_RAM);
			*blockingOutput = nullptr;
			return true;
		}
	}
	return false;
}

// Every step leaves the instances sorted by position with no overlaps, so each undo record finds its instance by
// position, forwards and backwards: moves to the left go left to right, moves to the right go right to left.
void ArrangerView::applyTimeEdit(TimeEdit edit, Output* output, Action* action) {
	int32_t start = timeSelectionStart;
	int32_t end = timeSelectionEnd;
	int32_t length = end - start;
	ClipInstanceVector& instances = output->clipInstances;

	if (edit == TimeEdit::INSERT) {
		for (int32_t i = instances.getNumElements() - 1; i >= 0; i--) {
			ClipInstance* clipInstance = instances.getElement(i);
			int32_t pos = clipInstance->pos;
			int32_t instanceEnd = pos + clipInstance->length;
			Clip* clip = clipInstance->clip;

			if (pos >= start) {
				clipInstance->change(action, output, pos + length, clipInstance->length, clip);
			}
			else {
				if (instanceEnd > start) {
					clipInstance->change(action, output, pos, start - pos, clip);
					insertClipInstancePiece(output, start + length, instanceEnd - start, clip, action);
				}
				break;
			}
		}
		return;
	}

	int32_t i = 0;
	while (i < instances.getNumElements()) {
		ClipInstance* clipInstance = instances.getElement(i);
		int32_t pos = clipInstance->pos;
		int32_t instanceLength = clipInstance->length;
		int32_t instanceEnd = pos + instanceLength;
		Clip* clip = clipInstance->clip;

		if (instanceEnd <= start) {
			i++;
		}
		else if (pos >= end) {
			if (edit == TimeEdit::CLEAR) {
				break;
			}
			clipInstance->change(action, output, pos - length, instanceLength, clip);
			i++;
		}
		else if (pos >= start && instanceEnd <= end) {
			deleteClipInstance(output, i, clipInstance, action); // Leaves i on the next one
		}
		else if (pos < start && instanceEnd <= end) {
			clipInstance->change(action, output, pos, start - pos, clip);
			i++;
		}
		else if (pos >= start) {
			// Starts inside, ends after: keeps the part after the selection, which closes up to its start (cut) or
			// stays where it is (clear)
			int32_t newPos = (edit == TimeEdit::CUT) ? start : end;
			clipInstance->change(action, output, newPos, instanceEnd - end, clip);
			i++;
		}
		else {
			// Spans the whole selection
			if (edit == TimeEdit::CUT && startsOnLoop(clipInstance, length)) {
				clipInstance->change(action, output, pos, instanceLength - length, clip);
				i++;
			}
			else {
				clipInstance->change(action, output, pos, start - pos, clip);
				int32_t newPos = (edit == TimeEdit::CUT) ? start : end;
				insertClipInstancePiece(output, newPos, instanceEnd - end, clip, action);
				i += 2;
			}
		}
	}
}

void ArrangerView::insertClipInstancePiece(Output* output, int32_t pos, int32_t length, Clip* clip, Action* action) {
	int32_t i = output->clipInstances.insertAtKey(pos);
	ClipInstance* clipInstance = output->clipInstances.getElement(i);
	if (clipInstance == nullptr) {
		display->displayError(Error::INSUFFICIENT_RAM); // Space was made beforehand, so this shouldn't happen
		return;
	}
	clipInstance->length = length;
	clipInstance->clip = clip;
	if (action != nullptr) {
		action->recordClipInstanceExistenceChange(output, clipInstance, ExistenceChangeType::CREATE);
	}
}

void ArrangerView::renderTimeSelection(uint32_t whichRows, RGB image[][kDisplayWidth + kSideBarWidth]) {
	if (!timeSelectionActive) {
		return;
	}
	bool inTime[kDisplayWidth];
	for (int32_t x = 0; x < kDisplayWidth; x++) {
		inTime[x] = getPosFromSquare(x) < timeSelectionEnd && getPosFromSquare(x + 1) > timeSelectionStart;
	}
	for (int32_t yDisplay = 0; yDisplay < kDisplayHeight; yDisplay++) {
		if (!(whichRows & (1 << yDisplay))) {
			continue;
		}
		for (int32_t x = 0; x < kDisplayWidth; x++) {
			RGB& colour = image[yDisplay][x];
			if (inTime[x]) {
				if (colour == deluge::gui::colours::black) {
					colour = RGB::monochrome(24); // So the empty part of the selection shows too
				}
			}
			else {
				colour = colour.dim(2);
			}
		}
	}
}

// "Bars 9-10" when the selection is whole bars, otherwise bar.beat or bar.beat.step, last unit included
void ArrangerView::formatTimeRange(int32_t start, int32_t end, char* buffer, int32_t bufferSize) {
	int32_t barLength = currentSong->getBarLength();
	int32_t beatLength = std::max(barLength / 4, 1_i32);
	int32_t stepLength = std::max(barLength / 16, 1_i32);

	if (start % barLength == 0 && end % barLength == 0) {
		int32_t first = start / barLength + 1;
		int32_t last = end / barLength;
		if (first == last) {
			snprintf(buffer, bufferSize, "Bar %d", static_cast<int>(first));
		}
		else {
			snprintf(buffer, bufferSize, "Bars %d-%d", static_cast<int>(first), static_cast<int>(last));
		}
		return;
	}

	bool beats = (start % beatLength == 0 && end % beatLength == 0);
	int32_t unit = beats ? beatLength : stepLength;
	int32_t lastStart = std::max(end - unit, start);
	auto at = [&](int32_t pos, char* out, int32_t outSize) {
		int32_t bar = pos / barLength + 1;
		int32_t beat = (pos % barLength) / beatLength + 1;
		if (beats) {
			snprintf(out, outSize, "%d.%d", static_cast<int>(bar), static_cast<int>(beat));
		}
		else {
			int32_t step = (pos % beatLength) / stepLength + 1;
			snprintf(out, outSize, "%d.%d.%d", static_cast<int>(bar), static_cast<int>(beat), static_cast<int>(step));
		}
	};
	char first[16];
	char last[16];
	at(start, first, sizeof(first));
	at(lastStart, last, sizeof(last));
	if (lastStart == start) {
		snprintf(buffer, bufferSize, "%s", first);
	}
	else {
		snprintf(buffer, bufferSize, "%s-%s", first, last);
	}
}

void ArrangerView::popupTimeSelection() {
	if (display->haveOLED()) {
		renderDisplay(); // The selection screen shows the range
		return;
	}
	char buffer[24];
	int32_t barLength = currentSong->getBarLength();
	if (timeSelectionStart % barLength == 0 && timeSelectionEnd % barLength == 0) {
		snprintf(buffer, sizeof(buffer), "%d-%d", static_cast<int>(timeSelectionStart / barLength + 1),
		         static_cast<int>(timeSelectionEnd / barLength));
	}
	else {
		formatTimeRange(timeSelectionStart, timeSelectionEnd, buffer, sizeof(buffer));
	}
	display->displayPopup(buffer);
}

void ArrangerView::renderTimeSelectionOLED(deluge::hid::display::oled_canvas::Canvas& canvas) {
	deluge::hid::display::OLED::clearMainImage();

	char range[32];
	formatTimeRange(timeSelectionStart, timeSelectionEnd, range, sizeof(range));
	int32_t yPos = OLED_MAIN_TOPMOST_PIXEL + 1;
	if (canvas.getStringWidthInPixels(range, kTextTitleSizeY) <= OLED_MAIN_WIDTH_PIXELS) {
		canvas.drawStringCentred(range, yPos, kTextTitleSpacingX, kTextTitleSizeY);
	}
	else {
		canvas.drawStringCentred(range, yPos + 1, kTextSpacingX, kTextSpacingY);
	}

	yPos += kTextTitleSizeY + 3;
	canvas.drawString("Cut out: Shift+Save", 0, yPos, kTextSpacingX, kTextSpacingY);
	yPos += kTextSpacingY;
	canvas.drawString("Add gap: Shift+<>", 0, yPos, kTextSpacingX, kTextSpacingY);
	yPos += kTextSpacingY;
	canvas.drawString("Clear:   <> + Back", 0, yPos, kTextSpacingX, kTextSpacingY);

	deluge::hid::display::OLED::markChanged();
}
