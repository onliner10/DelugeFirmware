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

#include "gui/views/clip_note_selection.h"
#include "model/action/action.h"
#include "model/action/action_logger.h"
#include "model/clip/instrument_clip.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "util/exceptions.h"
#include <algorithm>
#include <limits>

namespace {
constexpr int32_t kNumYNotes = 128;
}

// ---- Selection ----

void ClipNoteSelection::select(InstrumentClip* clip, const Range& range) {
	endSession();
	clip_ = clip;
	range_ = range;
}

void ClipNoteSelection::setRows(int32_t rowLo, int32_t rowHi) {
	endSession();
	range_.allRows = false;
	range_.rowLo = std::min(rowLo, rowHi);
	range_.rowHi = std::max(rowLo, rowHi);
}

void ClipNoteSelection::clear() {
	endSession();
	clip_ = nullptr;
}

void ClipNoteSelection::endSession() {
	if (action_ && sessionIsCurrent()) {
		actionLogger.closeAction(ActionType::SELECTION_EDIT);
	}
	action_ = nullptr;
	snapshots_.clear();
	floating_.clear();
	dx_ = 0;
}

bool ClipNoteSelection::isKit() const {
	return clip_->output->type == OutputType::KIT;
}

int32_t ClipNoteSelection::visualRowOf(InstrumentClip* clip, NoteRow* noteRow, int32_t noteRowIndex) {
	if (clip->output->type == OutputType::KIT) {
		return noteRowIndex;
	}
	return clip->getYVisualFromYNote(clip->getDisplayAnchorYNote(noteRow->y, currentSong), currentSong);
}

int32_t ClipNoteSelection::keyToVisual(int32_t key) const {
	if (isKit()) {
		return key;
	}
	return clip_->getYVisualFromYNote(clip_->getDisplayAnchorYNote(key, currentSong), currentSong);
}

int32_t ClipNoteSelection::visualToKey(int32_t visualRow, int32_t accidentalOffset) const {
	if (isKit()) {
		return (visualRow >= 0 && visualRow < clip_->noteRows.getNumElements()) ? visualRow : -1;
	}
	int32_t yNote = clip_->getYNoteFromYVisual(visualRow, currentSong) + accidentalOffset;
	return (yNote >= 0 && yNote < kNumYNotes) ? yNote : -1;
}

ModelStackWithNoteRow* ClipNoteSelection::rowForKey(ModelStackWithTimelineCounter* modelStack, int32_t key,
                                                    bool create) {
	if (isKit()) {
		if (key < 0 || key >= clip_->noteRows.getNumElements()) {
			return modelStack->addNoteRow(0, nullptr);
		}
		NoteRow* noteRow = clip_->noteRows.getElement(key);
		return modelStack->addNoteRow(clip_->getNoteRowId(noteRow, key), noteRow);
	}
	if (create) {
		return clip_->getOrCreateNoteRowForYNote(key, modelStack);
	}
	return clip_->getNoteRowForYNote(key, modelStack);
}

// ---- Session ----

void ClipNoteSelection::undoing() {
	if (sessionIsCurrent()) {
		range_ = rangeBeforeSession_;
	}
	endSession();
}

bool ClipNoteSelection::sessionIsCurrent() const {
	Action* last = actionLogger.firstAction[BEFORE];
	return action_ && last == action_ && last->type == ActionType::SELECTION_EDIT && last->openForAdditions;
}

ClipNoteSelection::Snapshot* ClipNoteSelection::findSnapshot(int32_t key) {
	for (Snapshot& snapshot : snapshots_) {
		if (snapshot.key == key) {
			return &snapshot;
		}
	}
	return nullptr;
}

