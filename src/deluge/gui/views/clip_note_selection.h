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

#include "memory/sdram_allocator.h"
#include "model/note/note.h"
#include <cstdint>
#include <vector>

class Action;
class InstrumentClip;
class ModelStackWithTimelineCounter;
class ModelStackWithNoteRow;
class NoteRow;
class Song;

/// A range of notes selected in instrument clip view (Learn + two pads), and the edits done to it: move, transpose,
/// duplicate, delete, velocity, copy and paste.
///
/// Time is in clip ticks. Rows are "visual rows" (yDisplay + yScroll): a kit row's index, or for a melodic clip the
/// on-screen row of a note's pitch (an accidental counts as the in-key row it is drawn on). A selection with allRows
/// covers every row of the clip, also those scrolled off screen.
///
/// Edits made one after another (each encoder detent) form a session: every detent is applied to the clip as it was
/// before the session, with the total offset so far, so notes the block passes over come back once it has moved on,
/// and the whole session is one undo step. Anything else that changes the clip (undo, recording, another edit) ends
/// the session; the next edit then starts a new one from the clip as it is.
class ClipNoteSelection {
public:
	struct Range {
		int32_t start = 0; ///< First tick
		int32_t end = 0;   ///< One past the last tick
		bool allRows = true;
		int32_t rowLo = 0; ///< Visual rows, inclusive. Unused if allRows
		int32_t rowHi = 0;
	};

	enum class Step { ROW, SEMITONE, OCTAVE };

	enum class Result {
		DONE,
		BLOCKED, ///< Would go past the clip's start or end, or the lowest or highest row: nothing changed
		NO_RAM,
	};

	[[nodiscard]] bool isActive() const { return clip_ != nullptr; }
	[[nodiscard]] InstrumentClip* clip() const { return clip_; }
	[[nodiscard]] const Range& range() const { return range_; }

	/// Selects range on clip, ending any session
	void select(InstrumentClip* clip, const Range& range);
	/// Narrows the rows of the selection (ending any session)
	void setRows(int32_t rowLo, int32_t rowHi);
	/// Forgets the selection. The clip keeps whatever edits were made.
	void clear();
	/// Keeps the selection but closes the undo step: the next edit starts a new one
	void endSession();
	/// Undo is about to take back the session's edits: puts the selection back where they started, and ends it
	void undoing();

	[[nodiscard]] bool containsRow(int32_t visualRow) const {
		return range_.allRows || (visualRow >= range_.rowLo && visualRow <= range_.rowHi);
	}

	Result move(ModelStackWithTimelineCounter* modelStack, int32_t ticks);
	Result transpose(ModelStackWithTimelineCounter* modelStack, Step step, int32_t direction);
	/// Puts a copy of the selected notes right next to them (direction -1: to the left) and selects that copy
	Result duplicate(ModelStackWithTimelineCounter* modelStack, int32_t direction);
	Result deleteNotes(ModelStackWithTimelineCounter* modelStack);
	/// Adds delta to the velocity of every selected note. velocityShown: the first note's new velocity, -1 if none
	Result changeVelocity(ModelStackWithTimelineCounter* modelStack, int32_t delta, int32_t* velocityShown);

	/// Copies the selected notes to the clipboard. Returns how many
	int32_t copy(ModelStackWithTimelineCounter* modelStack);
	[[nodiscard]] bool hasClipboard() const { return clipboardValid_; }
	/// Pastes the clipboard with its first tick at the selection's start and its lowest row on the selection's lowest
	/// row (or on the rows it came from, if the selection is a time range). The pasted notes become the selection,
	/// still floating: moving them doesn't eat what they pass over.
	Result paste(ModelStackWithTimelineCounter* modelStack);

	/// How many notes are selected
	[[nodiscard]] int32_t countNotes(ModelStackWithTimelineCounter* modelStack);

	/// The visual row a NoteRow is drawn on
	static int32_t visualRowOf(InstrumentClip* clip, NoteRow* noteRow, int32_t noteRowIndex);

private:
	template <typename T>
	using Vec = std::vector<T, deluge::memory::sdram_allocator<T>>;

	/// A row's notes as they were before the session
	struct Snapshot {
		int32_t key; ///< Kit: row index. Melodic: yNote
		Vec<Note> notes;
		bool isSource; ///< Notes were lifted from it
	};
	struct Floating {
		int32_t destKey;
		Note note; ///< pos is where it was before the session: it's placed at pos + dx_
	};
	struct Copied {
		int32_t relPos;
		int32_t visualRow;
		int32_t accidentalOffset; ///< yNote minus the yNote of its visual row; 0 for kits
		Note note;
	};

	bool sessionIsCurrent() const;
	Result beginSession(ModelStackWithTimelineCounter* modelStack, bool liftNotes);
	Result ensureSession(ModelStackWithTimelineCounter* modelStack);
	Result apply(ModelStackWithTimelineCounter* modelStack);
	Snapshot* findSnapshot(int32_t key);
	Result addSnapshot(ModelStackWithTimelineCounter* modelStack, int32_t key, bool isSource);
	ModelStackWithNoteRow* rowForKey(ModelStackWithTimelineCounter* modelStack, int32_t key, bool create);
	bool isKit() const;
	int32_t keyToVisual(int32_t key) const;
	/// The yNote / row index for a visual row, or -1 if there's none
	int32_t visualToKey(int32_t visualRow, int32_t accidentalOffset) const;
	void fitRowsToFloatingNotes();
	Result copyInto(ModelStackWithTimelineCounter* modelStack, Vec<Copied>& into, int32_t& width, int32_t& rowLo);
	Result pasteFrom(ModelStackWithTimelineCounter* modelStack, const Vec<Copied>& from, int32_t width, int32_t rowLo,
	                 bool dropLocks = false);

	InstrumentClip* clip_ = nullptr;
	Range range_;

	// Session
	Action* action_ = nullptr;
	Vec<Snapshot> snapshots_;
	Vec<Floating> floating_;
	int32_t dx_ = 0;
	Range rangeBeforeSession_;
	int32_t sourceStart_ = 0; ///< Range of the session's notes before it moved them
	int32_t sourceEnd_ = 0;
	bool removeSource_ = true; ///< False when the floating notes are a copy (paste, duplicate)

	// Clipboard
	Vec<Copied> clipboard_;
	int32_t clipboardWidth_ = 0;
	int32_t clipboardRowLo_ = 0;
	bool clipboardValid_ = false;
	Song* clipboardSong_ = nullptr;
};
