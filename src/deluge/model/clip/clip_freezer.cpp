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

#include "model/clip/clip_freezer.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/root_ui.h"
#include "gui/ui/ui.h"
#include "gui/views/session_view.h"
#include "gui/views/view.h"
#include "hid/display/display.h"
#include "memory/general_memory_allocator.h"
#include "model/action/action_logger.h"
#include "model/clip/audio_clip.h"
#include "model/clip/freeze_latency.h"
#include "model/clip/freeze_warmup.h"
#include "model/clip/instrument_clip.h"
#include "model/model_stack.h"
#include "model/note/note.h"
#include "model/note/note_row.h"
#include "model/sample/sample.h"
#include "model/sample/sample_cluster.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "playback/mode/session.h"
#include "playback/playback_handler.h"
#include "processing/audio_output.h"
#include "processing/engines/audio_engine.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/cluster/cluster.h"
#include "storage/flash_storage.h"
#include <cstdio>
#include <new>

ClipFreezer clipFreezer{};

namespace {
/// Latency is looked for this far either side of where the recording's loop starts. Late is limited by the recording's
/// end margin, kAudioClipMarginSizePostEnd, which is 46 ms.
constexpr int32_t kMaxEarlySamples = 512;
constexpr int32_t kMaxLateSamples = kAudioClipMarginSizePostEnd;
/// How long after its loop's end a freeze recording needs, to have its end margin
constexpr int32_t kRecordingTailSamples = kAudioClipMarginSizePostEnd + 4 * SSI_TX_BUFFER_NUM_SAMPLES + 4410;
} // namespace

namespace {

using L10nString = deluge::l10n::String;

void popup(L10nString string) {
	display->displayPopup(deluge::l10n::get(string));
}

int32_t sessionIndexOf(Song* song, Clip const* clip) {
	return song->sessionClips.getIndexForClip(const_cast<Clip*>(clip));
}

/// Point anything in the song which refers to one session clip at another, for when one takes the other's place
void retargetClipReferences(Song* song, Clip* from, Clip* to) {
	for (auto& macro : song->sessionMacros) {
		if (macro.kind == CLIP_LAUNCH && macro.clip == from) {
			macro.clip = to;
		}
	}
	if (song->getCurrentClip() == from) {
		song->setCurrentClip(to);
	}
	if (song->syncScalingClip == from) {
		song->syncScalingClip = to;
	}
}

/// Remove an Output's session macros, before it leaves the song
void forgetOutputMacros(Song* song, Output* output) {
	for (auto& macro : song->sessionMacros) {
		if (macro.kind == OUTPUT_CYCLE && macro.output == output) {
			macro.kind = NO_MACRO;
		}
	}
}

} // namespace

bool ClipFreezer::canFreeze(Clip* clip, bool showPopup) {
	auto refuse = [showPopup](L10nString why) {
		if (showPopup) {
			popup(why);
		}
		return false;
	};

	if (!runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::ClipFreeze)) {
		return false;
	}
	if (clip == nullptr || clip->type != ClipType::INSTRUMENT || clip->output == nullptr
	    || clip->output->type != OutputType::MIDI_OUT) {
		return refuse(L10nString::STRING_FOR_FREEZE_MIDI_CLIPS_ONLY);
	}
	if (clipFreezer.isBusy()) {
		return refuse(L10nString::STRING_FOR_FREEZE_BUSY);
	}
	if (sessionIndexOf(currentSong, clip) < 0 || clip->output->clipHasInstance(clip)) {
		return refuse(L10nString::STRING_FOR_FREEZE_CLIP_IN_ARRANGEMENT);
	}
	if (playbackHandler.isEitherClockActive() && currentPlaybackMode != &session) {
		return refuse(L10nString::STRING_FOR_FREEZE_NOT_IN_ARRANGER);
	}
	if (clip->getCurrentlyRecordingLinearly() || clip->isPendingOverdub) {
		return refuse(L10nString::STRING_FOR_FREEZE_BUSY);
	}
	if (static_cast<InstrumentClip*>(clip)->isEmpty(false)) {
		return refuse(L10nString::STRING_FOR_FREEZE_EMPTY_CLIP);
	}
	return true;
}

bool ClipFreezer::involves(Clip const* clip) const {
	return state != State::IDLE && clip != nullptr && (clip == midiClip || clip == audioClip);
}