ClipNoteSelection::Result ClipNoteSelection::addSnapshot(ModelStackWithTimelineCounter* modelStack, int32_t key,
                                                         bool isSource) {
	ModelStackWithNoteRow* modelStackWithNoteRow = rowForKey(modelStack, key, true);
	NoteRow* noteRow = modelStackWithNoteRow->getNoteRowAllowNull();
	if (!noteRow) {
		return Result::NO_RAM;
	}
	try {
		snapshots_.push_back(Snapshot{key, {}, isSource});
		Snapshot& snapshot = snapshots_.back();
		snapshot.notes.reserve(noteRow->notes.getNumElements());
		for (int32_t i = 0; i < noteRow->notes.getNumElements(); i++) {
			snapshot.notes.push_back(*noteRow->notes.getElement(i));
		}
	} catch (deluge::exception&) {
		return Result::NO_RAM;
	}
	if (action_) {
		action_->recordNoteArrayChangeIfNotAlreadySnapshotted(clip_, modelStackWithNoteRow->noteRowId, &noteRow->notes,
		                                                      false);
	}
	return Result::DONE;
}

ClipNoteSelection::Result ClipNoteSelection::beginSession(ModelStackWithTimelineCounter* modelStack, bool liftNotes) {
	endSession();
	action_ = actionLogger.getNewAction(ActionType::SELECTION_EDIT, ActionAddition::NOT_ALLOWED);
	rangeBeforeSession_ = range_;
	sourceStart_ = range_.start;
	sourceEnd_ = range_.end;
	removeSource_ = liftNotes;
	if (!liftNotes) {
		return Result::DONE;
	}

	for (int32_t i = 0; i < clip_->noteRows.getNumElements(); i++) {
		NoteRow* noteRow = clip_->noteRows.getElement(i);
		if (!containsRow(visualRowOf(clip_, noteRow, i))) {
			continue;
		}
		int32_t first = noteRow->notes.search(range_.start, GREATER_OR_EQUAL);
		int32_t last = noteRow->notes.search(range_.end, GREATER_OR_EQUAL);
		if (first >= last) {
			continue;
		}
		int32_t key = isKit() ? i : noteRow->y;
		try {
			for (int32_t n = first; n < last; n++) {
				floating_.push_back(Floating{key, *noteRow->notes.getElement(n)});
			}
		} catch (deluge::exception&) {
			endSession();
			return Result::NO_RAM;
		}
		Result result = addSnapshot(modelStack, key, true);
		if (result != Result::DONE) {
			endSession();
			return result;
		}
	}
	return Result::DONE;
}

ClipNoteSelection::Result ClipNoteSelection::ensureSession(ModelStackWithTimelineCounter* modelStack) {
	if (sessionIsCurrent()) {
		return Result::DONE;
	}
	return beginSession(modelStack, true);
}

