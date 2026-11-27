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

#include "model/consequence/consequence_clip_move_to_session.h"
#include "model/clip/clip.h"
#include "model/model_stack.h"
#include "model/song/song.h"
#include "playback/mode/session.h"
#include <algorithm>

Error ConsequenceClipMoveToSession::revert(TimeType time, ModelStack* model_stack) {
	Song& song = *model_stack->song;

	if (time == TimeType::BEFORE) {
		// Undo: back to an arrangement-only clip. Insert first, so a RAM failure can't leave the clip in no list.
		int32_t session_index = song.sessionClips.getIndexForClip(clip_);
		if (session_index == -1) {
			return Error::BUG;
		}
		Error error = song.arrangementOnlyClips.insertClipAtIndex(
		    clip_, std::min(arrangement_only_index_, song.arrangementOnlyClips.getNumElements()));
		if (error != Error::NONE) {
			return error;
		}

		// It may have been launched, soloed or armed in the session since
		if (clip_->soloingInSessionMode) {
			session.unsoloClip(clip_);
		}
		clip_->armState = ArmState::OFF;
		song.removeSessionClipLowLevel(clip_, session_index);

		section_ = clip_->section;
		clip_->section = 255;
		clip_->activeIfNoSolo = false; // Always the case for arrangement-only clips
	}
	else {
		// Redo
		int32_t arrangement_only_index = song.arrangementOnlyClips.getIndexForClip(clip_);
		if (arrangement_only_index == -1) {
			return Error::BUG;
		}
		Error error =
		    song.sessionClips.insertClipAtIndex(clip_, std::min(session_index_, song.sessionClips.getNumElements()));
		if (error != Error::NONE) {
			return error;
		}
		song.arrangementOnlyClips.deleteAtIndex(arrangement_only_index);
		clip_->section = section_;
	}

	return Error::NONE;
}
