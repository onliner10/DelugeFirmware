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

#include "model/clip/preset_lock.h"
#include "model/clip/instrument_clip.h"
#include "model/model_stack.h"
#include "model/song/song.h"
#include "modulation/params/param_descriptor.h"
#include "modulation/params/param_manager.h"
#include "modulation/patch/patch_cable_set.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_instrument.h"

namespace deluge::preset_lock {

namespace {

ParamDescriptor duckingDestination() {
	ParamDescriptor descriptor;
	descriptor.setToHaveParamOnly(params::GLOBAL_VOLUME_POST_REVERB_SEND);
	return descriptor;
}

ModelStackWithThreeMainThings* setupModelStack(void* memory, InstrumentClip* clip) {
	ModelStackWithTimelineCounter* modelStack = setupModelStackWithTimelineCounter(memory, currentSong, clip);
	return modelStack->addOtherTwoThingsButNoNoteRow(clip->output->toModControllable(), &clip->paramManager);
}

int32_t getPatched(ModelStackWithThreeMainThings* modelStack, int32_t p) {
	ModelStackWithAutoParam* withParam = modelStack->getPatchedAutoParamFromId(p);
	return withParam->autoParam ? withParam->autoParam->getCurrentValue() : 0;
}

int32_t getUnpatched(ModelStackWithThreeMainThings* modelStack, int32_t p) {
	ModelStackWithAutoParam* withParam = modelStack->getUnpatchedAutoParamFromId(p);
	return withParam->autoParam ? withParam->autoParam->getCurrentValue() : 0;
}

// Goes through the param collection so the sound hears about the change, but never records automation - the user may
// well be holding shift while recording, which would otherwise delete automation instead.
void setParam(ModelStackWithAutoParam* withParam, int32_t value) {
	if (withParam->autoParam) {
		withParam->autoParam->setCurrentValueWithNoReversionOrRecording(withParam, value);
	}
}

void setPatched(ModelStackWithThreeMainThings* modelStack, int32_t p, int32_t value) {
	setParam(modelStack->getPatchedAutoParamFromId(p), value);
}

void setUnpatched(ModelStackWithThreeMainThings* modelStack, int32_t p, int32_t value) {
	setParam(modelStack->getUnpatchedAutoParamFromId(p), value);
}

void applyArp(Snapshot const& snapshot, InstrumentClip* clip, Sound* sound, ModelStackWithThreeMainThings* modelStack) {
	bool wasOff = clip->arpSettings.mode == ArpMode::OFF;
	bool willBeOff = snapshot.arpSettings.mode == ArpMode::OFF;

	// Must switch off all notes when switching arp on / off
	if (wasOff != willBeOff && clip->isActiveOnOutput()) {
		sound->allNotesOff(modelStack, sound->getArp());
		sound->reassessRenderSkippingStatus(modelStack->addSoundFlags());
	}

	clip->arpSettings.cloneFrom(&snapshot.arpSettings);
	clip->arpSettings.flagForceArpRestart = true;

	setPatched(modelStack, params::GLOBAL_ARP_RATE, snapshot.arpRate);
	for (int32_t i = 0; i < kNumUnpatchedArpParams; i++) {
		setUnpatched(modelStack, params::UNPATCHED_FIRST_ARP_PARAM + i, snapshot.arpUnpatched[i]);
	}
}

void applySidechain(Snapshot const& snapshot, InstrumentClip* clip, Sound* sound,
                    ModelStackWithThreeMainThings* modelStack) {
	sound->sideChainSendLevel = snapshot.sidechainSendLevel;
	sound->sidechain.attack = snapshot.sidechainAttack;
	sound->sidechain.release = snapshot.sidechainRelease;
	sound->sidechain.syncType = snapshot.sidechainSyncType;
	sound->sidechain.syncLevel = snapshot.sidechainSyncLevel;
	setUnpatched(modelStack, params::UNPATCHED_SIDECHAIN_SHAPE, snapshot.sidechainShape);

	// Volume ducking is a patch cable from the sidechain to the post-reverb-send volume.
	PatchCableSet* patchCableSet = clip->paramManager.getPatchCableSet();
	bool cableExistsNow = patchCableSet->getPatchCableIndex(PatchSource::SIDECHAIN, duckingDestination()) != 255;
	if (snapshot.duckingCableExists || cableExistsNow) {
		// This creates the cable if needed
		ModelStackWithAutoParam* withParam = modelStack->getPatchCableAutoParamFromId(
		    PatchCableSet::getParamId(duckingDestination(), PatchSource::SIDECHAIN));
		setParam(withParam, snapshot.duckingCableExists ? snapshot.duckingCableValue : 0);

		uint8_t c = patchCableSet->getPatchCableIndex(PatchSource::SIDECHAIN, duckingDestination());
		if (c != 255 && snapshot.duckingCableExists) {
			patchCableSet->patchCables[c].polarity = snapshot.duckingCablePolarity;
		}
	}

	AudioEngine::mustUpdateReverbParamsBeforeNextRender = true;
}

void applyDelay(Snapshot const& snapshot, Sound* sound, ModelStackWithThreeMainThings* modelStack) {
	setPatched(modelStack, params::GLOBAL_DELAY_RATE, snapshot.delayRate);
	setPatched(modelStack, params::GLOBAL_DELAY_FEEDBACK, snapshot.delayFeedback);
	sound->delay.pingPong = snapshot.delayPingPong;
	sound->delay.analog = snapshot.delayAnalog;
	sound->delay.syncType = snapshot.delaySyncType;
	sound->delay.syncLevel = snapshot.delaySyncLevel;
}

} // namespace

bool canLock(InstrumentClip* clip) {
	return clip != nullptr && clip->output != nullptr && clip->output->type == OutputType::SYNTH
	       && clip->paramManager.containsAnyMainParamCollections();
}

void capture(Snapshot& snapshot, InstrumentClip* clip) {
	snapshot.valid = false;
	if (!canLock(clip)) {
		return;
	}

	auto* sound = static_cast<SoundInstrument*>(clip->output);
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithThreeMainThings* modelStack = setupModelStack(modelStackMemory, clip);

	// Arp
	snapshot.arpSettings.cloneFrom(&clip->arpSettings);
	snapshot.arpRate = getPatched(modelStack, params::GLOBAL_ARP_RATE);
	for (int32_t i = 0; i < kNumUnpatchedArpParams; i++) {
		snapshot.arpUnpatched[i] = getUnpatched(modelStack, params::UNPATCHED_FIRST_ARP_PARAM + i);
	}

	// Mix
	snapshot.volume = getPatched(modelStack, params::GLOBAL_VOLUME_POST_FX);
	snapshot.pan = getPatched(modelStack, params::LOCAL_PAN);

	// Sidechain
	snapshot.sidechainSendLevel = sound->sideChainSendLevel;
	snapshot.sidechainAttack = sound->sidechain.attack;
	snapshot.sidechainRelease = sound->sidechain.release;
	snapshot.sidechainSyncType = sound->sidechain.syncType;
	snapshot.sidechainSyncLevel = sound->sidechain.syncLevel;
	snapshot.sidechainShape = getUnpatched(modelStack, params::UNPATCHED_SIDECHAIN_SHAPE);

	PatchCableSet* patchCableSet = clip->paramManager.getPatchCableSet();
	uint8_t c = patchCableSet->getPatchCableIndex(PatchSource::SIDECHAIN, duckingDestination());
	snapshot.duckingCableExists = (c != 255);
	if (snapshot.duckingCableExists) {
		snapshot.duckingCableValue = patchCableSet->patchCables[c].param.getCurrentValue();
		snapshot.duckingCablePolarity = patchCableSet->patchCables[c].polarity;
	}

	// Reverb
	snapshot.reverbAmount = getPatched(modelStack, params::GLOBAL_REVERB_AMOUNT);

	// Delay
	snapshot.delayRate = getPatched(modelStack, params::GLOBAL_DELAY_RATE);
	snapshot.delayFeedback = getPatched(modelStack, params::GLOBAL_DELAY_FEEDBACK);
	snapshot.delayPingPong = sound->delay.pingPong;
	snapshot.delayAnalog = sound->delay.analog;
	snapshot.delaySyncType = sound->delay.syncType;
	snapshot.delaySyncLevel = sound->delay.syncLevel;

	snapshot.valid = true;
}

void apply(Snapshot const& snapshot, uint8_t modules, InstrumentClip* clip) {
	if (!snapshot.valid || modules == 0 || !canLock(clip)) {
		return;
	}

	auto* sound = static_cast<SoundInstrument*>(clip->output);
	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithThreeMainThings* modelStack = setupModelStack(modelStackMemory, clip);

	if (modules & ARP) {
		applyArp(snapshot, clip, sound, modelStack);
	}
	if (modules & MIX) {
		setPatched(modelStack, params::GLOBAL_VOLUME_POST_FX, snapshot.volume);
		setPatched(modelStack, params::LOCAL_PAN, snapshot.pan);
	}
	if (modules & SIDECHAIN) {
		applySidechain(snapshot, clip, sound, modelStack);
	}
	if (modules & REVERB) {
		setPatched(modelStack, params::GLOBAL_REVERB_AMOUNT, snapshot.reverbAmount);
	}
	if (modules & DELAY) {
		applyDelay(snapshot, sound, modelStack);
	}
}

uint8_t moduleForPad(int32_t x, int32_t y) {
	switch (x) {
	case 6: // Level and pan
		return (y == 0 || y == 3) ? MIX : 0;
	case 10: // Sidechain column
		return (y >= 0 && y <= 5) ? SIDECHAIN : 0;
	case 11: // Arp column
		return (y >= 0 && y <= 4) ? ARP : 0;
	case 13: // Reverb amount (the rest of the reverb column is song-wide)
		return (y == 3) ? REVERB : 0;
	case 14: // Delay column
		return (y >= 0 && y <= 4) ? DELAY : 0;
	case 15: // Arp randomizer
		return (y >= 1 && y <= 3) ? ARP : 0;
	default:
		return 0;
	}
}

RGB padColour(uint8_t module, bool locked) {
	RGB colour;
	switch (module) {
	case ARP:
		colour = RGB{.r = 255, .g = 96, .b = 0};
		break;
	case MIX:
		colour = RGB{.r = 255, .g = 255, .b = 255};
		break;
	case SIDECHAIN:
		colour = RGB{.r = 0, .g = 255, .b = 48};
		break;
	case REVERB:
		colour = RGB{.r = 0, .g = 112, .b = 255};
		break;
	case DELAY:
		colour = RGB{.r = 192, .g = 0, .b = 255};
		break;
	default:
		return RGB{};
	}
	return locked ? colour : colour.dim(5);
}

char const* moduleName(uint8_t module) {
	switch (module) {
	case ARP:
		return "Arp";
	case MIX:
		return "Level/pan";
	case SIDECHAIN:
		return "Sidechain";
	case REVERB:
		return "Reverb";
	case DELAY:
		return "Delay";
	default:
		return "";
	}
}

} // namespace deluge::preset_lock