// Writes every row the session has touched: as it was before the session, minus the lifted notes, minus whatever is
// under where they land now, plus the lifted notes at their new place.
ClipNoteSelection::Result ClipNoteSelection::apply(ModelStackWithTimelineCounter* modelStack) {
	for (const Floating& floating : floating_) {
		if (!findSnapshot(floating.destKey)) {
			Result result = addSnapshot(modelStack, floating.destKey, false);
			if (result != Result::DONE) {
				return result;
			}
		}
	}

	bool playing = playbackHandler.isEitherClockActive() && currentSong->isClipActive(clip_);
	int32_t destStart = sourceStart_ + dx_;
	int32_t destEnd = sourceEnd_ + dx_;

	Vec<std::pair<Note, bool>> notes; // bool: is a placed (floating) note, which wins over one at the same tick
	for (Snapshot& snapshot : snapshots_) {
		// Created if need be: something may have deleted the row since, if it was empty
		ModelStackWithNoteRow* modelStackWithNoteRow = rowForKey(modelStack, snapshot.key, true);
		NoteRow* noteRow = modelStackWithNoteRow->getNoteRowAllowNull();
		if (!noteRow) {
			continue;
		}
		int32_t rowLength = modelStackWithNoteRow->getLoopLength();
		bool isDest = std::any_of(floating_.begin(), floating_.end(),
		                          [&](const Floating& floating) { return floating.destKey == snapshot.key; });

		notes.clear();
		try {
			for (const Note& note : snapshot.notes) {
				if (removeSource_ && snapshot.isSource && note.pos >= sourceStart_ && note.pos < sourceEnd_) {
					continue;
				}
				if (isDest && note.pos >= destStart && note.pos < destEnd) {
					continue;
				}
				notes.emplace_back(note, false);
			}
			for (const Floating& floating : floating_) {
				if (floating.destKey != snapshot.key) {
					continue;
				}
				Note note = floating.note;
				note.pos += dx_;
				if (note.pos < 0 || note.pos >= rowLength) {
					continue;
				}
				notes.emplace_back(note, true);
			}
		} catch (deluge::exception&) {
			return Result::NO_RAM;
		}

		// Placed notes first at equal ticks, then drop the others at that tick
		std::stable_sort(notes.begin(), notes.end(), [](const auto& a, const auto& b) {
			if (a.first.pos != b.first.pos) {
				return a.first.pos < b.first.pos;
			}
			return a.second && !b.second;
		});
		notes.erase(std::unique(notes.begin(), notes.end(),
		                        [](const auto& a, const auto& b) { return a.first.pos == b.first.pos; }),
		            notes.end());

		// No note may run into the next one, including the last one wrapping round into the first
		int32_t numNotes = static_cast<int32_t>(notes.size());
		for (int32_t i = 0; i < numNotes; i++) {
			int32_t nextPos = (i + 1 < numNotes) ? notes[i + 1].first.pos : notes[0].first.pos + rowLength;
			Note& note = notes[i].first;
			note.length = std::max<int32_t>(1, std::min(note.length, nextPos - note.pos));
		}

		NoteVector newNotes;
		if (numNotes && newNotes.insertAtIndex(0, numNotes) != Error::NONE) {
			return Result::NO_RAM;
		}
		for (int32_t i = 0; i < numNotes; i++) {
			*newNotes.getElement(i) = notes[i].first;
		}
		if (playing) {
			noteRow->stopCurrentlyPlayingNote(modelStackWithNoteRow);
		}
		noteRow->notes.swapStateWith(&newNotes);
	}

	if (playing) {
		clip_->expectEvent();
	}
	return Result::DONE;
}

void ClipNoteSelection::fitRowsToFloatingNotes() {
	if (range_.allRows || floating_.empty()) {
		return;
	}
	int32_t lo = std::numeric_limits<int32_t>::max();
	int32_t hi = std::numeric_limits<int32_t>::min();
	for (const Floating& floating : floating_) {
		int32_t visual = keyToVisual(floating.destKey);
		lo = std::min(lo, visual);
		hi = std::max(hi, visual);
	}
	range_.rowLo = lo;
	range_.rowHi = hi;
}

// ---- Edits ----

ClipNoteSelection::Result ClipNoteSelection::move(ModelStackWithTimelineCounter* modelStack, int32_t ticks) {
	if (range_.start + ticks < 0 || range_.end + ticks > (int32_t)clip_->loopLength) {
		return Result::BLOCKED;
	}
	Result result = ensureSession(modelStack);
	if (result != Result::DONE) {
		return result;
	}
	dx_ += ticks;
	range_.start += ticks;
	range_.end += ticks;
	return apply(modelStack);
}

