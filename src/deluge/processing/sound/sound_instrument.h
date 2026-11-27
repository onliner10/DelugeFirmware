/*
 * Copyright © 2014-2023 Synthstrom Audible Limited
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
#include "model/instrument/melodic_instrument.h"
#include "modulation/arpeggiator.h"
#include "processing/sound/sound.h"

class ParamManagerForTimeline;
class ParamManagerForTimeline;
class ModelStack;
class ModelStackWithThreeMainThings;

class SoundInstrument final : public Sound, public MelodicInstrument {
public:
	SoundInstrument();
	bool writeDataToFile(Serializer& writer, Clip* clipForSavingOutputOnly, Song* song) override;
	Error readFromFile(Deserializer& reader, Song* song, Clip* clip, int32_t readAutomationUpToPos) override;
	void cutAllSound() override;
	bool noteIsOn(int32_t noteCode, bool resetTimeEntered);

	void renderOutput(ModelStack* modelStack, std::span<StereoSample> buffer, int32_t* reverbBuffer,
	                  int32_t reverbAmountAdjust, int32_t sideChainHitPending, bool shouldLimitDelayFeedback,
	                  bool isClipActive) override;

	// A timelineCounter is required
	void offerReceivedCCToLearnedParams(MIDICable& cable, uint8_t channel, uint8_t ccNumber, uint8_t value,
	                                    ModelStackWithTimelineCounter* modelStack) override {
		Sound::offerReceivedCCToLearnedParamsForClip(cable, channel, ccNumber, value, modelStack);
	}
	bool offerReceivedPitchBendToLearnedParams(MIDICable& cable, uint8_t channel, uint8_t data1, uint8_t data2,
	                                           ModelStackWithTimelineCounter* modelStack) override {
		return Sound::offerReceivedPitchBendToLearnedParams(cable, channel, data1, data2, modelStack);
	}

	Error loadAllAudioFiles(bool mayActuallyReadFiles) override;
	/// For previewing presets quickly: of a big multisample, loads only the samples that the clips of notesFrom (the
	/// Instrument this one is replacing) play, and leaves the rest to loadLazyAudioFilesTask().
	Error loadAudioFilesForPreview(Song* song, Output* notesFrom);
	/// While loading song, instead of loadAllAudioFiles(): of a big multisample, loads only the zones its Clips play,
	/// unless it needs them all (see getSampleNeed()), and leaves the rest to loadLazyAudioFilesTask().
	Error loadAudioFilesLazily(Song* song, bool mayActuallyReadFiles);
	/// Loads one sample left out by loadAudioFilesForPreview() or loadAudioFilesLazily(): first one its Clips or the
	/// visible audition pads play, then, if it needs all of them (see getSampleNeed()) and mayLoadTheRest, any.
	/// Returns whether it loaded one, or took the time to find there's none it needs.
	bool loadLazyAudioFile(Song* song, bool mayLoadTheRest);
	/// Loads the zone note plays, if left out, and gets its start into RAM, so it can be played straight away
	void loadZonesForNoteNow(Song* song, int32_t note);
	/// When loadLazyAudioFile() last found every zone its Clips play loaded, in AudioEngine::audioSampleTimer units,
	/// so it doesn't check again for a while. 0 if not. Notes can change off screen too (undo, transposing the song).
	uint32_t playedZonesLoadedAt = 0;
	/// Whether the samples left out came from loading the Song (so live in its folders), rather than from previewing
	bool lazyFromSong = false;
	bool hasLazyAudioFiles() { return sources[0].hasLazySamples() || sources[1].hasLazySamples(); }
	void resyncLFOs(bool reset_state = false) override;
	ModControllable* toModControllable() override;
	bool setActiveClip(ModelStackWithTimelineCounter* modelStack, PgmChangeSend maySendMIDIPGMs) override;
	void setupPatchingForAllParamManagers(Song* song) override;
	void setupPatching(ModelStackWithTimelineCounter* modelStack) override;

	void deleteBackedUpParamManagers(Song* song) override;
	void polyphonicExpressionEventOnChannelOrNote(int32_t newValue, int32_t expressionDimension,
	                                              int32_t channelOrNoteNumber,
	                                              MIDICharacteristic whichCharacteristic) override;
	void monophonicExpressionEvent(int32_t newValue, int32_t expressionDimension) override;

	void sendNote(ModelStackWithThreeMainThings* modelStack, bool isOn, int32_t noteCode, int16_t const* mpeValues,
	              int32_t fromMIDIChannel, uint8_t velocity, uint32_t sampleSyncLength, int32_t ticksLate,
	              uint32_t samplesLate) override;

	ArpeggiatorSettings* getArpSettings(InstrumentClip* clip = nullptr) override;
	bool readTagFromFile(Deserializer& reader, char const* tagName) override;

	void prepareForHibernationOrDeletion() override;
	void compensateInstrumentVolumeForResonance(ModelStackWithThreeMainThings* modelStack) override;
	bool isSkippingRendering() override { return skippingRendering; }
	void loadCrucialAudioFilesOnly() override;
	void beenEdited(bool shouldMoveToEmptySlot = true) override;
	int32_t doTickForwardForArp(ModelStack* modelStack, int32_t currentPos) override;
	void setupWithoutActiveClip(ModelStack* modelStack) override;
	void getThingWithMostReverb(Sound** soundWithMostReverb, ParamManager** paramManagerWithMostReverb,
	                            GlobalEffectableForClip** globalEffectableWithMostReverb,
	                            int32_t* highestReverbAmountFound) override;
	uint8_t* getModKnobMode() override { return &modKnobMode; }
	ArpeggiatorBase* getArp() override;
	char const* getXMLTag() override { return "sound"; }
	const char* getName() override { return name.get(); }

	ArpeggiatorSettings defaultArpSettings;
};

/// How much of its samples an Instrument needs loaded: none while it can't be heard soon, the zones its Clips play
/// while one of them is active or armed, and all of them while it's on screen (audition pads), has MIDI learned to it
/// or the arranger is in use.
enum class SampleNeed : uint8_t { NONE, PLAYED_ZONES, ALL };
SampleNeed getSampleNeed(Song* song, Output* output);
/// Whether the samples clip plays could start sounding now (see Source::zonesReadyFor()). A Clip launching before they
/// are would have silent notes until they've loaded.
bool clipSamplesReady(Clip* clip);
/// Scheduler task: loads, one at a time, the samples that loadAudioFilesForPreview() or loadAudioFilesLazily() left
/// out, as the Instruments need them (see SoundInstrument::loadLazyAudioFile()).
void loadLazyAudioFilesTask();
/// Lets the start-of-sample Clusters of Instruments that can't be heard soon be stolen (Sound::setSampleStartsIdle()):
/// in session mode, those with no active or armed Clip that aren't the one on screen and have no MIDI learned to them.
void idleSampleStartsTask();
/// What idleSampleStartsTask() does, for song, now: e.g. while loading it, before trying again to load samples that
/// didn't fit in RAM
void releaseIdleSampleStarts(Song* song);
