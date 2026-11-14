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
#include "io/midi/midi_engine.h"
#include "memory/general_memory_allocator.h"
#include "model/action/action_logger.h"
#include "model/clip/audio_clip.h"
#include "model/clip/freeze_latency.h"
#include "model/clip/freeze_warmup.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/midi_instrument.h"
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
/// A latency measured beforehand moves the recording itself, so isn't limited by its margin. This is just for sanity
constexpr int32_t kMaxRecordShiftSamples = kSampleRate / 2;
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

	bool pinging = canPing();
	if (pinging) {
		choosePings();
	}
	pingLatencyKnown = false;
	recordingLatency = 0;
	midiLaunchPending = false;
	bool clockActive = playbackHandler.isEitherClockActive();
	midiWasPlaying = clockActive && thisSong->isClipActive(clip);

	newClip->activeIfNoSolo = true;
	newClip->soloingInSessionMode = clip->soloingInSessionMode;
	if (clockActive) {
		// The AudioClip joins in where the MIDI clip's up to - or where it would be, if it isn't playing
		int32_t pos = midiWasPlaying ? clip->getCurrentPosAsIfPlayingInForwardDirection()
		                             : (int32_t)((uint64_t)playbackHandler.lastSwungTickActioned % clip->loopLength);
		newClip->setPos(modelStackWithAudioClip, pos, true);
		newClip->resumePlayback(modelStackWithAudioClip);
	}

	if (pinging) {
		// The gear has to be quiet for the pings: the MIDI clip starts again after them
		if (midiWasPlaying) {
			clip->expectNoFurtherTicks(thisSong);
		}
		clip->activeIfNoSolo = false;
		state = State::MEASURE_WAIT;
		popup(L10nString::STRING_FOR_FREEZE_MEASURING);
	}
	else {
		if (clockActive && !midiWasPlaying) {
			launchMIDIClipInStep();
		}
		else {
			clip->activeIfNoSolo = true;
		}
		beginWarmingUp();
	}

	if (!clockActive) {
		playbackHandler.setupPlaybackUsingInternalClock();
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
	case State::MEASURE_WAIT:
		// Record one loop of pings, as a recording of the loop would be made
		audioClip->originalLength = audioClip->loopLength;
		audioClip->isPendingOverdub = false;
		{
			bool marginsSetting = FlashStorage::audioClipRecordMargins;
			FlashStorage::audioClipRecordMargins = true; // The latency's looked for in its end margin too
			Error error = audioClip->beginLinearRecording(modelStack, 0);
			FlashStorage::audioClipRecordMargins = marginsSetting;
			if (error != Error::NONE) {
				state = State::WARMING_UP; // Carry on without measuring
				midiLaunchPending = true;
				loopEndsUntilRecording = deluge::freeze::warmupPasses(0);
				return;
			}
		}
		nextPing = 0;
		pingSounding = false;
		state = State::MEASURING;
		break; // processPings() plays the first one at position 0, straight after this

	case State::MEASURING:
		if (pingSounding) {
			sendPing(false);
		}
		audioClip->finishLinearRecording(modelStack, nullptr, 0);
		// Keep the recording for routine() to analyse, without the AudioClip playing it
		measurementSample = audioClip->takeRecordedSample();
		measurementFinishedAt = AudioEngine::audioSampleTimer;
		midiLaunchPending = true;
		beginWarmingUp();
		break;

	case State::WARMING_UP:
		if (--loopEndsUntilRecording > 0) {
			return;
		}
		// Not ready to record until the MIDI clip's playing and the pings are measured - which they will be, as warming
		// up is always a few seconds
		if (midiLaunchPending || measurementSample != nullptr) {
			loopEndsUntilRecording = 1;
			return;
		}
		// Recording takes the loop from here to the next loop end, as when a session clip is launched to record - but
		// later by the latency, so the recording lines up with the notes that made it
		recordingLatency = pingLatencyKnown ? std::clamp(pingLatency, -kMaxEarlySamples, kMaxRecordShiftSamples) : 0;
		audioClip->originalLength = audioClip->loopLength;
		audioClip->isPendingOverdub = false;
		{
			// Record margins whatever the setting: if the latency's still unknown, the audio past the loop's end is
			// what the sample's start and end move into to make up for it
			bool marginsSetting = FlashStorage::audioClipRecordMargins;
			FlashStorage::audioClipRecordMargins = true;
			Error error = audioClip->beginLinearRecording(modelStack, -recordingLatency);
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
		audioClip->finishLinearRecording(modelStack, nullptr, -recordingLatency);
		recordingFinishedAt = AudioEngine::audioSampleTimer;
		state = State::SWAPPING;
		break;

	default:
		break;
	}
}

