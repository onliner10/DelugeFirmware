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
#include <cstddef>
#include <cstdint>
#include <span>

class AudioClip;
class Sample;
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
		MEASURE_WAIT, ///< The AudioClip's playing, the MIDI clip's quiet; waiting for a loop end to start measuring
		MEASURING,    ///< Recording the gear's answer to test notes (pings) played across one loop
		WARMING_UP,   ///< Both playing, counting loop ends until recording starts
		RECORDING,    ///< Recording the loop
		SWAPPING,     ///< Recorded; waiting for routine() to put the AudioClip in the MIDI clip's place
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
	/// Called from AudioClip::processCurrentPos() for the AudioClip being recorded: plays the pings while measuring
	void processPings();
	/// Called regularly by the task scheduler, to move things along outside of the tick processing
	void routine();

	/// The input last chosen to freeze from, offered first next time
	AudioInputChannel lastInputChannel = AudioInputChannel::STEREO;

	/// The round-trip latency last measured from a freeze recording, in samples - used when a recording has nothing
	/// clear enough to measure it from. Unknown until then.
	int32_t lastLatencySamples = kLatencyUnknown;
	static constexpr int32_t kLatencyUnknown = INT32_MIN;

private:
	bool stillValid();
	bool safeToRestructure() const;
	void beginWarmingUp();
	void swapInPlace();
	/// Whether the MIDI clip's gear can be measured with pings: plain MIDI out, not MPE or to the Deluge's own synths
	bool canPing() const;
	void choosePings();
	void sendPing(bool on);
	/// Start the MIDI clip playing now, in step with the AudioClip, stopping anything else on its Output
	void launchMIDIClipInStep();
	/// Measure latency from the pings' recording, once it has its end margin
	void analysePings();
	/// Measure how late the recording is from its own notes, and move the AudioClip's sample start and end to make up
	/// for it. For when the pings didn't give a measurement
	void compensateLatency();
	/// Measure the latency of a recording of the given notes (positions in ticks, within a loop of loopLength ticks).
	/// latencyRange is how far either side of the loop's start it can be. Returns whether it could be measured
	bool measureLatency(Sample* sample, uint64_t loopStart, uint64_t loopEnd, std::span<uint32_t> noteTicks,
	                    int32_t* latency);
	/// Fill levels with freeze_latency's block levels of a recording, from startSample. Returns how many it did.
	size_t readRecordingLevels(Sample* sample, uint64_t startSample, std::span<uint32_t> levels);
	void showFrozenPopup(int32_t latency, bool measured);
	void releaseMeasurementSample();
	void finish(bool cancelled);

	State state = State::IDLE;
	Song* song = nullptr;
	InstrumentClip* midiClip = nullptr;
	AudioClip* audioClip = nullptr;
	int32_t loopEndsUntilRecording = 0;
	uint32_t recordingFinishedAt = 0; ///< AudioEngine::audioSampleTimer when it did

	bool midiWasPlaying = false;         ///< Before freezing stopped it to measure, so cancelling can start it again
	bool midiLaunchPending = false;      ///< routine() is to start the MIDI clip, now that measuring's done
	Sample* measurementSample = nullptr; ///< The pings' recording, held until it's been analysed
	uint32_t measurementFinishedAt = 0;
	int32_t pingNote = 60;
	int32_t pingVelocity = 100;
	int32_t pingSpacing = 0; ///< Ticks between pings
	int32_t numPings = 0;
	int32_t nextPing = 0;
	int32_t pingOffPos = 0;
	bool pingSounding = false;
	bool pingLatencyKnown = false;
	int32_t pingLatency = 0;
	int32_t recordingLatency = 0; ///< What the recording was shifted by, already
};

extern ClipFreezer clipFreezer;