bool ClipFreezer::isRecordingInto(AudioClip const* clip) const {
	return state != State::IDLE && clip == audioClip;
}

void ClipFreezer::start(InstrumentClip* clip, AudioInputChannel inputChannel) {
	if (!canFreeze(clip, true)) {
		return;
	}
	Song* thisSong = currentSong;
	int32_t midiIndex = sessionIndexOf(thisSong, clip);

	// Holding the clip's status pad to get to Clip Settings armed it to stop (or start). That's not what was wanted:
	// freezing plays it from where it is
	if (clip->armState != ArmState::OFF) {
		session.cancelArmingForClip(clip, &midiIndex);
	}

	if (!thisSong->sessionClips.ensureEnoughSpaceAllocated(1)) {
		display->displayError(Error::INSUFFICIENT_RAM);
		return;
	}
	void* clipMemory = GeneralMemoryAllocator::get().allocMaxSpeed(sizeof(AudioClip));
	if (clipMemory == nullptr) {
		display->displayError(Error::INSUFFICIENT_RAM);
		return;
	}
	AudioOutput* newOutput = thisSong->createNewAudioOutput();
	if (newOutput == nullptr) {
		delugeDealloc(clipMemory);
		display->displayError(Error::INSUFFICIENT_RAM);
		return;
	}
	newOutput->colour = clip->output->colour;
	newOutput->inputChannel = inputChannel;
	newOutput->mode = AudioOutputMode::player; // No monitoring - the gear is already heard wherever it's heard

	// Like Song::replaceInstrumentClipWithAudioClip(), but it doesn't take the MIDI clip's place until it's recorded
	auto* newClip = new (clipMemory) AudioClip();
	newClip->cloneFrom(clip);
	newClip->armState = ArmState::OFF;
	newClip->armedForRecording = false; // So a press of RECORD can't start it recording anything else
	newClip->activeIfNoSolo = false;
	newClip->soloingInSessionMode = false;

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStack* modelStack = setupModelStackWithSong(modelStackMemory, thisSong);
	ModelStackWithTimelineCounter* modelStackWithAudioClip = modelStack->addTimelineCounter(newClip);
	if (newClip->setOutput(modelStackWithAudioClip, newOutput) != Error::NONE) {
		newClip->~AudioClip();
		delugeDealloc(clipMemory);
		thisSong->deleteOutputThatIsInMainList(newOutput);
		display->displayError(Error::INSUFFICIENT_RAM);
		return;
	}
	// Just below the MIDI clip while it records
	thisSong->sessionClips.insertClipAtIndex(newClip, midiIndex + 1);

	// Undo can't follow clips swapping places, so start afresh
	actionLogger.deleteAllLogs();

	song = thisSong;
	midiClip = clip;
	audioClip = newClip;
	lastInputChannel = inputChannel;

	if (!playbackHandler.isEitherClockActive()) {
		// Play from the start, with the MIDI clip playing
		clip->activeIfNoSolo = true;
		newClip->activeIfNoSolo = true;
		newClip->soloingInSessionMode = clip->soloingInSessionMode;
		beginWarmingUp();
		playbackHandler.setupPlaybackUsingInternalClock();
	}
	else if (thisSong->isClipActive(clip)) {
		// Join in with the MIDI clip where it's up to
		newClip->activeIfNoSolo = true;
		newClip->soloingInSessionMode = clip->soloingInSessionMode;
		newClip->setPos(modelStackWithAudioClip, clip->getCurrentPosAsIfPlayingInForwardDirection(), true);
		newClip->resumePlayback(modelStackWithAudioClip);
		beginWarmingUp();
	}
	else {
		// Launch both at the MIDI clip's next launch
		session.toggleClipStatus(clip, &midiIndex, false, 0);
		newClip->armState = clip->armState;
		state = State::STARTING;
	}

	uiNeedsRendering(&sessionView);
}

void ClipFreezer::beginWarmingUp() {
	uint64_t loopLengthInSamples = (uint64_t)midiClip->loopLength * playbackHandler.getTimePerInternalTick();
	loopEndsUntilRecording = deluge::freeze::warmupPasses(loopLengthInSamples);
	state = State::WARMING_UP;
	popup(L10nString::STRING_FOR_FREEZE_WARMING_UP);
}

