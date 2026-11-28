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

#include "dsp/stereo_sample.h"
#include <cstdint>
#include <span>

class Output;

/// Skipback: the Deluge keeps the last few seconds of what it played - the master output, and the notes played live
/// on melodic instruments - in RAM, so a good moment can be kept after it happened, without having pressed record.
///
/// The Community Features setting picks how many seconds. Off by default, and while it's off nothing is allocated and
/// nothing is done. While it's on the audio costs 16-bit stereo in SDRAM (176 KB a second, taken from the sample cache)
/// and a copy of each render; nothing touches the SD card until the user saves.
///
/// Record + Select opens the skipback screen (gui/ui/skipback_ui.h), which freezes what's kept, so the user can pick
/// and audition a range. Saving it writes the audio to SAMPLES/SKIPBACK and puts it in a new audio clip, and puts the
/// notes played in that range into a new clip next to the clip of each instrument they were played on.
namespace deluge::skipback {

/// Allocates or frees the rings to match the setting. Not from the audio routine.
void applySetting();

bool isOn();

/// From the audio routine: what just went to the codec.
void feedOutput(std::span<StereoSample const> output);

/// A note played live (keyboard screen, audition pads, MIDI in) on a melodic instrument.
void noteOn(Output* output, int32_t note, int32_t velocity);
void noteOff(Output* output, int32_t note);

/// A fixed look at what's kept, while the skipback screen is open. Frames count from the oldest kept, so frame
/// numFrames - 1 is the newest. A Range may reach past either end, which is silence - so a range snapped to bars
/// still lines up with them.
struct Capture {
	int64_t numFrames;
	uint32_t newestTime;  ///< AudioEngine::audioSampleTimer at the newest frame
	bool playing;         ///< The transport was playing, so ranges go by the song's grid
	int64_t newestTick;   ///< The song's tick at the newest frame, while playing
	float samplesPerTick; ///< At the tempo when frozen
	int64_t barLength;    ///< In ticks
	int32_t maxPeak;      ///< Loudest value kept, to scale the waveform to
};

struct Range {
	int64_t first;
	int64_t end; ///< One past the last frame
	[[nodiscard]] int64_t length() const { return end - first; }
};

/// Stops the rings, so the screen and the save see one fixed picture, and works out what the screen needs. Returns
/// false, with a popup saying why, if there's nothing to show.
bool freeze(Capture& capture);
void unfreeze();

/// The loudest absolute value (16-bit) of the frames in [first, end); 0 for any outside what's kept.
int32_t peak(int64_t first, int64_t end);

/// The last phrase played: sound (or notes) back to the first gap of two seconds. Snapped out to whole bars if the
/// transport was playing, otherwise with a little room before and after.
Range suggest(Capture const& capture);

/// While playing, the frame on the song's grid (gridTicks apart) nearest frame, or the one before (direction < 0) or
/// after (direction > 0). Otherwise frame unchanged.
int64_t snap(Capture const& capture, int64_t frame, int64_t gridTicks, int32_t direction);

/// How many notes were started in the range.
int32_t countNotes(Capture const& capture, Range range);

/// Plays the range in place of the output, until it ends or stopPreview().
void startPreview(Range range);
void stopPreview();
bool isPreviewing();
int64_t previewFrame();

/// From the audio routine, per output sample while previewing: the next frame of the preview, as the 32-bit values
/// that go to the codec. Silence once it's over.
void nextPreviewFrame(int32_t& l, int32_t& r);

struct Saved {
	int32_t fileNumber = -1; ///< The WAV's number, or -1 if there was no sound to write or it failed
	bool audioClip = false;
	int32_t numNoteClips = 0;
};

/// Writes the range to a WAV and an audio clip, and its notes to clips. From the UI, not in the card routine.
Saved save(Capture const& capture, Range range);

} // namespace deluge::skipback
