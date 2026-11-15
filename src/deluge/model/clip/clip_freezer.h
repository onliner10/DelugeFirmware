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

#include <cstdint>

class AudioClip;
class Clip;
class InstrumentClip;
class ModelStackWithTimelineCounter;
class Song;

/// Freezing records what a session MIDI clip plays, coming back in through an audio input, into a new AudioClip on a
/// new audio track just below it, then stops the MIDI clip. The MIDI clip stays in the song as it was, so going back to
/// it is a matter of launching it again.
///
/// The AudioClip plays in step with the MIDI clip and lets warmupPasses() loop ends go by, so the gear's tails and
/// effects settle, then records exactly one loop, from one loop end to the next. The MIDI clip is armed to stop at that
/// same loop end, so as it stops, the AudioClip starts playing back what it recorded - which, having been recorded with
/// the previous pass's tails at its start, loops seamlessly.
class ClipFreezer {
public:
	/// Whether the clip can be frozen. If not and showPopup, says why.
	static bool canFreeze(Clip* clip, bool showPopup);
	/// Start freezing a session MIDI clip. It's recorded from the input new audio tracks get (see
	/// Song::createNewAudioOutput()), or the stereo line input if that's an internal source. Starts playback if it's
	/// stopped.
	void start(InstrumentClip* clip);
	/// Stop freezing, deleting the AudioClip it was recording into
	void cancel();

	/// Whether this clip is the MIDI clip being frozen or the AudioClip being recorded for it
	bool involves(Clip const* clip) const;
	bool isRecordingInto(AudioClip const* clip) const;
	/// Called from AudioClip::posReachedEnd() in place of its usual handling, for the AudioClip being recorded
	void loopEnded(ModelStackWithTimelineCounter* modelStack);
	/// Called as an AudioClip is deleted, in case it's the one being recorded
	void forget(AudioClip const* clip);

private:
	void stop();

	enum class State : uint8_t { IDLE, WARMING_UP, RECORDING };
	State state = State::IDLE;
	Song* song = nullptr;
	InstrumentClip* midiClip = nullptr;
	AudioClip* audioClip = nullptr;
	int32_t loopEndsUntilRecording = 0;
	int32_t recordingLatency = 0; ///< Samples the recording is taken later by
};

extern ClipFreezer clipFreezer;