void ClipFreezer::loopEnded(ModelStackWithTimelineCounter* modelStack) {
	switch (state) {
	case State::WARMING_UP:
		if (--loopEndsUntilRecording > 0) {
			return;
		}
		// Recording takes the loop from here to the next loop end, as when a session clip is launched to record
		audioClip->originalLength = audioClip->loopLength;
		audioClip->isPendingOverdub = false;
		{
			// Record margins whatever the setting: the audio past the loop's end is what the sample's start and end
			// move into to make up for latency
			bool marginsSetting = FlashStorage::audioClipRecordMargins;
			FlashStorage::audioClipRecordMargins = true;
			Error error = audioClip->beginLinearRecording(modelStack, 0);
			FlashStorage::audioClipRecordMargins = marginsSetting;
			if (error != Error::NONE) {
				// routine() sees there's no recording, and cancels
				state = State::SWAPPING;
				return;
			}
		}
		state = State::RECORDING;
		// The MIDI clip stops as the recording ends, and the AudioClip carries on with the same loop
		if (song->isClipActive(midiClip)) {
			session.armClipToStopAction(midiClip);
		}
		break;

	case State::RECORDING:
		audioClip->finishLinearRecording(modelStack, nullptr, 0);
		recordingFinishedAt = AudioEngine::audioSampleTimer;
		state = State::SWAPPING;
		break;

	default:
		break;
	}
}

bool ClipFreezer::safeToRestructure() const {
	// Taking a clip out of the song while its own view is showing it would leave that view with a dangling clip. Any
	// other view is fine: the clips are swapped, or the AudioClip deleted, and currentClip pointed at what remains
	Clip* shown = getCurrentClip();
	return getRootUI() == &sessionView || (shown != midiClip && shown != audioClip);
}

bool ClipFreezer::stillValid() {
	// A song swap would have deleted them. Undo could have taken them out of the song.
	if (currentSong != song || sessionIndexOf(song, midiClip) < 0 || sessionIndexOf(song, audioClip) < 0) {
		return false;
	}
	if (state == State::STARTING) {
		return playbackHandler.isEitherClockActive() || midiClip->armState != ArmState::OFF;
	}
	return playbackHandler.isEitherClockActive() && currentPlaybackMode == &session;
}

void ClipFreezer::routine() {
	if (state == State::IDLE) {
		return;
	}

	if (!stillValid()) {
		if (safeToRestructure() || currentSong != song) {
			cancel();
		}
		return;
	}

	switch (state) {
	case State::STARTING:
		if (song->isClipActive(midiClip) && song->isClipActive(audioClip)) {
			beginWarmingUp();
		}
		else if (midiClip->armState == ArmState::OFF) {
			cancel(); // The launch got called off
		}
		break;

	case State::WARMING_UP:
		// Someone stopped one of them
		if ((!song->isClipActive(midiClip) || !song->isClipActive(audioClip)) && safeToRestructure()) {
			cancel();
		}
		break;

	case State::RECORDING:
		if ((!song->isClipActive(audioClip) || !audioClip->getCurrentlyRecordingLinearly()) && safeToRestructure()) {
			cancel();
		}
		break;

	case State::SWAPPING:
		// finishLinearRecording() leaves no sample if the recording got aborted
		if (audioClip->sampleHolder.audioFile == nullptr) {
			if (safeToRestructure()) {
				cancel();
			}
		}
		// The recording carries on for its end margin, which measuring the latency needs
		else if ((int32_t)(AudioEngine::audioSampleTimer - recordingFinishedAt) < kRecordingTailSamples) {}
		else if (safeToRestructure()) {
			compensateLatency();
			swapInPlace();
			finish(false);
		}
		// Otherwise the AudioClip's already playing in place of the MIDI clip, and the swap waits for its view to close
		break;

	default:
		break;
	}
}