void ClipFreezer::processPings() {
	if (state != State::MEASURING || numPings <= 0) {
		return;
	}
	int32_t pos = audioClip->lastProcessedPos;
	if (pingSounding && pos >= pingOffPos) {
		sendPing(false);
	}
	if (!pingSounding && nextPing < numPings && pos >= nextPing * pingSpacing) {
		pingOffPos = nextPing * pingSpacing + pingSpacing / 2;
		sendPing(true);
		nextPing++;
	}
	// Make sure the playback handler comes back here for the next one
	int32_t nextEvent = pingSounding            ? pingOffPos
	                    : (nextPing < numPings) ? nextPing * pingSpacing
	                                            : audioClip->loopLength;
	int32_t ticksUntil = nextEvent - pos;
	if (ticksUntil > 0 && ticksUntil < playbackHandler.swungTicksTilNextEvent) {
		playbackHandler.swungTicksTilNextEvent = ticksUntil;
	}
}

bool ClipFreezer::canPing() const {
	auto* instrument = static_cast<MIDIInstrument*>(midiClip->output);
	return !instrument->sendsToInternal() && !instrument->sendsToMPE();
}

void ClipFreezer::choosePings() {
	// The clip's commonest note, as loud as its loudest: something the gear's patch plays, and plays clearly
	int32_t mostNotes = 0;
	int32_t loudest = 1;
	for (int32_t r = 0; r < midiClip->noteRows.getNumElements(); r++) {
		NoteRow* noteRow = midiClip->noteRows.getElement(r);
		int32_t numNotes = noteRow->notes.getNumElements();
		if (noteRow->muted || numNotes == 0) {
			continue;
		}
		if (numNotes > mostNotes) {
			mostNotes = numNotes;
			pingNote = noteRow->getNoteCode();
		}
		for (int32_t n = 0; n < numNotes; n++) {
			loudest = std::max<int32_t>(loudest, noteRow->notes.getElement(n)->velocity);
		}
	}
	pingNote = std::clamp<int32_t>(pingNote, 0, 127);
	pingVelocity = std::clamp<int32_t>(loudest, 1, 127);

	// One every quarter note, a short one, for as many as fit in the loop, up to 16 - and at least two
	int32_t loopLength = midiClip->loopLength;
	pingSpacing = std::max<int32_t>(song->getQuarterNoteLength(), loopLength / 16);
	if (pingSpacing * 2 > loopLength) {
		pingSpacing = loopLength / 2;
	}
	numPings = (pingSpacing > 0) ? loopLength / pingSpacing : 0;
}

void ClipFreezer::sendPing(bool on) {
	auto* instrument = static_cast<MIDIInstrument*>(midiClip->output);
	int32_t channel = instrument->getChannel();
	// As MIDIInstrument::noteOnPostArp() and noteOffPostArp() send a clip's notes
	midiEngine.sendNote(instrument, on, pingNote, on ? pingVelocity : kDefaultLiftValue, channel,
	                    on ? channel : kMIDIOutputFilterNoMPE);
	pingSounding = on;
}

void ClipFreezer::launchMIDIClipInStep() {
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, song, midiClip);
	Output* output = midiClip->output;
	Clip* other = output->getActiveClip();
	if (other != nullptr && other != midiClip && song->isClipActive(other)) {
		other->expectNoFurtherTicks(song);
		other->activeIfNoSolo = false;
	}
	midiClip->activeIfNoSolo = true;
	if (output->getActiveClip() != midiClip) {
		output->setActiveClip(modelStack, PgmChangeSend::ONCE);
	}
	if (playbackHandler.isEitherClockActive()) {
		midiClip->setPos(modelStack, audioClip->getCurrentPosAsIfPlayingInForwardDirection(), true);
		midiClip->resumePlayback(modelStack);
	}
	uiNeedsRendering(&sessionView);
}

void ClipFreezer::releaseMeasurementSample() {
	if (measurementSample != nullptr) {
		measurementSample->removeReason("freeze pings");
		measurementSample = nullptr;
	}
}

