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

#include "processing/skipback/skipback.h"
#include "definitions_cxx.hpp"
#include "gui/views/session_view.h"
#include "hid/display/display.h"
#include "lib/printf.h"
#include "memory/general_memory_allocator.h"
#include "model/action/action_logger.h"
#include "model/clip/audio_clip.h"
#include "model/clip/instrument_clip.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/output.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/storage_manager.h"
#include "util/d_string.h"
#include <algorithm>
#include <arm_neon.h>
#include <array>
#include <cmath>
#include <cstring>

extern "C" {
#include "drivers/ssi/ssi.h" // The line input's ring
}

namespace deluge::skipback {
namespace {

constexpr uint32_t kMaxSeconds = 60;

// The audio ring is made of chunks rather than one block, so it can be allocated while a song's samples are scattered
// through the stealable region
constexpr uint32_t kChunkFramesMagnitude = 14;
constexpr uint32_t kChunkFrames = 1 << kChunkFramesMagnitude; // 64 KiB of 16-bit stereo
constexpr uint32_t kMaxChunks = (kMaxSeconds * kSampleRate + kChunkFrames - 1) / kChunkFrames;

struct Frame {
	int16_t l;
	int16_t r;
};
// feedOutput() treats both as plain interleaved L R values
static_assert(sizeof(Frame) == 2 * sizeof(int16_t));
static_assert(sizeof(StereoSample) == 2 * sizeof(int32_t));

std::array<Frame*, kMaxChunks> chunks{};
uint32_t numChunks = 0;
uint32_t capacityFrames = 0;
uint32_t writeFrame = 0;   // Where the next frame goes, 0 to capacityFrames - 1
uint32_t framesFilled = 0; // How many frames hold audio, up to capacityFrames
uint32_t lastFeedTime = 0; // AudioEngine::audioSampleTimer just after the newest frame was written
bool frozen = false;       // While the skipback screen is open, so what it shows stays put

struct NoteEvent {
	uint32_t time; // AudioEngine::audioSampleTimer
	Output* output;
	int16_t note;
	uint8_t velocity; // 0 for a note-off
};
constexpr uint32_t kNumNoteEvents = 2048; // 24 KiB
NoteEvent* noteEvents = nullptr;
uint32_t noteWrite = 0;
uint32_t noteCount = 0;

// Quieter than this (16-bit, about -72 dBFS) counts as silence
constexpr int32_t kSilence = 8;
constexpr int64_t kPreRoll = kSampleRate / 100; // 10 ms kept before the first sound when not on the grid
constexpr int64_t kTail = kSampleRate / 2;      // 500 ms kept after the last, for release tails under the threshold
constexpr int64_t kPhraseGap = 2 * kSampleRate; // This much silence ends a phrase

// While frozen: the loudest value of each block of frames, so the waveform and the phrase search needn't read
// every frame each time
constexpr uint32_t kBlockFramesMagnitude = 8;
constexpr uint32_t kBlockFrames = 1 << kBlockFramesMagnitude;
uint16_t* blockPeaks = nullptr;
uint32_t numBlocks = 0;
Capture frozenCapture{};

// Whose audio is kept (Community Features > Skipback Source), as the setting's values have it
enum class Source : uint8_t { TRACK = 0, MASTER = 1, LINE_IN = 2 };
Source source = Source::TRACK;
bool trackMode = true;         // source == Source::TRACK
bool onlyWhilePlaying = false; // Community Features > Skipback Record: While playing
bool wasPlaying = false;       // At the last feed

// The song's position at the newest frame kept, as the feed saw it, so a range can go by the grid even once the
// transport has stopped
bool feedPlaying = false;
int64_t feedTick = 0;
float feedSamplesPerTick = 0;
uint32_t feedTickTime = 0;       // AudioEngine::audioSampleTimer at feedTick
Clip* lastCurrentClip = nullptr; // The current clip the track was last picked up from; only compared with
bool trackRendered = false;      // trackMemory holds this render of trackSource
int32_t songGain = 0;            // The song's volume this render, as setSongGain() has it
alignas(CACHE_LINE_SIZE) std::array<StereoSample, SSI_TX_BUFFER_NUM_SAMPLES> trackMemory;

// The preview, in frames as Capture counts them
bool previewing = false;
int64_t previewPos = 0;
int64_t previewEnd = 0;

/// Non-stealable memory in the stealable region, so the rings never take the external region, which holds the
/// song's objects and fills up on big songs. Costs sample cache instead, which is what the setting says it costs
void* allocInStealableRegion(uint32_t size) {
	auto& allocator = GeneralMemoryAllocator::get();
	if (allocator.lock) {
		return nullptr;
	}
	allocator.lock = true;
	void* address = allocator.regions[MEMORY_REGION_STEALABLE].alloc(size, false, nullptr);
	allocator.lock = false;
	return address;
}

/// Forgets what's kept, as when another track starts being kept
void restart() {
	writeFrame = 0;
	framesFilled = 0;
	noteWrite = 0;
	noteCount = 0;
}

void setTrackSource(Output* output) {
	if (output != trackSource) {
		trackSource = output;
		restart();
	}
}

void freeAll() {
	uint32_t const had = numChunks;
	numChunks = 0; // First, so the audio routine stops writing
	trackSource = nullptr;
	lastCurrentClip = nullptr;
	capacityFrames = 0;
	writeFrame = 0;
	framesFilled = 0;
	for (uint32_t i = 0; i < had; i++) {
		delugeDealloc(chunks[i]);
		chunks[i] = nullptr;
	}
	if (noteEvents != nullptr) {
		NoteEvent* const events = noteEvents;
		noteEvents = nullptr;
		delugeDealloc(events);
	}
	noteWrite = 0;
	noteCount = 0;
}

[[gnu::always_inline]] inline Frame& frameAt(uint32_t ringIndex) {
	return chunks[ringIndex >> kChunkFramesMagnitude][ringIndex & (kChunkFrames - 1)];
}

/// The ring index of the k-th oldest frame
[[gnu::always_inline]] inline uint32_t ringIndexOf(uint32_t k) {
	uint32_t index = writeFrame + capacityFrames - framesFilled + k;
	if (index >= capacityFrames) {
		index -= capacityFrames;
	}
	return index;
}

[[gnu::always_inline]] inline int32_t frameLoudness(Frame const& frame) {
	return std::max(std::abs(static_cast<int32_t>(frame.l)), std::abs(static_cast<int32_t>(frame.r)));
}

/// The loudest value of frames [first, end), which must be within what's kept. Straight from the frames
int32_t scanPeak(uint32_t first, uint32_t end) {
	int32_t loudest = 0;
	for (uint32_t k = first; k < end;) {
		uint32_t const ringIndex = ringIndexOf(k);
		uint32_t const run = std::min(end - k, kChunkFrames - (ringIndex & (kChunkFrames - 1)));
		Frame const* frames = &frameAt(ringIndex);
		for (uint32_t j = 0; j < run; j++) {
			loudest = std::max(loudest, frameLoudness(frames[j]));
		}
		k += run;
	}
	return loudest;
}

/// Works out blockPeaks, letting the audio routine run between chunks: 60 seconds is too much to read in one go
bool buildBlockPeaks() {
	numBlocks = (framesFilled + kBlockFrames - 1) >> kBlockFramesMagnitude;
	if (numBlocks == 0) {
		return true;
	}
	blockPeaks = static_cast<uint16_t*>(allocInStealableRegion(numBlocks * sizeof(uint16_t)));
	if (blockPeaks == nullptr) {
		numBlocks = 0;
		return false;
	}
	for (uint32_t b = 0; b < numBlocks; b++) {
		uint32_t const first = b << kBlockFramesMagnitude;
		blockPeaks[b] = static_cast<uint16_t>(scanPeak(first, std::min(first + kBlockFrames, framesFilled)));
		if ((b & ((kChunkFrames >> kBlockFramesMagnitude) - 1)) == 0) {
			AudioEngine::routineWithClusterLoading();
		}
	}
	return true;
}

void addNoteEvent(Output* output, int32_t note, int32_t velocity) {
	if (noteEvents == nullptr || frozen || (onlyWhilePlaying && !playbackHandler.isEitherClockActive())) {
		return;
	}
	noteEvents[noteWrite] = {
	    .time = AudioEngine::audioSampleTimer,
	    .output = output,
	    .note = static_cast<int16_t>(note),
	    .velocity = static_cast<uint8_t>(velocity),
	};
	noteWrite = (noteWrite + 1) & (kNumNoteEvents - 1);
	noteCount = std::min(noteCount + 1, kNumNoteEvents);
}

NoteEvent const& noteEventAt(uint32_t k) { // k-th oldest
	return noteEvents[(noteWrite + kNumNoteEvents - noteCount + k) & (kNumNoteEvents - 1)];
}

/// Where a note event falls, in frames as Capture counts them
int64_t frameOfTime(Capture const& capture, uint32_t time) {
	return capture.numFrames - 1 + static_cast<int32_t>(time - capture.newestTime);
}

double tickOfFrame(Capture const& capture, int64_t frame) {
	return static_cast<double>(capture.newestTick)
	       - static_cast<double>(capture.numFrames - 1 - frame) / capture.samplesPerTick;
}

int64_t frameOfTick(Capture const& capture, double tick) {
	return capture.numFrames - 1
	       - static_cast<int64_t>(
	           std::llround((static_cast<double>(capture.newestTick) - tick) * capture.samplesPerTick));
}

bool isInSong(Output* output) {
	for (Output* o = currentSong->firstOutput; o != nullptr; o = o->next) {
		if (o == output) {
			return true;
		}
	}
	return false;
}

/// The lowest grid section none of the output's session clips is in, so the new clip gets a pad of its own there
int32_t freeSectionFor(Output* output) {
	for (int32_t section = 0; section < kMaxNumSections; section++) {
		bool used = false;
		for (int32_t i = 0; i < currentSong->sessionClips.getNumElements(); i++) {
			Clip* clip = currentSong->sessionClips.getClipAtIndex(i);
			if (clip->output == output && clip->section == section) {
				used = true;
				break;
			}
		}
		if (!used) {
			return section;
		}
	}
	return 0;
}

/// In the rows layout, scrolls the song view so the clip at this index is on screen
void showRow(int32_t index) {
	if (currentSong->sessionLayout == SessionLayoutType::SessionLayoutTypeGrid) {
		return;
	}
	if (index - currentSong->songViewYScroll >= kDisplayHeight) {
		currentSong->songViewYScroll = index - kDisplayHeight + 1;
	}
	else if (index < currentSong->songViewYScroll) {
		currentSong->songViewYScroll = index;
	}
}

/// The clip length for a range: what it lasts at the frozen tempo, on the 1/16 grid if the transport was playing
int32_t loopLengthFor(Capture const& capture, Range range) {
	auto ticks = static_cast<int64_t>(std::llround(static_cast<double>(range.length()) / capture.samplesPerTick));
	if (capture.playing) {
		int64_t const grid = std::max<int64_t>(capture.barLength / 16, 1);
		ticks = ((ticks + grid / 2) / grid) * grid;
	}
	return static_cast<int32_t>(std::clamp<int64_t>(ticks, 1, kMaxSequenceLength));
}

/// Puts the notes one instrument started in the range into a new clip next to its active one. Returns the clip's
/// index in the session, or -1 if it made none
int32_t captureNotesFor(Output* output, Capture const& capture, Range range, int32_t loopLength) {
	Clip* source = output->getActiveClip();
	if (source == nullptr || source->type != ClipType::INSTRUMENT) {
		return -1;
	}

	// Pair each note-on with the next note-off of that note; a note still held ends at the range's end
	struct CapturedNote {
		int32_t pos;
		int32_t length;
		int16_t note;
		uint8_t velocity;
	};
	constexpr int32_t kMaxNotes = 512;
	auto* notes = static_cast<CapturedNote*>(allocInStealableRegion(kMaxNotes * sizeof(CapturedNote)));
	if (notes == nullptr) {
		return -1;
	}
	auto ticksFromStart = [&](int64_t frame) {
		return static_cast<int32_t>(std::llround(static_cast<double>(frame - range.first) / capture.samplesPerTick));
	};
	int32_t numNotes = 0;
	for (uint32_t k = 0; k < noteCount && numNotes < kMaxNotes; k++) {
		NoteEvent const& on = noteEventAt(k);
		int64_t const onFrame = frameOfTime(capture, on.time);
		if (on.output != output || on.velocity == 0 || onFrame < range.first || onFrame >= range.end) {
			continue;
		}
		int64_t offFrame = range.end;
		for (uint32_t j = k + 1; j < noteCount; j++) {
			NoteEvent const& off = noteEventAt(j);
			if (off.output == output && off.note == on.note) {
				// A note-off, or a new note-on of the same note, which also ends this one
				offFrame = std::min(frameOfTime(capture, off.time), range.end);
				break;
			}
		}
		int32_t const pos = std::min(ticksFromStart(onFrame), loopLength - 1);
		notes[numNotes++] = {
		    .pos = pos,
		    .length = std::max<int32_t>(ticksFromStart(offFrame) - pos, 1),
		    .note = on.note,
		    .velocity = on.velocity,
		};
	}
	if (numNotes == 0) {
		delugeDealloc(notes);
		return -1;
	}

	// A copy of the instrument's clip, emptied - as a new clip in that track on the grid is made, so it sounds the same
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack =
	    setupModelStackWithSong(modelStackMemory, currentSong)->addTimelineCounter(source);
	if (source->clone(modelStack, false) != Error::NONE) {
		delugeDealloc(notes);
		display->displayError(Error::INSUFFICIENT_RAM);
		return -1;
	}
	auto* clip = static_cast<InstrumentClip*>(modelStack->getTimelineCounter());
	clip->clear(nullptr, modelStack, true, true);
	clip->name.clear();
	clip->soloingInSessionMode = false;
	clip->wasActiveBefore = false;
	clip->isPendingOverdub = false;
	clip->isUnfinishedAutoOverdub = false;
	clip->armState = ArmState::OFF;
	clip->activeIfNoSolo = false; // Launched by the user, when they want it
	clip->onKeyboardScreen = false;
	clip->loopLength = loopLength;
	// A section of its own: on the grid that's a pad of its own in the track's column, and in rows it keeps launching
	// its source's section from starting it too - an instrument plays one clip at a time
	clip->section = freeSectionFor(output);

	for (int32_t i = 0; i < numNotes; i++) {
		CapturedNote const& n = notes[i];
		ModelStackWithNoteRow* modelStackWithNoteRow = clip->getOrCreateNoteRowForYNote(n.note, modelStack);
		NoteRow* noteRow = modelStackWithNoteRow->getNoteRowAllowNull();
		if (noteRow == nullptr) {
			continue;
		}
		noteRow->attemptNoteAdd(n.pos, std::min(n.length, loopLength - n.pos), n.velocity, kNumProbabilityValues,
		                        noteRow->getDefaultIterance(), noteRow->getDefaultFill(modelStackWithNoteRow),
		                        modelStackWithNoteRow, nullptr);
	}
	delugeDealloc(notes);

	int32_t index = currentSong->sessionClips.getIndexForClip(source);
	index = (index < 0) ? 0 : index + 1;
	if (currentSong->sessionClips.insertClipAtIndex(clip, index) != Error::NONE) {
		clip->~InstrumentClip();
		delugeDealloc(clip);
		display->displayError(Error::INSUFFICIENT_RAM);
		return -1;
	}
	return index;
}

void writeLE16(uint8_t*& p, uint16_t v) {
	*p++ = v & 0xFF;
	*p++ = v >> 8;
}
void writeLE32(uint8_t*& p, uint32_t v) {
	writeLE16(p, v & 0xFFFF);
	writeLE16(p, v >> 16);
}

/// Writes the range as a 16-bit stereo WAV, silence where it reaches past what's kept. Returns the file's number,
/// or -1 on failure, and its path in filePath
int32_t writeWav(Range range, String& filePath) {
	String tempFilePath;
	String noSongName;
	uint32_t number = 0;
	Error error = audioFileManager.getUnusedAudioRecordingFilePath(
	    filePath, &tempFilePath, AudioRecordingFolder::SKIPBACK, &number, nullptr, &noSongName);
	if (error != Error::NONE) {
		display->displayError(error);
		return -1;
	}
	auto created = StorageManager::createFile(filePath.get(), false);
	if (!created) {
		display->displayError(created.error());
		return -1;
	}
	FatFS::File& file = *created; // Not a copy: the copy's destructor would close the file a second time

	auto const dataBytes = static_cast<uint32_t>(range.length() * sizeof(Frame));
	std::array<uint8_t, 44> header;
	uint8_t* p = header.data();
	std::memcpy(p, "RIFF", 4);
	p += 4;
	writeLE32(p, 36 + dataBytes);
	std::memcpy(p, "WAVEfmt ", 8);
	p += 8;
	writeLE32(p, 16);                          // fmt chunk size
	writeLE16(p, 1);                           // PCM
	writeLE16(p, 2);                           // Channels
	writeLE32(p, kSampleRate);                 // Sample rate
	writeLE32(p, kSampleRate * sizeof(Frame)); // Byte rate
	writeLE16(p, sizeof(Frame));               // Block align
	writeLE16(p, 16);                          // Bits per sample
	std::memcpy(p, "data", 4);
	p += 4;
	writeLE32(p, dataBytes);

	bool ok = file.write({reinterpret_cast<std::byte*>(header.data()), header.size()}).has_value();

	static std::array<Frame, 1024> silence{};
	auto writeFrames = [&](Frame* frames, uint32_t num) {
		auto written = file.write({reinterpret_cast<std::byte*>(frames), static_cast<size_t>(num) * sizeof(Frame)});
		ok = written.has_value() && *written == num * sizeof(Frame);
	};
	auto const kept = static_cast<int64_t>(framesFilled);
	// Straight from the chunks - the audio routine carries on during the writes, but frozen keeps it off them
	for (int64_t k = range.first; ok && k < range.end;) {
		uint32_t run;
		if (k < 0 || k >= kept) {
			int64_t const silenceEnd = (k < 0) ? std::min<int64_t>(0, range.end) : range.end;
			run = static_cast<uint32_t>(std::min<int64_t>(silenceEnd - k, silence.size()));
			writeFrames(silence.data(), run);
		}
		else {
			uint32_t const ringIndex = ringIndexOf(static_cast<uint32_t>(k));
			run = static_cast<uint32_t>(
			    std::min<int64_t>(std::min(range.end, kept) - k, kChunkFrames - (ringIndex & (kChunkFrames - 1))));
			writeFrames(&frameAt(ringIndex), run);
		}
		k += run;
	}
	ok = file.close().has_value() && ok;
	if (!ok) {
		display->displayError(Error::SD_CARD);
		return -1;
	}
	return static_cast<int32_t>(number);
}

/// A muted audio clip of the file on a new track, as long as the range. Returns its index in the session, or -1
int32_t makeAudioClip(String const& filePath, int32_t loopLength) {
	bool const grid = currentSong->sessionLayout == SessionLayoutType::SessionLayoutTypeGrid;
	// In rows, at the top: the row above the last clip. On the grid, a new track's column, which createNewClip()
	// leaves to us to add
	int32_t const index = currentSong->sessionClips.getNumElements();
	Clip* made = sessionView.createNewClip(OutputType::AUDIO, index - currentSong->songViewYScroll);
	if (made == nullptr || made->type != ClipType::AUDIO) {
		return -1;
	}
	auto* clip = static_cast<AudioClip*>(made);
	if (grid) {
		clip->section = 0;
		if (currentSong->sessionClips.insertClipAtIndex(clip, index) != Error::NONE) {
			display->displayError(Error::INSUFFICIENT_RAM);
			return -1; // Its track stays, empty
		}
	}
	clip->activeIfNoSolo = false;

	clip->sampleHolder.filePath.set(filePath.get());
	Error error = clip->sampleHolder.loadFile(false, true, true);
	if (error != Error::NONE) {
		display->displayError(error);
	}
	int32_t const oldLength = clip->loopLength;
	clip->loopLength = loopLength;
	clip->originalLength = loopLength;
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack =
	    setupModelStackWithSong(modelStackMemory, currentSong)->addTimelineCounter(clip);
	clip->lengthChanged(modelStack, oldLength);
	return currentSong->sessionClips.getIndexForClip(clip);
}

} // namespace

Output* trackSource = nullptr;

void applySetting() {
	if (frozen) {
		return; // The screen's using the rings - and isn't letting the menu open anyway
	}
	auto const wantSource = static_cast<Source>(std::min<uint32_t>(
	    runtimeFeatureSettings.get(RuntimeFeatureSettingType::SkipbackSource), static_cast<uint32_t>(Source::LINE_IN)));
	bool const wantWhilePlaying = runtimeFeatureSettings.get(RuntimeFeatureSettingType::SkipbackRecord) != 0;
	if (wantSource != source || wantWhilePlaying != onlyWhilePlaying) {
		source = wantSource;
		trackMode = (source == Source::TRACK);
		onlyWhilePlaying = wantWhilePlaying;
		trackSource = nullptr; // Picked up again at the next render, if it's tracks now
		lastCurrentClip = nullptr;
		restart();
	}
	uint32_t const seconds =
	    std::min<uint32_t>(runtimeFeatureSettings.get(RuntimeFeatureSettingType::Skipback), kMaxSeconds);
	uint32_t const wantChunks = (seconds * kSampleRate + kChunkFrames - 1) / kChunkFrames;
	if (wantChunks == numChunks && (seconds == 0) == (noteEvents == nullptr)) {
		return;
	}
	freeAll();
	if (seconds == 0) {
		return;
	}

	noteEvents = static_cast<NoteEvent*>(allocInStealableRegion(kNumNoteEvents * sizeof(NoteEvent)));

	uint32_t got = 0;
	while (got < wantChunks) {
		void* chunk = allocInStealableRegion(kChunkFrames * sizeof(Frame));
		if (chunk == nullptr) {
			break;
		}
		chunks[got++] = static_cast<Frame*>(chunk);
	}
	capacityFrames = got << kChunkFramesMagnitude;
	writeFrame = 0;
	framesFilled = 0;
	numChunks = got; // Last, so the audio routine only starts writing once it's all there

	if (got < wantChunks) {
		char text[32];
		snprintf(text, sizeof(text), display->haveOLED() ? "Skipback: RAM for %lus" : "%luS",
		         static_cast<unsigned long>(capacityFrames / kSampleRate));
		display->displayPopup(text);
	}
}

bool isOn() {
	return numChunks != 0 || noteEvents != nullptr;
}

void beginRender() {
	trackRendered = false;
	if (!trackMode || numChunks == 0 || frozen || currentSong == nullptr) {
		return;
	}
	Clip* clip = currentSong->getCurrentClip();
	if (clip != lastCurrentClip) {
		lastCurrentClip = clip;
		if (clip != nullptr) {
			setTrackSource(clip->output);
		}
	}
}

std::span<StereoSample> trackBuffer(size_t numSamples) {
	numSamples = std::min(numSamples, trackMemory.size());
	std::memset(trackMemory.data(), 0, numSamples * sizeof(StereoSample));
	trackRendered = true;
	return {trackMemory.data(), numSamples};
}

void mixTrack(std::span<StereoSample> mix) {
	auto* __restrict__ dst = reinterpret_cast<int32_t*>(mix.data());
	auto const* __restrict__ src = reinterpret_cast<int32_t const*>(trackMemory.data());
	uint32_t const numValues = std::min(mix.size(), trackMemory.size()) * 2;
	uint32_t j = 0;
	for (; j + 4 <= numValues; j += 4) {
		vst1q_s32(dst + j, vaddq_s32(vld1q_s32(dst + j), vld1q_s32(src + j)));
	}
	for (; j < numValues; j++) {
		dst[j] += src[j];
	}
}

void setSongGain(int32_t gain) {
	songGain = gain;
}

namespace {

/// Narrows interleaved 32-bit values to their top 16 bits, 8 at a time
void narrow(int16_t* __restrict__ dst, int32_t const* __restrict__ src, uint32_t numValues) {
	uint32_t j = 0;
	for (; j + 8 <= numValues; j += 8) {
		int16x4_t const lo = vshrn_n_s32(vld1q_s32(src + j), 16);
		int16x4_t const hi = vshrn_n_s32(vld1q_s32(src + j + 4), 16);
		vst1q_s16(dst + j, vcombine_s16(lo, hi));
	}
	for (; j < numValues; j++) {
		dst[j] = static_cast<int16_t>(src[j] >> 16);
	}
}

/// A track's audio, (x * gain) >> 31 saturated to 16 bits
void narrowWithGain(int16_t* __restrict__ dst, int32_t const* __restrict__ src, uint32_t numValues,
                    std::array<int32_t, 4> const& gains) {
	int32x4_t const gain = vld1q_s32(gains.data());
	uint32_t j = 0;
	for (; j + 8 <= numValues; j += 8) {
		int16x4_t const lo = vqmovn_s32(vqdmulhq_s32(vld1q_s32(src + j), gain));
		int16x4_t const hi = vqmovn_s32(vqdmulhq_s32(vld1q_s32(src + j + 4), gain));
		vst1q_s16(dst + j, vcombine_s16(lo, hi));
	}
	for (; j < numValues; j++) {
		int64_t const v = (static_cast<int64_t>(src[j]) * gains[j & 1]) >> 31;
		dst[j] = static_cast<int16_t>(std::clamp<int64_t>(v, INT16_MIN, INT16_MAX));
	}
}

/// The line input, from its ring: both channels, or the left one twice if it's the internal mic, which has no right
void narrowLineIn(int16_t* dst, int32_t const*& in, uint32_t numFrames, bool mono) {
	static_assert(NUM_MONO_INPUT_CHANNELS == 2);
	int32_t const* const ringStart = getRxBufferStart();
	int32_t const* const ringEnd = getRxBufferEnd();
	while (numFrames != 0) {
		auto const run = std::min<uint32_t>(numFrames, (ringEnd - in) / NUM_MONO_INPUT_CHANNELS);
		if (mono) {
			for (uint32_t f = 0; f < run; f++) {
				dst[2 * f] = dst[2 * f + 1] = static_cast<int16_t>(in[f * NUM_MONO_INPUT_CHANNELS] >> 16);
			}
		}
		else {
			narrow(dst, in, run * 2);
		}
		dst += run * 2;
		in += run * NUM_MONO_INPUT_CHANNELS;
		if (in >= ringEnd) {
			in = ringStart;
		}
		numFrames -= run;
	}
}

} // namespace

void feedOutput(std::span<StereoSample const> output, size_t renderOffset, int32_t gainL, int32_t gainR,
                int32_t const* lineIn) {
	if (numChunks == 0 || frozen) {
		return;
	}
	bool const playing = playbackHandler.isEitherClockActive();
	if (onlyWhilePlaying) {
		if (!playing) {
			wasPlaying = false;
			return; // What the last play kept stays, until the next one
		}
		if (!wasPlaying) {
			restart(); // Each play keeps its own
		}
	}
	wasPlaying = playing;

	// A track's audio as the output gets it, but for the song's FX: the song's volume, ((x * song) >> 32) << 5, the
	// master compressor's, ((x * compressor) >> 32) << 4, the master gain, ((x * master) >> 32) << 8, then its top 16
	// bits. That's x * (song * compressor * master >> 64) >> 31, the gain being under 2^29 - saturated
	int32_t const* track = nullptr;
	std::array<int32_t, 4> gains{};
	if (trackMode && trackRendered && currentSong != nullptr) {
		track = &trackMemory[std::min(renderOffset, trackMemory.size() - output.size())].l;
		auto const& compressor = currentSong->globalEffectable.compressor;
		auto const combined = [](int32_t song, int32_t compressorVolume, int32_t master) {
			int64_t const g = (static_cast<int64_t>(song) * compressorVolume) >> 32;
			return static_cast<int32_t>((g * master) >> 32);
		};
		gains[0] = gains[2] = combined(songGain, compressor.getVolumeL(), gainL);
		gains[1] = gains[3] = combined(songGain, compressor.getVolumeR(), gainR);
	}
	bool const lineInMono = !AudioEngine::lineInPluggedIn && !AudioEngine::micPluggedIn;

	size_t i = 0;
	size_t const n = output.size();
	while (i < n) {
		// capacityFrames is a whole number of chunks, so a run never goes past the end of the ring
		uint32_t const offset = writeFrame & (kChunkFrames - 1);
		uint32_t const run = std::min<uint32_t>(n - i, kChunkFrames - offset);
		auto* dst = reinterpret_cast<int16_t*>(&chunks[writeFrame >> kChunkFramesMagnitude][offset]);
		uint32_t const numValues = run * 2;
		switch (source) {
		case Source::MASTER:
			// Both sides interleaved L R, so it's one stream of values
			narrow(dst, reinterpret_cast<int32_t const*>(&output[i]), numValues);
			break;
		case Source::LINE_IN:
			narrowLineIn(dst, lineIn, run, lineInMono);
			break;
		case Source::TRACK:
			if (track != nullptr) {
				narrowWithGain(dst, track + i * 2, numValues, gains);
			}
			else {
				std::memset(dst, 0, numValues * sizeof(int16_t)); // The track didn't render
			}
			break;
		}
		writeFrame += run;
		if (writeFrame >= capacityFrames) {
			writeFrame = 0;
		}
		i += run;
	}
	framesFilled = std::min<uint32_t>(framesFilled + n, capacityFrames);
	lastFeedTime = AudioEngine::audioSampleTimer;
	feedPlaying = playing;
	if (playing) {
		feedTick = playbackHandler.getActualSwungTickCount();
		feedSamplesPerTick = playbackHandler.getTimePerInternalTickFloat();
		feedTickTime = AudioEngine::audioSampleTimer;
	}
}

void noteOn(Output* output, int32_t note, int32_t velocity) {
	if (trackMode && numChunks != 0 && !frozen) {
		setTrackSource(output); // The track being played is the one kept
	}
	addNoteEvent(output, note, std::clamp<int32_t>(velocity, 1, 127));
}

void noteOff(Output* output, int32_t note) {
	addNoteEvent(output, note, 0);
}

bool freeze(Capture& capture) {
	if (!isOn()) {
		display->displayPopup(display->haveOLED() ? "Skipback is off (Community Features)" : "OFF");
		return false;
	}
	if (numChunks == 0 || framesFilled == 0) {
		display->displayPopup(display->haveOLED() ? "Nothing kept yet" : "NONE");
		return false;
	}
	frozen = true;
	previewing = false;

	capture.numFrames = framesFilled;
	capture.newestTime = lastFeedTime - 1;
	// The song's position when the newest frame was kept - which, while only keeping while playing, can be from
	// before the transport stopped
	capture.samplesPerTick = feedSamplesPerTick;
	capture.playing = feedPlaying && capture.samplesPerTick > 0;
	capture.newestTick = 0;
	if (capture.playing) {
		capture.newestTick =
		    feedTick - std::llround(static_cast<double>(feedTickTime - capture.newestTime) / capture.samplesPerTick);
	}
	else {
		capture.samplesPerTick = playbackHandler.getTimePerInternalTickFloat();
	}
	if (capture.samplesPerTick <= 0) {
		capture.samplesPerTick = 1;
	}
	capture.barLength = std::max<int64_t>(currentSong->getBarLength(), 1);
	char const* name = trackMode ? "Track" : (source == Source::LINE_IN) ? "Line in" : "Master";
	if (trackMode && trackSource != nullptr && isInSong(trackSource) && !trackSource->name.isEmpty()) {
		name = trackSource->name.get();
	}
	snprintf(capture.sourceName, sizeof(capture.sourceName), "%s", name);

	if (!buildBlockPeaks()) {
		frozen = false;
		display->displayError(Error::INSUFFICIENT_RAM);
		return false;
	}
	capture.maxPeak = 0;
	for (uint32_t b = 0; b < numBlocks; b++) {
		capture.maxPeak = std::max<int32_t>(capture.maxPeak, blockPeaks[b]);
	}
	frozenCapture = capture;
	return true;
}

void unfreeze() {
	previewing = false;
	if (blockPeaks != nullptr) {
		delugeDealloc(blockPeaks);
		blockPeaks = nullptr;
	}
	numBlocks = 0;
	frozen = false;
}

int32_t peak(int64_t first, int64_t end) {
	first = std::max<int64_t>(first, 0);
	end = std::min<int64_t>(end, framesFilled);
	if (end <= first) {
		return 0;
	}
	auto const f = static_cast<uint32_t>(first);
	auto const e = static_cast<uint32_t>(end);
	if (blockPeaks == nullptr || e - f < 2 * kBlockFrames) {
		return scanPeak(f, e);
	}
	// Whole blocks from the table, the bits either side from the frames
	uint32_t const firstWhole = (f + kBlockFrames - 1) >> kBlockFramesMagnitude;
	uint32_t const endWhole = e >> kBlockFramesMagnitude;
	int32_t loudest =
	    std::max(scanPeak(f, firstWhole << kBlockFramesMagnitude), scanPeak(endWhole << kBlockFramesMagnitude, e));
	for (uint32_t b = firstWhole; b < endWhole; b++) {
		loudest = std::max<int32_t>(loudest, blockPeaks[b]);
	}
	return loudest;
}

Range suggest(Capture const& capture) {
	int64_t const n = capture.numFrames;
	int64_t const gapBlocks = kPhraseGap >> kBlockFramesMagnitude;

	// The last stretch of sound, back to a two-second gap
	int64_t first = -1;
	int64_t last = -1;
	int64_t b = static_cast<int64_t>(numBlocks) - 1;
	while (b >= 0 && blockPeaks[b] <= kSilence) {
		b--;
	}
	if (b >= 0) {
		int64_t firstBlock = b;
		int64_t gap = 0;
		for (int64_t c = b - 1; c >= 0 && gap < gapBlocks; c--) {
			if (blockPeaks[c] > kSilence) {
				firstBlock = c;
				gap = 0;
			}
			else {
				gap++;
			}
		}
		// To the frame, within the blocks at either end
		first = firstBlock << kBlockFramesMagnitude;
		while (first < n && frameLoudness(frameAt(ringIndexOf(first))) <= kSilence) {
			first++;
		}
		last = std::min<int64_t>((b + 1) << kBlockFramesMagnitude, n) - 1;
		while (last > first && frameLoudness(frameAt(ringIndexOf(last))) <= kSilence) {
			last--;
		}
	}

	// The last stretch of notes, the same way - for instruments that aren't heard through the Deluge, like MIDI ones
	int64_t notesFirst = -1;
	int64_t notesLast = -1;
	for (int64_t k = static_cast<int64_t>(noteCount) - 1; k >= 0; k--) {
		int64_t const frame = frameOfTime(capture, noteEventAt(k).time);
		if (frame < 0 || frame >= n) {
			continue;
		}
		if (notesLast < 0) {
			notesLast = frame;
			notesFirst = frame;
		}
		else if (notesFirst - frame < kPhraseGap) {
			notesFirst = frame;
		}
		else {
			break;
		}
	}
	if (notesLast >= 0) {
		if (last < 0 || notesFirst > last + kPhraseGap) {
			first = notesFirst; // The notes are later than the sound, with a gap between
			last = notesLast;
		}
		else if (notesLast + kPhraseGap >= first) {
			first = std::min(first, notesFirst);
			last = std::max(last, notesLast);
		}
	}

	if (last < 0) {
		// Nothing at all: the last four seconds, so there's something to look at
		return {std::max<int64_t>(n - 4 * kSampleRate, 0), n};
	}

	if (capture.playing) {
		Range range{snap(capture, first, capture.barLength, -1), snap(capture, last + 1, capture.barLength, 1)};
		if (range.end <= range.first) {
			range.end = snap(capture, range.first + 1, capture.barLength, 1);
		}
		return range;
	}
	return {std::max<int64_t>(first - kPreRoll, 0), std::min<int64_t>(last + 1 + kTail, n)};
}

int64_t snap(Capture const& capture, int64_t frame, int64_t gridTicks, int32_t direction) {
	if (!capture.playing || gridTicks <= 0) {
		return frame;
	}
	double const steps = tickOfFrame(capture, frame) / static_cast<double>(gridTicks);
	double const snapped = (direction < 0) ? std::floor(steps) : (direction > 0) ? std::ceil(steps) : std::round(steps);
	return frameOfTick(capture, snapped * static_cast<double>(gridTicks));
}

int32_t countNotes(Capture const& capture, Range range) {
	int32_t count = 0;
	for (uint32_t k = 0; k < noteCount; k++) {
		NoteEvent const& e = noteEventAt(k);
		int64_t const frame = frameOfTime(capture, e.time);
		if (e.velocity != 0 && frame >= range.first && frame < range.end && isInSong(e.output)) {
			count++;
		}
	}
	return count;
}

void startPreview(Range range) {
	previewPos = range.first;
	previewEnd = range.end;
	previewing = frozen && range.end > range.first;
}

void stopPreview() {
	previewing = false;
}

bool isPreviewing() {
	return previewing;
}

int64_t previewFrame() {
	return previewPos;
}

void nextPreviewFrame(int32_t& l, int32_t& r) {
	if (!previewing) {
		l = 0;
		r = 0;
		return;
	}
	if (previewPos >= 0 && previewPos < static_cast<int64_t>(framesFilled)) {
		Frame const& frame = frameAt(ringIndexOf(static_cast<uint32_t>(previewPos)));
		l = static_cast<int32_t>(frame.l) << 16;
		r = static_cast<int32_t>(frame.r) << 16;
	}
	else {
		l = 0;
		r = 0;
	}
	if (++previewPos >= previewEnd) {
		previewing = false;
	}
}

Saved save(Capture const& capture, Range range) {
	Saved saved;
	previewing = false;
	if (range.length() <= 0) {
		return saved;
	}
	int32_t const loopLength = loopLengthFor(capture, range);
	int32_t topIndex = -1;

	// The notes first: nothing in there touches the card
	bool clearedUndo = false;
	constexpr int32_t kMaxOutputs = 16;
	std::array<Output*, kMaxOutputs> outputs{};
	int32_t numOutputs = 0;
	for (uint32_t k = 0; k < noteCount && numOutputs < kMaxOutputs; k++) {
		NoteEvent const& e = noteEventAt(k);
		int64_t const frame = frameOfTime(capture, e.time);
		if (e.velocity == 0 || frame < range.first || frame >= range.end) {
			continue;
		}
		if (std::find(outputs.begin(), outputs.begin() + numOutputs, e.output) == outputs.begin() + numOutputs) {
			outputs[numOutputs++] = e.output;
		}
	}
	for (int32_t i = 0; i < numOutputs; i++) {
		// The song may have changed, or the instrument been deleted, since the note was played
		if (!isInSong(outputs[i])) {
			continue;
		}
		if (!clearedUndo) {
			actionLogger.deleteAllLogs(); // As making a new clip does - there's no undoing it
			clearedUndo = true;
		}
		int32_t const index = captureNotesFor(outputs[i], capture, range, loopLength);
		if (index >= 0) {
			saved.numNoteClips++;
			topIndex = std::max(topIndex, index);
		}
	}

	// Then the audio, if there's any sound in the range
	if (peak(range.first, range.end) > kSilence) {
		display->displayLoadingAnimationText("Saving");
		String filePath;
		saved.fileNumber = writeWav(range, filePath);
		if (saved.fileNumber >= 0) {
			int32_t const index = makeAudioClip(filePath, loopLength);
			if (index >= 0) {
				saved.audioClip = true;
				topIndex = std::max(topIndex, index);
			}
		}
		display->removeLoadingAnimation();
	}

	if (topIndex >= 0) {
		showRow(topIndex);
	}
	return saved;
}

} // namespace deluge::skipback
