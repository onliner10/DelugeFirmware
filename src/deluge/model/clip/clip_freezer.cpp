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
#include "gui/ui/ui.h"
#include "gui/views/session_view.h"
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
	    || clip->output->type != OutputType::MIDI_OUT || sessionIndexOf(currentSong, clip) < 0) {
		return refuse(L10nString::STRING_FOR_FREEZE_MIDI_CLIPS_ONLY);
	}
	if (clipFreezer.state != State::IDLE || clip->getCurrentlyRecordingLinearly() || clip->isPendingOverdub) {
		return refuse(L10nString::STRING_FOR_FREEZE_BUSY);
	}
	if (playbackHandler.isEitherClockActive() && currentPlaybackMode != &session) {
		return refuse(L10nString::STRING_FOR_FREEZE_NOT_IN_ARRANGER);
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

void ClipFreezer::start(InstrumentClip* clip) {
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
	// What the gear plays comes in from outside: not from an internal source like resampling, if that's the default
	if (newOutput->inputChannel <= AudioInputChannel::NONE
	    || newOutput->inputChannel >= AUDIO_INPUT_CHANNEL_FIRST_INTERNAL_OPTION) {
		newOutput->inputChannel = AudioInputChannel::STEREO;
	}
	newOutput->mode = AudioOutputMode::player; // No monitoring - the gear is already heard wherever it's heard

	// Like Song::replaceInstrumentClipWithAudioClip(), but alongside the MIDI clip
	auto* newClip = new (clipMemory) AudioClip();
	newClip->cloneFrom(clip);
	newClip->armState = ArmState::OFF;
	newClip->armedForRecording = false; // So a press of RECORD can't start it recording anything else
	newClip->activeIfNoSolo = true;
	newClip->soloingInSessionMode = clip->soloingInSessionMode;

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
	thisSong->sessionClips.insertClipAtIndex(newClip, midiIndex + 1);
	actionLogger.deleteAllLogs(); // The Clip was added without an Action to undo it

	song = thisSong;
	midiClip = clip;
	audioClip = newClip;
	uint64_t loopLengthInSamples = (uint64_t)clip->loopLength * playbackHandler.getTimePerInternalTick();
	loopEndsUntilRecording = deluge::freeze::warmupPasses(loopLengthInSamples);
	state = State::WARMING_UP;

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

	popup(L10nString::STRING_FOR_FREEZE_WARMING_UP);
	uiNeedsRendering(&sessionView);
}

void ClipFreezer::loopEnded(ModelStackWithTimelineCounter* modelStack) {
	// Someone took the MIDI clip away: freezing stops, leaving the AudioClip as it is
	if (currentSong != song || sessionIndexOf(song, midiClip) < 0) {
		if (audioClip->getCurrentlyRecordingLinearly()) {
			audioClip->abortRecording();
		}
		stop();
		popup(L10nString::STRING_FOR_FREEZE_CANCELLED);
		return;
	}

	switch (state) {
	case State::WARMING_UP:
		// If the MIDI clip's been stopped (the pad hold that opens Clip Settings does that), wait for it to play again
		if (!song->isClipActive(midiClip) || --loopEndsUntilRecording > 0) {
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
			stop();
			popup(L10nString::STRING_FOR_FREEZE_CANCELLED);
			return;
		}
		state = State::RECORDING;
		// The MIDI clip stops as the recording ends, and the AudioClip carries on with the same loop
		session.armClipToStopAction(midiClip);
		break;

	case State::RECORDING:
		if (!audioClip->getCurrentlyRecordingLinearly()) {
			// Playback was stopped part way through, which aborted it: record the next loop instead
			loopEndsUntilRecording = 1;
			state = State::WARMING_UP;
			return;
		}
		{
			audioClip->finishLinearRecording(modelStack, nullptr, -recordingLatency);
			bool recorded = audioClip->sampleHolder.audioFile != nullptr; // Not if the recording had to be aborted
			stop();
			popup(recorded ? L10nString::STRING_FOR_FROZEN : L10nString::STRING_FOR_FREEZE_CANCELLED);
			break;
		}

	default:
		break;
	}
}

void ClipFreezer::cancel() {
	if (state == State::IDLE) {
		return;
	}
	int32_t audioIndex = sessionIndexOf(song, audioClip);
	if (currentSong == song && audioIndex >= 0) {
		audioClip->abortRecording();
		if (audioClip->armState != ArmState::OFF) {
			session.cancelArmingForClip(audioClip, &audioIndex);
		}
		song->removeSessionClipLowLevel(audioClip, audioIndex);
		song->deleteClipObject(audioClip, false, InstrumentRemoval::DELETE); // Calls forget()
		int32_t midiIndex = sessionIndexOf(song, midiClip);
		if (midiIndex >= 0 && midiClip->armState != ArmState::OFF) {
			session.cancelArmingForClip(midiClip, &midiIndex); // It was only going to stop for us
		}
		actionLogger.deleteAllLogs();
		uiNeedsRendering(&sessionView);
	}
	stop();
	popup(L10nString::STRING_FOR_FREEZE_CANCELLED);
}

void ClipFreezer::forget(AudioClip const* clip) {
	if (clip == audioClip) {
		stop();
	}
}

void ClipFreezer::stop() {
	state = State::IDLE;
	song = nullptr;
	midiClip = nullptr;
	audioClip = nullptr;
}
