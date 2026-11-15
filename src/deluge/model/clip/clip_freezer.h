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

#include "definitions_cxx.hpp"
#include <cstdint>

class AudioClip;
class Clip;
class InstrumentClip;
class ModelStackWithTimelineCounter;
class Song;

/// Freezing replaces a session MIDI clip, in place, with an AudioClip recorded from whatever the MIDI drives, coming
/// back in through an audio input. The MIDI clip is kept, hidden inside the AudioClip (AudioClip::frozen), and
/// unfreezing puts it back.
///
/// Recording: a new AudioClip, playing in sync with the MIDI clip, lets warmupPasses() loop ends go by so the gear's
/// tails and effects settle, then records exactly one loop, from one loop end to the next. The MIDI clip is armed to
/// stop at that same loop end, so as it stops, the AudioClip starts playing back what it recorded - which, having been
/// recorded with the previous pass's tails at its start, loops seamlessly. Then the two clips swap places.
class ClipFreezer {
public:
	enum class State : uint8_t {
		IDLE,
		STARTING,   ///< Waiting for the MIDI clip, and with it the AudioClip, to launch
		WARMING_UP, ///< Both playing, counting loop ends until recording starts
		RECORDING,  ///< Recording the loop
		SWAPPING,   ///< Recorded; waiting for routine() to put the AudioClip in the MIDI clip's place
	};

	/// Whether the clip can be frozen. If not and showPopup, says why.
	static bool canFreeze(Clip* clip, bool showPopup);
	/// Start freezing a session MIDI clip, recording from the given input. Starts playback if it's stopped.
	void start(InstrumentClip* clip, AudioInputChannel inputChannel);
	/// Stop freezing, leaving the MIDI clip as it was
	void cancel();
	/// Put an AudioClip's frozen MIDI clip back in its place. Returns whether it did.
	static bool unfreeze(AudioClip* clip);

	bool isBusy() const { return state != State::IDLE; }
	/// Whether this clip is the MIDI clip being frozen or the AudioClip being recorded for it
	bool involves(Clip const* clip) const;
	bool isRecordingInto(AudioClip const* clip) const;

	/// Called from AudioClip::posReachedEnd() in place of its usual handling, for the AudioClip being recorded
	void loopEnded(ModelStackWithTimelineCounter* modelStack);
	/// Called regularly by the task scheduler, to move things along outside of the tick processing
	void routine();

	/// The input last chosen to freeze from, offered first next time
	AudioInputChannel lastInputChannel = AudioInputChannel::STEREO;

private:
	bool stillValid();
	bool safeToRestructure() const;
	void beginWarmingUp();
	void swapInPlace();
	void finish(bool cancelled);

	State state = State::IDLE;
	Song* song = nullptr;
	InstrumentClip* midiClip = nullptr;
	AudioClip* audioClip = nullptr;
	int32_t loopEndsUntilRecording = 0;
};

extern ClipFreezer clipFreezer;
