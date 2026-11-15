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
#include "model/clip/freeze_warmup.h"
#include "model/clip/instrument_clip.h"
#include "model/instrument/midi_instrument.h"
#include "model/model_stack.h"
#include "model/settings/runtime_feature_settings.h"
#include "model/song/song.h"
#include "playback/mode/session.h"
#include "playback/playback_handler.h"
#include "processing/audio_output.h"
#include <new>

ClipFreezer clipFreezer{};

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
	// The frozen clip takes its MIDI track with it, out of the song, so nothing else may be using that. (As with
	// converting an instrument clip to audio.)
	if (currentSong->getClipWithOutput(clip->output, false, clip) != nullptr
	    || clip->output->clipInstances.getNumElements() != 0) {
		return refuse(L10nString::STRING_FOR_FREEZE_TRACK_HAS_OTHER_CLIPS);
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

	newClip->activeIfNoSolo = true;
	newClip->soloingInSessionMode = clip->soloingInSessionMode;
	if (!playbackHandler.isEitherClockActive()) {
		// Play from the start, with the MIDI clip playing
		clip->activeIfNoSolo = true;
		playbackHandler.setupPlaybackUsingInternalClock();
	}
	else {
		// Start the MIDI clip now if it isn't playing, as a shift-press of its pad would, and the AudioClip with it
		if (!thisSong->isClipActive(clip)) {
			session.toggleClipStatus(clip, &midiIndex, true, 0);
		}
		newClip->setPos(modelStackWithAudioClip, clip->getCurrentPosAsIfPlayingInForwardDirection(), true);
		newClip->resumePlayback(modelStackWithAudioClip);
	}
	beginWarmingUp();

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
		// But taken later by the MIDI track's hardware latency, so the recording lines up with the notes that made it.
		// This is the SampleRecorder's button press latency compensation, the other way round
		recordingLatency =
		    deluge::freeze::latencySamples(static_cast<MIDIInstrument*>(midiClip->output)->hardwareLatencyMs);
		if (audioClip->beginLinearRecording(modelStack, -recordingLatency) != Error::NONE) {
			// routine() sees there's no recording, and cancels
			state = State::SWAPPING;
			return;
		}
		state = State::RECORDING;
		// The MIDI clip stops as the recording ends, and the AudioClip carries on with the same loop
		if (song->isClipActive(midiClip)) {
			session.armClipToStopAction(midiClip);
		}
		break;

	case State::RECORDING:
		audioClip->finishLinearRecording(modelStack, nullptr, -recordingLatency);
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
		else if (safeToRestructure()) {
			// Its MIDI track has to leave the song with it, so not if another clip's been put on that meanwhile
			if (song->getClipWithOutput(midiClip->output, false, midiClip) != nullptr
			    || midiClip->output->clipInstances.getNumElements() != 0) {
				cancel();
			}
			else {
				swapInPlace();
				finish(false);
			}
		}
		// Otherwise the AudioClip's already playing in place of the MIDI clip, and the swap waits for its view to close
		break;

	default:
		break;
	}
}

void ClipFreezer::swapInPlace() {
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
	// Output's clip any more. canFreeze() made sure it was the Output's only clip, so the Output leaves the song with
	// it, and the AudioClip's Output takes its place in the list - which, overall, is as long as before freezing began
	Output* midiOutput = midiClip->output;
	if (midiClip->isActiveOnOutput()) {
		midiOutput->detachActiveClip(song);
	}
	forgetOutputMacros(song, midiOutput);
	Output* audioOutput = audioClip->output;
	int32_t arrangementYScroll = song->arrangementYScroll;
	song->removeOutputFromMainList(audioOutput, false);
	int32_t outputIndex = song->removeOutputFromMainList(midiOutput);
	song->insertOutputAtIndex(audioOutput, outputIndex);
	song->arrangementYScroll = arrangementYScroll - 1; // createNewAudioOutput() added 1, for adding at the start
	audioClip->frozen.clip = midiClip;
	audioClip->frozen.detachedOutput = midiOutput;

	actionLogger.deleteAllLogs();
	if (getRootUI() == &sessionView || view.activeModControllableModelStack.getTimelineCounterAllowNull() == midiClip) {
		view.setActiveModControllableTimelineCounter(audioClip);
	}
	popup(L10nString::STRING_FOR_FROZEN);
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

	// Its Output goes back where the AudioClip's is, which is deleted below: the list stays as long, and as scrolled
	int32_t arrangementYScroll = thisSong->arrangementYScroll;
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
	if (detachedOutput != nullptr) {
		thisSong->arrangementYScroll = arrangementYScroll;
	}

	actionLogger.deleteAllLogs();
	popup(L10nString::STRING_FOR_UNFROZEN);
	if (getCurrentUI() == &sessionView) {
		uiNeedsRendering(&sessionView);
	}
	return true;
}