void ClipFreezer::analysePings() {
	Sample* sample = measurementSample;
	uint32_t pingTicks[16];
	int32_t count = std::min<int32_t>(numPings, 16);
	for (int32_t i = 0; i < count; i++) {
		pingTicks[i] = i * pingSpacing;
	}
	int32_t latency = 0;
	if (sample != nullptr && sample->fileLoopEndSamples > sample->fileLoopStartSamples
	    && measureLatency(sample, sample->fileLoopStartSamples, sample->fileLoopEndSamples, {pingTicks, (size_t)count},
	                      &latency)) {
		pingLatencyKnown = true;
		pingLatency = latency;
		lastLatencySamples = latency;
	}
	releaseMeasurementSample();
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
	case State::MEASURE_WAIT:
	case State::MEASURING:
		if (!song->isClipActive(audioClip) && safeToRestructure()) {
			cancel(); // Someone stopped it
		}
		break;

	case State::WARMING_UP:
		if (midiLaunchPending) {
			launchMIDIClipInStep();
			midiLaunchPending = false;
		}
		// The pings' recording carries on for its end margin, which measuring needs
		if (measurementSample != nullptr
		    && (int32_t)(AudioEngine::audioSampleTimer - measurementFinishedAt) >= kRecordingTailSamples) {
			analysePings();
		}
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
		// The recording carries on for its end margin, which measuring the latency from it needs
		else if ((int32_t)(AudioEngine::audioSampleTimer - recordingFinishedAt) < kRecordingTailSamples) {}
		else if (safeToRestructure()) {
			if (pingLatencyKnown) {
				showFrozenPopup(recordingLatency, true); // Already recorded in time
			}
			else {
				compensateLatency();
			}
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

size_t ClipFreezer::readRecordingLevels(Sample* sample, uint64_t startSample, std::span<uint32_t> levels) {
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

bool ClipFreezer::measureLatency(Sample* sample, uint64_t loopStart, uint64_t loopEnd, std::span<uint32_t> noteTicks,
                                 int32_t* latency) {
	uint64_t loopSamples = loopEnd - loopStart;
	if (noteTicks.empty() || loopSamples == 0 || midiClip->loopLength <= 0) {
		return false;
	}
	// The search can go this far either side of where the recording's loop starts, as far as there's audio
	int32_t before = std::min<uint64_t>(loopStart, kMaxEarlySamples);
	int32_t after =
	    std::min<uint64_t>(sample->lengthInSamples > loopEnd ? sample->lengthInSamples - loopEnd : 0, kMaxLateSamples);

	size_t numBlocks = (loopSamples + before + after) / deluge::freeze::kLatencyBlockSize;
	auto* levels = static_cast<uint32_t*>(GeneralMemoryAllocator::get().allocLowSpeed(numBlocks * sizeof(uint32_t)));
	if (levels == nullptr) {
		return false;
	}
	// Where the notes start, in samples into the levels
	for (uint32_t& note : noteTicks) {
		note = before + (uint64_t)note * loopSamples / midiClip->loopLength;
	}
	size_t numLevels = readRecordingLevels(sample, loopStart - before, {levels, numBlocks});
	deluge::freeze::LatencyEstimate estimate =
	    deluge::freeze::estimateLatency({levels, numLevels}, noteTicks, before + after);
	delugeDealloc(levels);
	if (!estimate.confident) {
		return false;
	}
	*latency = estimate.samples - before;
	return true;
}

void ClipFreezer::compensateLatency() {
	auto* sample = static_cast<Sample*>(audioClip->sampleHolder.audioFile);
	SampleHolderForClip& holder = audioClip->sampleHolder;
	if (sample == nullptr || holder.endPos <= holder.startPos || midiClip->loopLength <= 0) {
		popup(L10nString::STRING_FOR_FROZEN);
		return;
	}

	// From the clip's own notes
	constexpr size_t kMaxNotes = 1024;
	auto* noteTicks = static_cast<uint32_t*>(GeneralMemoryAllocator::get().allocLowSpeed(kMaxNotes * sizeof(uint32_t)));
	int32_t latency = 0;
	bool measured = false;
	if (noteTicks != nullptr) {
		size_t numNotes = 0;
		for (int32_t r = 0; r < midiClip->noteRows.getNumElements(); r++) {
			NoteRow* noteRow = midiClip->noteRows.getElement(r);
			if (noteRow->muted) {
				continue;
			}
			for (int32_t n = 0; n < noteRow->notes.getNumElements() && numNotes < kMaxNotes; n++) {
				int32_t pos = noteRow->notes.getElement(n)->pos;
				if (pos >= 0 && pos < midiClip->loopLength) {
					noteTicks[numNotes++] = pos;
				}
			}
		}
		measured = measureLatency(sample, holder.startPos, holder.endPos, {noteTicks, numNotes}, &latency);
		delugeDealloc(noteTicks);
	}
	if (measured) {
		lastLatencySamples = latency;
	}
	else if (lastLatencySamples != kLatencyUnknown) {
		latency = lastLatencySamples;
	}
	int32_t before = std::min<uint64_t>(holder.startPos, kMaxEarlySamples);
	int32_t after = std::min<uint64_t>(
	    sample->lengthInSamples > holder.endPos ? sample->lengthInSamples - holder.endPos : 0, kMaxLateSamples);
	latency = std::clamp(latency, -before, after);

	// The loop is the same length, just later (or earlier) in the recording
	holder.startPos += latency;
	holder.endPos += latency;
	audioClip->renderData.xScroll = -1;
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithTimelineCounter* modelStack = setupModelStackWithTimelineCounter(modelStackMemory, song, audioClip);
	audioClip->sampleZoneChanged(modelStack);

	showFrozenPopup(latency, measured);
}

void ClipFreezer::showFrozenPopup(int32_t latency, bool measured) {
	if (!display->haveOLED()) {
		popup(L10nString::STRING_FOR_FROZEN);
		return;
	}
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

void ClipFreezer::cancel() {
	finish(true);
}

void ClipFreezer::finish(bool cancelled) {
	if (pingSounding && currentSong == song && sessionIndexOf(song, midiClip) >= 0) {
		sendPing(false);
	}
	pingSounding = false;
	releaseMeasurementSample();
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
		// Freezing stopped it to measure: start it again, if that hasn't happened yet
		if (midiIndex >= 0 && midiWasPlaying && playbackHandler.isEitherClockActive() && currentPlaybackMode == &session
		    && !song->isClipActive(midiClip)) {
			midiClip->activeIfNoSolo = true;
			session.reSyncClipToSongTicks(midiClip);
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