ClipNoteSelection::Result ClipNoteSelection::transpose(ModelStackWithTimelineCounter* modelStack, Step step,
                                                       int32_t direction) {
	Result result = ensureSession(modelStack);
	if (result != Result::DONE) {
		return result;
	}

	// The key a note on row key goes to, -1 if there's none
	auto moveKey = [&](int32_t key) -> int32_t {
		if (isKit()) {
			return visualToKey(key + direction, 0);
		}
		if (step == Step::ROW) {
			int32_t anchor = clip_->getDisplayAnchorYNote(key, currentSong);
			return visualToKey(clip_->getYVisualFromYNote(anchor, currentSong) + direction, key - anchor);
		}
		int32_t moved = key + direction * (step == Step::OCTAVE ? 12 : 1);
		return (moved >= 0 && moved < kNumYNotes) ? moved : -1;
	};

	if (floating_.empty()) {
		// Nothing to carry: just move the rows of the selection
		if (!range_.allRows && (isKit() || step == Step::ROW)) {
			if (isKit()
			    && (range_.rowLo + direction < 0 || range_.rowHi + direction >= clip_->noteRows.getNumElements())) {
				return Result::BLOCKED;
			}
			range_.rowLo += direction;
			range_.rowHi += direction;
		}
		return Result::DONE;
	}

	// Every note must be able to go, or none goes
	Vec<int32_t> newKeys;
	try {
		newKeys.reserve(floating_.size());
		for (const Floating& floating : floating_) {
			int32_t newKey = moveKey(floating.destKey);
			if (newKey < 0) {
				return Result::BLOCKED;
			}
			newKeys.push_back(newKey);
		}
	} catch (deluge::exception&) {
		return Result::NO_RAM;
	}
	for (size_t i = 0; i < floating_.size(); i++) {
		floating_[i].destKey = newKeys[i];
		// A note moved to another row by hand is a new note: its accidental lineage starts again
		floating_[i].note.anchorMarker = AnchorMarker::derived();
	}
	if (!range_.allRows) {
		if (isKit() || step == Step::ROW) {
			range_.rowLo += direction;
			range_.rowHi += direction;
		}
		else {
			fitRowsToFloatingNotes();
		}
	}
	return apply(modelStack);
}

ClipNoteSelection::Result ClipNoteSelection::deleteNotes(ModelStackWithTimelineCounter* modelStack) {
	Result result = ensureSession(modelStack);
	if (result != Result::DONE) {
		return result;
	}
	floating_.clear();
	result = apply(modelStack);
	endSession();
	return result;
}

ClipNoteSelection::Result ClipNoteSelection::changeVelocity(ModelStackWithTimelineCounter* modelStack, int32_t delta,
                                                            int32_t* velocityShown) {
	*velocityShown = -1;
	Result result = ensureSession(modelStack);
	if (result != Result::DONE) {
		return result;
	}
	for (Floating& floating : floating_) {
		floating.note.velocity = std::clamp<int32_t>(floating.note.velocity + delta, 1, 127);
	}
	if (!floating_.empty()) {
		*velocityShown = floating_.front().note.velocity;
	}
	return apply(modelStack);
}

ClipNoteSelection::Result ClipNoteSelection::duplicate(ModelStackWithTimelineCounter* modelStack, int32_t direction) {
	int32_t width = range_.end - range_.start;
	int32_t newStart = range_.start + direction * width;
	if (newStart < 0 || newStart + width > (int32_t)clip_->loopLength) {
		return Result::BLOCKED;
	}
	Vec<Copied> copied;
	int32_t copiedWidth;
	int32_t rowLo;
	Result result = copyInto(modelStack, copied, copiedWidth, rowLo);
	if (result != Result::DONE) {
		return result;
	}
	endSession();
	Range original = range_;
	range_.start = newStart;
	range_.end = newStart + width;
	result = pasteFrom(modelStack, copied, copiedWidth, rowLo);
	rangeBeforeSession_ = original; // Undo takes the selection back to what was duplicated
	return result;
}

// ---- Clipboard ----