void ClipFreezer::swapInPlace() {
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStack* modelStack = setupModelStackWithSong(modelStackMemory, song);

	// Normally the MIDI clip has just stopped, armed to at the end of the recording. If someone called that off, stop
	// it
	int32_t midiIndex = sessionIndexOf(song, midiClip);
	if (midiClip->armState != ArmState::OFF) {
		session.cancelArmingForClip(midiClip, &midiIndex);
	}
	if (playbackHandler.isEitherClockActive() && song->isClipActive(midiClip)) {
		midiClip->expectNoFurtherTicks(song);
	}
	midiClip->activeIfNoSolo = false;
	midiClip->soloingInSessionMode = false;

	// The AudioClip takes the MIDI clip's row
	song->sessionClips.deleteAtIndex(sessionIndexOf(song, audioClip));
	midiIndex = sessionIndexOf(song, midiClip);
	song->sessionClips.setPointerAtIndex(audioClip, midiIndex);
	retargetClipReferences(song, midiClip, audioClip);

	// The MIDI clip keeps its ParamManager (MIDI clips hold their own) and its link to its Output, but isn't the
	// Output's clip any more
	Output* midiOutput = midiClip->output;
	if (midiClip->isActiveOnOutput()) {
		midiOutput->detachActiveClip(song);
	}
	midiOutput->pickAnActiveClipIfPossible(modelStack, true, PgmChangeSend::NEVER, true);

	audioClip->frozen.clip = midiClip;

	// If it was the Output's only clip, the Output goes too, and the AudioClip's Output takes its place in the list
	if (song->getClipWithOutput(midiOutput) == nullptr && midiOutput->clipInstances.getNumElements() == 0) {
		forgetOutputMacros(song, midiOutput);
		Output* audioOutput = audioClip->output;
		song->removeOutputFromMainList(audioOutput, false);
		int32_t outputIndex = song->removeOutputFromMainList(midiOutput);
		song->insertOutputAtIndex(audioOutput, outputIndex);
		audioClip->frozen.detachedOutput = midiOutput;
	}

	actionLogger.deleteAllLogs();
	if (getRootUI() == &sessionView) {
		view.setActiveModControllableTimelineCounter(audioClip);
	}
}

size_t ClipFreezer::readRecordingLevels(uint64_t startSample, std::span<uint32_t> levels) {
	auto* sample = static_cast<Sample*>(audioClip->sampleHolder.audioFile);
	int32_t numChannels = sample->numChannels;
	int32_t byteDepth = sample->byteDepth;
	int32_t bytesPerFrame = numChannels * byteDepth;
	uint64_t endByte = sample->audioDataStartPosBytes + sample->audioDataLengthBytes;
	uint64_t bytePos = sample->audioDataStartPosBytes + startSample * bytesPerFrame;

	constexpr size_t kChunkFrames = 1024; // A multiple of the block size
	int32_t chunk[kChunkFrames];
	uint32_t envelope = 0;
	size_t numBlocks = 0;
	Cluster* cluster = nullptr;
	int32_t clusterIndex = -1;

	while (numBlocks < levels.size()) {
		size_t frames = 0;
		while (frames < kChunkFrames && bytePos + bytesPerFrame <= endByte) {
			// As Sample::determinePitch() reads it
			int32_t wantedCluster = bytePos >> Cluster::size_magnitude;
			if (wantedCluster != clusterIndex) {
				if (cluster != nullptr) {
					audioFileManager.removeReasonFromCluster(*cluster, "freeze latency");
				}
				clusterIndex = wantedCluster;
				cluster = sample->clusters.getElement(clusterIndex)
				              ->getCluster(sample, clusterIndex, CLUSTER_LOAD_IMMEDIATELY);
				if (cluster == nullptr) {
					return 0;
				}
			}
			int32_t mono = 0;
			for (int32_t c = 0; c < numChannels; c++) {
				int32_t offset = ((bytePos + c * byteDepth) & (Cluster::size - 1)) - 4 + byteDepth;
				int32_t value = *(int32_t*)&cluster->data[offset] & sample->bitMask;
				mono += value / numChannels;
			}
			chunk[frames++] = mono;
			bytePos += bytesPerFrame;
		}
		size_t made = deluge::freeze::blockLevels({chunk, frames}, levels.subspan(numBlocks), envelope);
		numBlocks += made;
		if (frames < kChunkFrames) {
			break; // Reached the end of the audio
		}
		AudioEngine::routineWithClusterLoading();
	}
	if (cluster != nullptr) {
		audioFileManager.removeReasonFromCluster(*cluster, "freeze latency");
	}
	return numBlocks;
}