ClipNoteSelection::Result ClipNoteSelection::copyInto(ModelStackWithTimelineCounter* modelStack, Vec<Copied>& into,
                                                      int32_t& width, int32_t& rowLo) {
	into.clear();
	width = range_.end - range_.start;
	rowLo = range_.allRows ? std::numeric_limits<int32_t>::max() : range_.rowLo;
	try {
		for (int32_t i = 0; i < clip_->noteRows.getNumElements(); i++) {
			NoteRow* noteRow = clip_->noteRows.getElement(i);
			int32_t visual = visualRowOf(clip_, noteRow, i);
			if (!containsRow(visual)) {
				continue;
			}
			int32_t first = noteRow->notes.search(range_.start, GREATER_OR_EQUAL);
			int32_t last = noteRow->notes.search(range_.end, GREATER_OR_EQUAL);
			int32_t accidentalOffset = isKit() ? 0 : noteRow->y - clip_->getDisplayAnchorYNote(noteRow->y, currentSong);
			for (int32_t n = first; n < last; n++) {
				Note* note = noteRow->notes.getElement(n);
				into.push_back(Copied{note->pos - range_.start, visual, accidentalOffset, *note});
				if (range_.allRows) {
					rowLo = std::min(rowLo, visual);
				}
			}
		}
	} catch (deluge::exception&) {
		into.clear();
		return Result::NO_RAM;
	}
	if (into.empty()) {
		rowLo = range_.allRows ? 0 : range_.rowLo;
	}
	return Result::DONE;
}

int32_t ClipNoteSelection::copy(ModelStackWithTimelineCounter* modelStack) {
	clipboardValid_ = (copyInto(modelStack, clipboard_, clipboardWidth_, clipboardRowLo_) == Result::DONE);
	clipboardSong_ = currentSong;
	return clipboardValid_ ? static_cast<int32_t>(clipboard_.size()) : 0;
}

ClipNoteSelection::Result ClipNoteSelection::paste(ModelStackWithTimelineCounter* modelStack) {
	if (!clipboardValid_) {
		return Result::DONE;
	}
	int32_t width = std::min<int32_t>(clipboardWidth_, (int32_t)clip_->loopLength - range_.start);
	if (width <= 0) {
		return Result::BLOCKED;
	}
	range_.end = range_.start + width;
	// Parameter locks are indices into the song's pool: from another song they'd point at unrelated locks
	return pasteFrom(modelStack, clipboard_, clipboardWidth_, clipboardRowLo_, clipboardSong_ != currentSong);
}

ClipNoteSelection::Result ClipNoteSelection::pasteFrom(ModelStackWithTimelineCounter* modelStack,
                                                       const Vec<Copied>& from, int32_t width, int32_t rowLo,
                                                       bool dropLocks) {
	Result result = beginSession(modelStack, false);
	if (result != Result::DONE) {
		return result;
	}
	sourceEnd_ = std::min<int32_t>(range_.start + width, clip_->loopLength);
	range_.end = sourceEnd_;

	bool keepRows = range_.allRows;
	try {
		for (const Copied& copied : from) {
			int32_t pos = range_.start + copied.relPos;
			if (pos >= (int32_t)clip_->loopLength) {
				continue;
			}
			int32_t visual = keepRows ? copied.visualRow : range_.rowLo + (copied.visualRow - rowLo);
			int32_t key = visualToKey(visual, isKit() ? 0 : copied.accidentalOffset);
			if (key < 0) {
				continue;
			}
			Note note = copied.note;
			note.pos = pos;
			if (dropLocks) {
				note.lockSet = 0;
			}
			if (!keepRows) {
				note.anchorMarker = AnchorMarker::derived();
			}
			floating_.push_back(Floating{key, note});
		}
	} catch (deluge::exception&) {
		endSession();
		return Result::NO_RAM;
	}
	fitRowsToFloatingNotes();
	return apply(modelStack);
}

int32_t ClipNoteSelection::countNotes(ModelStackWithTimelineCounter* modelStack) {
	if (sessionIsCurrent()) {
		return static_cast<int32_t>(floating_.size());
	}
	int32_t count = 0;
	for (int32_t i = 0; i < clip_->noteRows.getNumElements(); i++) {
		NoteRow* noteRow = clip_->noteRows.getElement(i);
		if (containsRow(visualRowOf(clip_, noteRow, i))) {
			count += noteRow->notes.search(range_.end, GREATER_OR_EQUAL)
			         - noteRow->notes.search(range_.start, GREATER_OR_EQUAL);
		}
	}
	return count;
}