void ClipFreezer::compensateLatency() {
	auto* sample = static_cast<Sample*>(audioClip->sampleHolder.audioFile);
	SampleHolderForClip& holder = audioClip->sampleHolder;
	if (sample == nullptr || holder.endPos <= holder.startPos || midiClip->loopLength <= 0) {
		popup(L10nString::STRING_FOR_FROZEN);
		return;
	}
	uint64_t loopSamples = holder.endPos - holder.startPos;

	// The search can go this far either side of where the recording's loop starts, as far as there's audio
	int32_t before = std::min<uint64_t>(holder.startPos, kMaxEarlySamples);
	int32_t after = std::min<uint64_t>(
	    sample->lengthInSamples > holder.endPos ? sample->lengthInSamples - holder.endPos : 0, kMaxLateSamples);

	int32_t latency = 0;
	bool measured = false;

	// Where the clip's notes start, in samples into the levels
	constexpr size_t kMaxNotes = 1024;
	auto* noteStarts =
	    static_cast<uint32_t*>(GeneralMemoryAllocator::get().allocLowSpeed(kMaxNotes * sizeof(uint32_t)));
	size_t numBlocks = (loopSamples + before + after) / deluge::freeze::kLatencyBlockSize;
	auto* levels = static_cast<uint32_t*>(GeneralMemoryAllocator::get().allocLowSpeed(numBlocks * sizeof(uint32_t)));
	if (noteStarts != nullptr && levels != nullptr) {
		size_t numNotes = 0;
		for (int32_t r = 0; r < midiClip->noteRows.getNumElements(); r++) {
			NoteRow* noteRow = midiClip->noteRows.getElement(r);
			if (noteRow->muted) {
				continue;
			}
			for (int32_t n = 0; n < noteRow->notes.getNumElements() && numNotes < kMaxNotes; n++) {
				int32_t pos = noteRow->notes.getElement(n)->pos;
				if (pos >= 0 && pos < midiClip->loopLength) {
					noteStarts[numNotes++] = before + (uint64_t)pos * loopSamples / midiClip->loopLength;
				}
			}
		}
		size_t numLevels = readRecordingLevels(holder.startPos - before, {levels, numBlocks});
		deluge::freeze::LatencyEstimate estimate =
		    deluge::freeze::estimateLatency({levels, numLevels}, {noteStarts, numNotes}, before + after);
		if (estimate.confident) {
			latency = estimate.samples - before;
			measured = true;
			lastLatencySamples = latency;
		}
	}
	if (noteStarts != nullptr) {
		delugeDealloc(noteStarts);
	}
	if (levels != nullptr) {
		delugeDealloc(levels);
	}

	if (!measured && lastLatencySamples != kLatencyUnknown) {
		latency = lastLatencySamples;
	}
	latency = std::clamp(latency, -before, after);

	// The loop is the same length, just later (or earlier) in the recording
	holder.startPos += latency;
	holder.endPos += latency;
	audioClip->renderData.xScroll = -1;
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, song, audioClip);
	audioClip->sampleZoneChanged(modelStack);

	if (display->haveOLED()) {
		// "Frozen, latency 7.3 ms", or say it couldn't be measured
		char text[80];
		char const* frozen = deluge::l10n::get(L10nString::STRING_FOR_FROZEN);
		if (!measured && latency == 0) {
			snprintf(text, sizeof(text), "%s, %s", frozen,
			         deluge::l10n::get(L10nString::STRING_FOR_FREEZE_LATENCY_UNCLEAR));
		}
		else {
			int32_t tenthsOfMs = (std::abs(latency) * 10000 + kSampleRate / 2) / kSampleRate;
			snprintf(text, sizeof(text), "%s, %s %s%d.%d ms", frozen,
			         deluge::l10n::get(measured ? L10nString::STRING_FOR_FREEZE_LATENCY
			                                    : L10nString::STRING_FOR_FREEZE_LATENCY_USED_LAST),
			         (latency < 0) ? "-" : "", static_cast<int>(tenthsOfMs / 10), static_cast<int>(tenthsOfMs % 10));
		}
		display->displayPopup(text);
	}
	else {
		popup(L10nString::STRING_FOR_FROZEN);
	}
}

void ClipFreezer::cancel() {
	finish(true);
}

void ClipFreezer::finish(bool cancelled) {
	if (cancelled && currentSong == song && song != nullptr) {
		int32_t audioIndex = sessionIndexOf(song, audioClip);
		if (audioIndex >= 0) {
			audioClip->abortRecording();
			if (audioClip->armState != ArmState::OFF) {
				session.cancelArmingForClip(audioClip, &audioIndex);
			}
			song->removeSessionClipLowLevel(audioClip, audioIndex);
			song->deleteClipObject(audioClip, false, InstrumentRemoval::DELETE);
		}
		int32_t midiIndex = sessionIndexOf(song, midiClip);
		if (midiIndex >= 0 && state == State::RECORDING && midiClip->armState != ArmState::OFF) {
			session.cancelArmingForClip(midiClip, &midiIndex); // It was only going to stop for us
		}
		actionLogger.deleteAllLogs();
		popup(L10nString::STRING_FOR_FREEZE_CANCELLED);
	}
	state = State::IDLE;
	song = nullptr;
	midiClip = nullptr;
	audioClip = nullptr;
	if (getCurrentUI() == &sessionView) {
		uiNeedsRendering(&sessionView);
	}
}

bool ClipFreezer::unfreeze(AudioClip* clip) {
	Song* thisSong = currentSong;
	int32_t index = sessionIndexOf(thisSong, clip);
	if (clip == nullptr || !clip->hasFrozenSource() || index < 0 || clipFreezer.involves(clip)) {
		return false;
	}
	// The arrangement would be left pointing at the AudioClip
	if (clip->output->clipHasInstance(clip)) {
		popup(L10nString::STRING_FOR_FREEZE_CLIP_IN_ARRANGEMENT);
		return false;
	}
	if (clip->getCurrentlyRecordingLinearly()) {
		popup(L10nString::STRING_FOR_FREEZE_BUSY);
		return false;
	}

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStack* modelStack = setupModelStackWithSong(modelStackMemory, thisSong);

	InstrumentClip* midiClip = clip->frozen.clip;
	Output* detachedOutput = clip->frozen.detachedOutput;
	clip->frozen = {}; // The song owns them again from here

	// Its Output goes back where the AudioClip's is
	if (detachedOutput != nullptr) {
		thisSong->insertOutputAtIndex(detachedOutput, thisSong->getOutputIndex(clip->output));
	}

	bool wasActive = thisSong->isClipActive(clip);
	bool clockActive = playbackHandler.isEitherClockActive() && currentPlaybackMode == &session;
	int32_t pos = clip->getCurrentPosAsIfPlayingInForwardDirection();
	if (clip->armState != ArmState::OFF) {
		clip->armState = ArmState::OFF;
		session.launchSchedulingMightNeedCancelling();
	}
	if (clockActive && wasActive) {
		clip->expectNoFurtherTicks(thisSong);
	}

	thisSong->sessionClips.setPointerAtIndex(midiClip, index);
	retargetClipReferences(thisSong, clip, midiClip);
	midiClip->activeIfNoSolo = clip->activeIfNoSolo;
	midiClip->soloingInSessionMode = clip->soloingInSessionMode;
	midiClip->armState = ArmState::OFF;

	ModelStackWithTimelineCounter* modelStackWithMIDIClip = modelStack->addTimelineCounter(midiClip);
	Clip* otherClip = midiClip->output->getActiveClip();
	if (otherClip != nullptr && otherClip != midiClip && thisSong->isClipActive(otherClip)) {
		// Another of the Output's clips was launched meanwhile, and only one can play
		midiClip->activeIfNoSolo = false;
		midiClip->soloingInSessionMode = false;
	}
	else {
		midiClip->output->setActiveClip(modelStackWithMIDIClip, PgmChangeSend::NEVER);
		// Carry on where the AudioClip was
		if (clockActive && wasActive) {
			midiClip->setPos(modelStackWithMIDIClip, pos, true);
			midiClip->resumePlayback(modelStackWithMIDIClip);
		}
	}

	view.setActiveModControllableTimelineCounter(midiClip);
	thisSong->deleteClipObject(clip, false, InstrumentRemoval::DELETE);

	actionLogger.deleteAllLogs();
	popup(L10nString::STRING_FOR_UNFROZEN);
	if (getCurrentUI() == &sessionView) {
		uiNeedsRendering(&sessionView);
	}
	return true;
}
