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

#include "processing/sound/sound_instrument.h"
#include "definitions_cxx.hpp"
#include "dsp/stereo_sample.h"
#include "gui/views/arranger_view.h"
#include "gui/views/view.h"
#include "model/clip/instrument_clip.h"
#include "model/drum/drum.h"
#include "model/instrument/kit.h"
#include "model/model_stack.h"
#include "model/note/note_row.h"
#include "model/song/clip_iterators.h"
#include "model/song/song.h"
#include "model/voice/voice.h"
#include "modulation/arpeggiator.h"
#include "modulation/params/param_manager.h"
#include "modulation/params/param_set.h"
#include "modulation/patch/patch_cable_set.h"
#include "playback/playback_handler.h"
#include "processing/engines/audio_engine.h"
#include "processing/sound/sound_drum.h"
#include "storage/audio/audio_file_manager.h"
#include "storage/storage_manager.h"
#include "util/misc.h"

namespace params = deluge::modulation::params;

SoundInstrument::SoundInstrument() : MelodicInstrument(OutputType::SYNTH) {
}

bool SoundInstrument::writeDataToFile(Serializer& writer, Clip* clipForSavingOutputOnly, Song* song) {

	// MelodicInstrument::writeDataToFile(writer, clipForSavingOutputOnly, song); // Nope, this gets called within the
	// below call
	writeMelodicInstrumentAttributesToFile(writer, clipForSavingOutputOnly, song);

	ParamManager* paramManager;

	// If saving Output only...
	if (clipForSavingOutputOnly) {
		paramManager = &clipForSavingOutputOnly->paramManager;

		// Or if saving Song...
	}
	else {

		// If no activeClip, that means no Clip has this Output, so there should be a backedUpParamManager that we
		// should use
		if (!activeClip) {
			paramManager = song->getBackedUpParamManagerPreferablyWithClip(this, NULL);
		}
		else {
			paramManager = nullptr;
		}
	}

	Sound::writeToFile(writer, clipForSavingOutputOnly == nullptr, paramManager,
	                   clipForSavingOutputOnly ? &((InstrumentClip*)clipForSavingOutputOnly)->arpSettings : nullptr,
	                   NULL);

	MelodicInstrument::writeMelodicInstrumentTagsToFile(writer, clipForSavingOutputOnly, song);

	return true;
}

// arpSettings optional - no need if you're loading a new V2.0 song where Instruments are all separate from Clips and
// won't store any arp stuff
Error SoundInstrument::readFromFile(Deserializer& reader, Song* song, Clip* clip, int32_t readAutomationUpToPos) {

	char modelStackMemory[MODEL_STACK_MAX_SIZE];
	ModelStackWithModControllable* modelStack =
	    setupModelStackWithSong(modelStackMemory, song)->addTimelineCounter(clip)->addModControllableButNoNoteRow(this);

	return Sound::readFromFile(reader, modelStack, readAutomationUpToPos, &defaultArpSettings);
}

void SoundInstrument::cutAllSound() {
	Sound::killAllVoices();
}

void SoundInstrument::renderOutput(ModelStack* modelStack, std::span<StereoSample> output, int32_t* reverbBuffer,
                                   int32_t reverbAmountAdjust, int32_t sideChainHitPending,
                                   bool shouldLimitDelayFeedback, bool isClipActive) {
	// this should only happen in the rare case that this is called while replacing an instrument but after the clips
	// have been cleared
	if (!activeClip) [[unlikely]] {
		return;
	}
	ModelStackWithThreeMainThings* modelStackWithThreeMainThings =
	    modelStack->addTimelineCounter(activeClip)
	        ->addOtherTwoThingsButNoNoteRow(this, getParamManager(modelStack->song));

	if (skippingRendering) {
		compressor.reset();
		compressor.gainReduction = 0;
	}
	else {
		Sound::render(modelStackWithThreeMainThings, output, reverbBuffer, sideChainHitPending, reverbAmountAdjust,
		              shouldLimitDelayFeedback, kMaxSampleValue, recorder);
	}

	if (playbackHandler.isEitherClockActive() && !playbackHandler.ticksLeftInCountIn && isClipActive) {

		// No time to call the proper function and do error checking, sorry.
		ParamCollectionSummary* patchedParamsSummary = &modelStackWithThreeMainThings->paramManager->summaries[1];
		bool anyInterpolating = false;
		if constexpr (params::kNumParams > 64) {
			anyInterpolating = patchedParamsSummary->whichParamsAreInterpolating[0]
			                   || patchedParamsSummary->whichParamsAreInterpolating[1]
			                   || patchedParamsSummary->whichParamsAreInterpolating[2];
		}
		else {
			anyInterpolating = patchedParamsSummary->whichParamsAreInterpolating[0]
			                   || patchedParamsSummary->whichParamsAreInterpolating[1];
		}
		if (anyInterpolating) {
yesTickParamManagerForClip:
			modelStackWithThreeMainThings->paramManager->toForTimeline()->tickSamples(output.size(),
			                                                                          modelStackWithThreeMainThings);
		}
		else {

			// Try other options too.

			// No time to call the proper function and do error checking, sorry.
			ParamCollectionSummary* unpatchedParamsSummary = &modelStackWithThreeMainThings->paramManager->summaries[0];
			if constexpr (params::UNPATCHED_SOUND_MAX_NUM > 32) {
				if (unpatchedParamsSummary->whichParamsAreInterpolating[0]
				    || unpatchedParamsSummary->whichParamsAreInterpolating[1]) {
					goto yesTickParamManagerForClip;
				}
			}
			else {
				if (unpatchedParamsSummary->whichParamsAreInterpolating[0]) {
					goto yesTickParamManagerForClip;
				}
			}

			// No time to call the proper function and do error checking, sorry.
			ParamCollectionSummary* patchCablesSummary = &modelStackWithThreeMainThings->paramManager->summaries[2];
			if constexpr (kMaxNumPatchCables > 32) {
				if (patchCablesSummary->whichParamsAreInterpolating[0]
				    || patchCablesSummary->whichParamsAreInterpolating[1]) {
					goto yesTickParamManagerForClip;
				}
			}
			else {
				if (patchCablesSummary->whichParamsAreInterpolating[0]) {
					goto yesTickParamManagerForClip;
				}
			}

			// No time to call the proper function and do error checking, sorry.
			ParamCollectionSummary* expressionParamsSummary =
			    &modelStackWithThreeMainThings->paramManager->summaries[3];
			if constexpr (kNumExpressionDimensions > 32) {
				if (expressionParamsSummary->whichParamsAreInterpolating[0]
				    || expressionParamsSummary->whichParamsAreInterpolating[1]) {
					goto yesTickParamManagerForClip;
				}
			}
			else {
				if (expressionParamsSummary->whichParamsAreInterpolating[0]) {
					goto yesTickParamManagerForClip;
				}
			}
		}

		// Do the ParamManagers of each NoteRow, too
		for (int32_t i = 0; i < ((InstrumentClip*)activeClip)->noteRows.getNumElements(); i++) {
			NoteRow* thisNoteRow = ((InstrumentClip*)activeClip)->noteRows.getElement(i);
			// No time to call the proper function and do error checking, sorry.
			ParamCollectionSummary* expressionParamsSummary = &thisNoteRow->paramManager.summaries[0];
			bool result = false;
			if constexpr (kNumExpressionDimensions > 32) {
				result = expressionParamsSummary->whichParamsAreInterpolating[0]
				         || expressionParamsSummary->whichParamsAreInterpolating[1];
			}
			else {
				result = expressionParamsSummary->whichParamsAreInterpolating[0];
			}
			if (result) {
				modelStackWithThreeMainThings->setNoteRow(thisNoteRow, thisNoteRow->y);
				modelStackWithThreeMainThings->paramManager = &thisNoteRow->paramManager;
				thisNoteRow->paramManager.tickSamples(output.size(), modelStackWithThreeMainThings);
			}
		}
	}
}

Error SoundInstrument::loadAllAudioFiles(bool mayActuallyReadFiles) {

	bool doingAlternatePath =
	    mayActuallyReadFiles && (audioFileManager.alternateLoadDirStatus == AlternateLoadDirStatus::NONE_SET);
	if (doingAlternatePath) {
		Error error = setupDefaultAudioFileDir();
		if (error != Error::NONE) {
			return error;
		}
	}

	Error error = Sound::loadAllAudioFiles(mayActuallyReadFiles);

	if (doingAlternatePath) {
		audioFileManager.thingFinishedLoading();
	}

	return error;
}

namespace {

// When loadAudioFilesForPreview() was last called, in AudioEngine::audioSampleTimer units
uint32_t lastPreviewTime = 0;
// How long after the last preset change before loading the rest of a multisample
constexpr uint32_t kLazyLoadRestDelay = kSampleRate;

// A small set of notes, transposed the way Voice::noteOn() picks a zone
class PreviewNotes {
public:
	explicit PreviewNotes(int32_t transpose) : transpose_(transpose) {}
	/// Returns false if full
	bool add(int32_t note) {
		note += transpose_;
		for (size_t i = 0; i < size_; i++) {
			if (notes_[i] == note) {
				return true;
			}
		}
		if (size_ == notes_.size()) {
			return false;
		}
		notes_[size_++] = static_cast<int16_t>(note);
		return true;
	}
	std::span<const int16_t> span() const { return {notes_.data(), size_}; }

private:
	std::array<int16_t, 256> notes_;
	size_t size_ = 0;
	int32_t transpose_;
};

// Adds the notes that output's Clips play, including any octaves the arpeggiator adds. Returns false if they don't fit.
bool addPlayedNotes(Song* song, Output* output, PreviewNotes& notes) {
	for (Clip* clip : AllClips::everywhere(song)) {
		if (clip->output != output || clip->type != ClipType::INSTRUMENT) {
			continue;
		}
		auto* instrumentClip = static_cast<InstrumentClip*>(clip);
		int32_t numOctaves =
		    (instrumentClip->arpSettings.mode == ArpMode::OFF) ? 1 : instrumentClip->arpSettings.numOctaves;
		for (int32_t i = 0; i < instrumentClip->noteRows.getNumElements(); i++) {
			NoteRow* noteRow = instrumentClip->noteRows.getElement(i);
			if (noteRow->hasNoNotes()) {
				continue;
			}
			for (int32_t o = 0; o < numOctaves; o++) {
				if (!notes.add(noteRow->y + o * 12)) {
					return false;
				}
			}
		}
	}
	return true;
}

// Sets up loading from song's folders, as loading the Song itself does. Call audioFileManager.thingFinishedLoading()
// after.
Error setupSongAudioFileDir(Song* song) {
	String name;
	Error error = name.set(song->name.get());
	if (error != Error::NONE) {
		return error;
	}
	size_t length = name.getLength();
	if (length > 4 && !strcasecmp(name.get() + length - 4, ".XML")) {
		error = name.shorten(length - 4);
		if (error != Error::NONE) {
			return error;
		}
	}
	error = audioFileManager.setupAlternateAudioFileDir(audioFileManager.alternateAudioFileLoadPath,
	                                                    song->dirPath.get(), name.get());
	if (error != Error::NONE) {
		return error;
	}
	audioFileManager.thingBeginningLoading(ThingType::SONG);
	return Error::NONE;
}

} // namespace

bool clipSamplesReady(Clip* clip) {
	if (clip->type != ClipType::INSTRUMENT) {
		return true;
	}
	auto* instrumentClip = static_cast<InstrumentClip*>(clip);
	if (clip->output->type == OutputType::KIT) {
		for (int32_t i = 0; i < instrumentClip->noteRows.getNumElements(); i++) {
			NoteRow* noteRow = instrumentClip->noteRows.getElement(i);
			if (noteRow->drum != nullptr && noteRow->drum->type == DrumType::SOUND && !noteRow->hasNoNotes()
			    && !static_cast<SoundDrum*>(noteRow->drum)->sampleZonesReadyFor({}, true)) {
				return false;
			}
		}
		return true;
	}
	if (clip->output->type != OutputType::SYNTH) {
		return true;
	}
	auto* synth = static_cast<SoundInstrument*>(clip->output);
	PreviewNotes notes(synth->transpose);
	int32_t numOctaves =
	    (instrumentClip->arpSettings.mode == ArpMode::OFF) ? 1 : instrumentClip->arpSettings.numOctaves;
	bool fits = true;
	for (int32_t i = 0; fits && i < instrumentClip->noteRows.getNumElements(); i++) {
		NoteRow* noteRow = instrumentClip->noteRows.getElement(i);
		if (noteRow->hasNoNotes()) {
			continue;
		}
		for (int32_t o = 0; fits && o < numOctaves; o++) {
			fits = notes.add(noteRow->y + o * 12);
		}
	}
	if (fits && notes.span().empty()) {
		return true;
	}
	return synth->sampleZonesReadyFor(notes.span(), !fits);
}

SampleNeed getSampleNeed(Song* song, Output* output) {
	// The arrangement can start any of its Clips with no arming beforehand to give us time to load, so anything in it
	// is always kept loaded. It plays nothing else, so being in the arranger needs nothing more - except the row being
	// auditioned there.
	if (output->clipInstances.getNumElements()
	    || (getRootUI() == &arrangerView && output == arrangerView.lastAuditionedOutput)) {
		return SampleNeed::ALL;
	}
	// The one shown or last shown, whose audition pads may be played, and anything MIDI may play any time
	Clip* currentClip = song->getCurrentClip();
	if ((currentClip != nullptr && currentClip->output == output)
	    || static_cast<Instrument*>(output)->midiInput.containsSomething()) {
		return SampleNeed::ALL;
	}
	bool anySoloing = song->getAnyClipsSoloing();
	for (int32_t c = 0; c < song->sessionClips.getNumElements(); c++) {
		Clip* clip = song->sessionClips.getClipAtIndex(c);
		if (clip->output == output
		    && (clip->soloingInSessionMode || (clip->activeIfNoSolo && !anySoloing)
		        || clip->armState != ArmState::OFF)) {
			return SampleNeed::PLAYED_ZONES;
		}
	}
	return SampleNeed::NONE;
}

Error SoundInstrument::loadAudioFilesForPreview(Song* song, Output* notesFrom) {
	lastPreviewTime = AudioEngine::audioSampleTimer;

	// The notes that the Clips will play, including any octaves the arpeggiator adds
	PreviewNotes notes(transpose);
	bool fits = (notesFrom != nullptr) && addPlayedNotes(song, notesFrom, notes);
	if (!fits) {
		return loadAllAudioFiles(true);
	}

	bool doingAlternatePath = (audioFileManager.alternateLoadDirStatus == AlternateLoadDirStatus::NONE_SET);
	if (doingAlternatePath) {
		Error error = setupDefaultAudioFileDir();
		if (error != Error::NONE) {
			return error;
		}
	}

	Error error = Error::NONE;
	for (int32_t s = 0; s < kNumSources && error == Error::NONE; s++) {
		if (sources[s].oscType == OscType::SAMPLE || sources[s].oscType == OscType::WAVETABLE) {
			error = sources[s].loadSamplesForNotes(notes.span());
		}
	}

	if (doingAlternatePath) {
		audioFileManager.thingFinishedLoading();
	}
	return error;
}

Error SoundInstrument::loadAudioFilesLazily(Song* song, bool mayActuallyReadFiles) {
	SampleNeed need = getSampleNeed(song, this);
	if (!mayActuallyReadFiles || need == SampleNeed::ALL) {
		return loadAllAudioFiles(mayActuallyReadFiles);
	}
	// Even if its Clips aren't playing, the zones they play are loaded, so starting one straight away (or exporting it
	// as a stem) has them ready. Only zones that none of them play are left out.
	PreviewNotes notes(transpose);
	if (!addPlayedNotes(song, this, notes)) {
		return loadAllAudioFiles(true);
	}

	bool doingAlternatePath = (audioFileManager.alternateLoadDirStatus == AlternateLoadDirStatus::NONE_SET);
	if (doingAlternatePath) {
		Error error = setupSongAudioFileDir(song);
		if (error != Error::NONE) {
			return error;
		}
	}

	Error error = Error::NONE;
	for (int32_t s = 0; s < kNumSources && error == Error::NONE; s++) {
		if (sources[s].oscType == OscType::SAMPLE || sources[s].oscType == OscType::WAVETABLE) {
			error = sources[s].loadSamplesForNotes(notes.span());
		}
	}
	if (hasLazyAudioFiles()) {
		lazyFromSong = true;
	}

	if (doingAlternatePath) {
		audioFileManager.thingFinishedLoading();
	}
	return error;
}

void SoundInstrument::unloadUnusedZones(Song* song) {
	if (unusedZonesUnloaded || !song->samplesMayLoadLazily || hasActiveVoices()) {
		return;
	}
	PreviewNotes notes(transpose);
	if (!addPlayedNotes(song, this, notes)) {
		return;
	}
	bool unloadedAny = false;
	for (Source& source : sources) {
		unloadedAny = source.unloadZonesNotFor(notes.span()) || unloadedAny;
	}
	if (unloadedAny) {
		lazyFromSong = true; // They were loaded from the Song's folders, or at least their normal paths
	}
	unusedZonesUnloaded = true;
}

void SoundInstrument::loadZonesForNoteNow(Song* song, int32_t note) {
	PreviewNotes notes(transpose);
	notes.add(note);
	if (hasLazyAudioFiles()
	    && (sources[0].hasLazySampleFor(notes.span()) || sources[1].hasLazySampleFor(notes.span()))) {
		Error error = lazyFromSong ? setupSongAudioFileDir(song) : setupDefaultAudioFileDir();
		if (error == Error::NONE) {
			for (Source& source : sources) {
				while (source.loadLazySample(notes.span(), false)) {}
			}
			audioFileManager.thingFinishedLoading();
		}
	}
	for (Source& source : sources) {
		source.loadZoneStartsNowFor(notes.span());
	}
}

bool SoundInstrument::loadLazyAudioFile(Song* song, bool mayLoadTheRest) {
	if (!hasLazyAudioFiles()) {
		return false;
	}
	SampleNeed need = getSampleNeed(song, this);
	if (need == SampleNeed::ALL) {
		playedZonesLoaded = false;
	}
	else if (playedZonesLoaded) {
		return false; // Checked already, and nothing has changed what it plays since
	}

	// The notes its Clips play, then the notes the audition pads play, if one of its Clips is on screen
	PreviewNotes notes(transpose);
	bool fits = addPlayedNotes(song, this, notes);
	Clip* clip = song->getCurrentClip();
	if (fits && clip && clip->output == this && clip->type == ClipType::INSTRUMENT) {
		for (int32_t yDisplay = 0; fits && yDisplay < kDisplayHeight; yDisplay++) {
			fits = notes.add(static_cast<InstrumentClip*>(clip)->getYNoteFromYDisplay(yDisplay, song));
		}
	}
	// The rest: if left from loading the Song, as soon as they're needed; if left from previewing presets, once the
	// user has stopped changing presets. Either way, not while the card has Clusters to read, most likely for Voices
	// that are playing: those mustn't be held up by lots of zones that nothing is about to play.
	bool anyOther = (!fits || (need == SampleNeed::ALL && (lazyFromSong || mayLoadTheRest)))
	                && !audioFileManager.anyClusterLoadsPending();
	if (!anyOther && !sources[0].hasLazySampleFor(notes.span()) && !sources[1].hasLazySampleFor(notes.span())) {
		playedZonesLoaded = (need != SampleNeed::ALL && fits);
		return false; // Nothing it needs is left: don't set up for loading again and again
	}

	Error error = lazyFromSong ? setupSongAudioFileDir(song) : setupDefaultAudioFileDir();
	if (error != Error::NONE) {
		return false;
	}
	bool loaded = false;
	for (int32_t s = 0; s < kNumSources && !loaded; s++) {
		loaded = sources[s].loadLazySample(notes.span(), anyOther);
	}
	audioFileManager.thingFinishedLoading();
	return loaded;
}

void loadLazyAudioFilesTask() {
	// Not while anything else is being loaded: that uses the same alternate-folder state in audioFileManager
	if (!currentSong || audioFileManager.thingTypeBeingLoaded != ThingType::NONE
	    || audioFileManager.alternateLoadDirStatus != AlternateLoadDirStatus::NONE_SET) {
		return;
	}
	bool loadTheRest = (AudioEngine::audioSampleTimer - lastPreviewTime) >= kLazyLoadRestDelay;

	// Only Instruments in the Song: one the user has scrolled past has been deleted or hibernated, so its loading stops
	for (Output* output = currentSong->firstOutput; output; output = output->next) {
		if (output->type == OutputType::SYNTH
		    && static_cast<SoundInstrument*>(output)->loadLazyAudioFile(currentSong, loadTheRest)) {
			return; // One file at a time
		}
	}
}

namespace {

// Whether drum has notes in any of kit's Clips
bool drumIsPlayed(Song* song, Kit* kit, Drum* drum) {
	for (Clip* clip : AllClips::everywhere(song)) {
		if (clip->output != kit || clip->type != ClipType::INSTRUMENT) {
			continue;
		}
		auto* instrumentClip = static_cast<InstrumentClip*>(clip);
		for (int32_t i = 0; i < instrumentClip->noteRows.getNumElements(); i++) {
			NoteRow* noteRow = instrumentClip->noteRows.getElement(i);
			if (noteRow->drum == drum && !noteRow->hasNoNotes()) {
				return true;
			}
		}
	}
	return false;
}

void setSynthIdle(Song* song, SoundInstrument* synth, bool idle) {
	if (idle == synth->sampleStartsIdle) {
		return;
	}
	PreviewNotes notes(synth->transpose);
	bool fits = addPlayedNotes(song, synth, notes);
	synth->setSampleStartsIdle(idle, notes.span(), !fits);
}

} // namespace

void idleSampleStartsTask() {
	Song* song = currentSong;
	if (song == nullptr || audioFileManager.thingTypeBeingLoaded != ThingType::NONE) {
		return;
	}
	for (Output* output = song->firstOutput; output; output = output->next) {
		if (output->type != OutputType::SYNTH && output->type != OutputType::KIT) {
			continue;
		}
		bool wanted = getSampleNeed(song, output) != SampleNeed::NONE;
		if (output->type == OutputType::SYNTH) {
			auto* synth = static_cast<SoundInstrument*>(output);
			setSynthIdle(song, synth, !wanted);
			if (wanted) {
				synth->unusedZonesUnloaded = false;
			}
			else {
				synth->unloadUnusedZones(song);
			}
			continue;
		}
		auto* kit = static_cast<Kit*>(output);
		for (Drum* drum = kit->firstDrum; drum; drum = drum->next) {
			if (drum->type != DrumType::SOUND) {
				continue;
			}
			auto* soundDrum = static_cast<SoundDrum*>(drum);
			bool idle = !wanted && !drum->midiInput.containsSomething();
			if (idle && !soundDrum->sampleStartsIdle) {
				soundDrum->setSampleStartsIdle(true, {}, drumIsPlayed(song, kit, drum));
			}
			else {
				soundDrum->setSampleStartsIdle(idle);
			}
		}
	}
}

void SoundInstrument::resyncLFOs(bool reset_state) {
	resyncGlobalLFOs(reset_state);
}

ModControllable* SoundInstrument::toModControllable() {
	return this;
}

void SoundInstrument::setupPatching(ModelStackWithTimelineCounter* modelStack) {

	InstrumentClip* clip = (InstrumentClip*)modelStack->getTimelineCounterAllowNull();
	ParamManagerForTimeline* paramManager;
	if (clip) {
		paramManager = &clip->paramManager;

		ModelStackWithThreeMainThings* modelStackWithThreeMainThings =
		    modelStack->addOtherTwoThingsButNoNoteRow(this, paramManager);

		ensureInaccessibleParamPresetValuesWithoutKnobsAreZero(modelStackWithThreeMainThings);
	}
	else {
		paramManager =
		    (ParamManagerForTimeline*)modelStack->song->getBackedUpParamManagerPreferablyWithClip(this, NULL);
		ensureInaccessibleParamPresetValuesWithoutKnobsAreZeroWithMinimalDetails(paramManager);
	}

	ModelStackWithParamCollection* modelStackWithParamCollection =
	    paramManager->getPatchCableSet(modelStack->addOtherTwoThingsButNoNoteRow(this, paramManager));

	PatchCableSet* patchCableSet = (PatchCableSet*)modelStackWithParamCollection->paramCollection;

	patchCableSet->setupPatching(modelStackWithParamCollection);
}

bool SoundInstrument::setActiveClip(ModelStackWithTimelineCounter* modelStack, PgmChangeSend maySendMIDIPGMs) {

	bool clipChanged = MelodicInstrument::setActiveClip(modelStack, maySendMIDIPGMs);

	if (clipChanged) {
		AudioEngine::mustUpdateReverbParamsBeforeNextRender = true;

		if (modelStack) {
			ParamManager* paramManager = &modelStack->getTimelineCounter()->paramManager;
			patcher.performInitialPatching(*this, *paramManager);

			// Grab mono expression params
			ExpressionParamSet* expressionParams = paramManager->getExpressionParamSet();
			if (expressionParams) {
				for (int32_t i = 0; i < kNumExpressionDimensions; i++) {
					monophonicExpressionValues[i] = expressionParams->params[i].getCurrentValue();
				}
			}
			else {
				for (int32_t i = 0; i < kNumExpressionDimensions; i++) {
					monophonicExpressionValues[i] = 0;
				}
			}
			expressionSourcesChangedAtSynthLevel.set();
		}
	}
	return clipChanged;
}

void SoundInstrument::setupWithoutActiveClip(ModelStack* modelStack) {

	ModelStackWithTimelineCounter* modelStackWithTimelineCounter = modelStack->addTimelineCounter(nullptr);

	setupPatching(modelStackWithTimelineCounter);

	ParamManager* paramManager =
	    modelStackWithTimelineCounter->song->getBackedUpParamManagerPreferablyWithClip(this, NULL);
	if (!paramManager) {
		FREEZE_WITH_ERROR("E173");
	}
	patcher.performInitialPatching(*this, *paramManager);

	// Clear mono expression params
	for (int32_t i = 0; i < kNumExpressionDimensions; i++) {
		monophonicExpressionValues[i] = 0;
	}
	expressionSourcesChangedAtSynthLevel.set();

	Instrument::setupWithoutActiveClip(modelStack);
}

void SoundInstrument::prepareForHibernationOrDeletion() {
	Sound::prepareForHibernation();
}

void SoundInstrument::setupPatchingForAllParamManagers(Song* song) {
	song->setupPatchingForAllParamManagersForInstrument(this);
}

void SoundInstrument::deleteBackedUpParamManagers(Song* song) {
	song->deleteBackedUpParamManagersForModControllable(this);
}

extern bool expressionValueChangesMustBeDoneSmoothly;

void SoundInstrument::monophonicExpressionEvent(int32_t newValue, int32_t expressionDimension) {
	expressionSourcesChangedAtSynthLevel[expressionDimension] = true;
	monophonicExpressionValues[expressionDimension] = newValue;
}

// Alternative to what's in the NonAudioInstrument:: implementation, which would almost work here, but we cut corner for
// Sound by avoiding going through the Arp and just talk directly to the Voices. (Despite my having made it now actually
// need to talk to the Arp too, as below...) Note, this virtual function actually overrides/implements from two base
// classes - MelodicInstrument and ModControllable.
void SoundInstrument::polyphonicExpressionEventOnChannelOrNote(int32_t newValue, int32_t expressionDimension,
                                                               int32_t channelOrNoteNumber,
                                                               MIDICharacteristic whichCharacteristic) {
	int32_t s = expressionDimension + util::to_underlying(PatchSource::X);
	for (const auto& voice : this->voices()) {
		if (voice->inputCharacteristics[util::to_underlying(whichCharacteristic)] == channelOrNoteNumber) {
			if (expressionValueChangesMustBeDoneSmoothly) {
				voice->expressionEventSmooth(newValue, s);
			}
			else {
				voice->expressionEventImmediate(*this, newValue, s);
			}
		}
	}

	// Must update MPE values in Arp too - useful either if it's on, or if we're in true monophonic mode - in either
	// case, we could need to suddenly do a note-on for a different note that the Arp knows about, and need these MPE
	// values.
	int32_t n, nEnd;
	if (whichCharacteristic == MIDICharacteristic::NOTE) {
		n = arpeggiator.notes.search(channelOrNoteNumber, GREATER_OR_EQUAL);
		if (n < arpeggiator.notes.getNumElements()) {
			nEnd = 0;
			goto lookAtArpNote;
		}
		return;
	}
	nEnd = arpeggiator.notes.getNumElements();
	for (n = 0; n < nEnd; n++) {
lookAtArpNote:
		ArpNote* arpNote = (ArpNote*)arpeggiator.notes.getElementAddress(n);
		if (arpNote->inputCharacteristics[util::to_underlying(whichCharacteristic)] == channelOrNoteNumber) {
			arpNote->mpeValues[expressionDimension] = newValue >> 16;
		}
	}

	// Let the Sound know about this polyphonic expression event
	// The Sound class will use it to send MIDI out (if enabled in the sound config)
	Sound::polyphonicExpressionEventOnChannelOrNote(newValue, expressionDimension, channelOrNoteNumber,
	                                                whichCharacteristic);
}

void SoundInstrument::sendNote(ModelStackWithThreeMainThings* modelStack, bool isOn, int32_t noteCode,
                               int16_t const* mpeValues, int32_t fromMIDIChannel, uint8_t velocity,
                               uint32_t sampleSyncLength, int32_t ticksLate, uint32_t samplesLate) {

	if (!inValidState) {
		return;
	}

	if (isOn) {
		noteOn(modelStack, &arpeggiator, noteCode, mpeValues, sampleSyncLength, ticksLate, samplesLate, velocity,
		       fromMIDIChannel);
	}
	else {
		noteOff(modelStack, &arpeggiator, noteCode);
	}
}

ArpeggiatorSettings* SoundInstrument::getArpSettings(InstrumentClip* clip) {
	return MelodicInstrument::getArpSettings(clip);
}

bool SoundInstrument::readTagFromFile(Deserializer& reader, char const* tagName) {
	return MelodicInstrument::readTagFromFile(reader, tagName);
}

void SoundInstrument::compensateInstrumentVolumeForResonance(ModelStackWithThreeMainThings* modelStack) {
	Sound::compensateVolumeForResonance(modelStack);
}

void SoundInstrument::loadCrucialAudioFilesOnly() {
	loadAllAudioFiles(true);
}

// Any time it gets edited, we want to grab the default arp settings from the activeClip
void SoundInstrument::beenEdited(bool shouldMoveToEmptySlot) {
	if (activeClip) {
		defaultArpSettings.cloneFrom(&((InstrumentClip*)activeClip)->arpSettings);
	}
	Instrument::beenEdited(shouldMoveToEmptySlot);
}

// Returns num ticks til next arp event
int32_t SoundInstrument::doTickForwardForArp(ModelStack* modelStack, int32_t currentPos) {
	if (!activeClip) {
		return 2147483647;
	}

	ModelStackWithThreeMainThings* modelStackWithThreeMainThings =
	    modelStack->addTimelineCounter(activeClip)
	        ->addOtherTwoThingsButNoNoteRow(this, getParamManager(modelStack->song));

	UnpatchedParamSet* unpatchedParams = modelStackWithThreeMainThings->paramManager->getUnpatchedParamSet();

	ArpeggiatorSettings* arpSettings = getArpSettings();
	arpSettings->updateParamsFromUnpatchedParamSet(unpatchedParams);

	ArpReturnInstruction instruction;

	int32_t ticksTilNextArpEvent =
	    arpeggiator.doTickForward(arpSettings, &instruction, currentPos, activeClip->currentlyPlayingReversed);

	ModelStackWithSoundFlags* modelStackWithSoundFlags = modelStackWithThreeMainThings->addSoundFlags();

	bool atLeastOneOff = false;
	for (int32_t n = 0; n < ARP_MAX_INSTRUCTION_NOTES; n++) {
		if (instruction.glideNoteCodeOffPostArp[n] == ARP_NOTE_NONE) {
			break;
		}
		atLeastOneOff = true;
		noteOffPostArpeggiator(modelStackWithSoundFlags, instruction.glideNoteCodeOffPostArp[n]);
	}
	for (int32_t n = 0; n < ARP_MAX_INSTRUCTION_NOTES; n++) {
		if (instruction.noteCodeOffPostArp[n] == ARP_NOTE_NONE) {
			break;
		}
		atLeastOneOff = true;
		noteOffPostArpeggiator(modelStackWithSoundFlags, instruction.noteCodeOffPostArp[n]);
	}
	if (atLeastOneOff) {
		invertReversed = false;
	}
	startArpNoteOns(modelStackWithSoundFlags, instruction);

	return ticksTilNextArpEvent;
}

void SoundInstrument::getThingWithMostReverb(Sound** soundWithMostReverb, ParamManager** paramManagerWithMostReverb,
                                             GlobalEffectableForClip** globalEffectableWithMostReverb,
                                             int32_t* highestReverbAmountFound) {
	if (activeClip) {
		Sound::getThingWithMostReverb(soundWithMostReverb, paramManagerWithMostReverb, globalEffectableWithMostReverb,
		                              highestReverbAmountFound, &activeClip->paramManager);
	}
}

ArpeggiatorBase* SoundInstrument::getArp() {
	return &arpeggiator;
}

bool SoundInstrument::noteIsOn(int32_t noteCode, bool resetTimeEntered) {

	ArpeggiatorSettings* arpSettings = getArpSettings();

	if (arpSettings != nullptr
	    && (arpSettings->mode != ArpMode::OFF || polyphonic == PolyphonyMode::LEGATO
	        || polyphonic == PolyphonyMode::MONO)) {
		int32_t n = arpeggiator.notes.search(noteCode, GREATER_OR_EQUAL);
		if (n >= arpeggiator.notes.getNumElements()) {
			return false;
		}
		ArpNote* arpNote = (ArpNote*)arpeggiator.notes.getElementAddress(n);
		return (arpNote->inputCharacteristics[util::to_underlying(MIDICharacteristic::NOTE)] == noteCode);
	}

	if (!hasActiveVoices()) {
		return false;
	}

	for (const auto& voice : this->voices()) {
		if ((voice->noteCodeAfterArpeggiation == noteCode)
		    && voice->envelopes[0].state < EnvelopeStage::RELEASE) { // Ignore releasing notes. Is this right?
			if (resetTimeEntered) {
				voice->envelopes[0].resetTimeEntered();
			}
			return true;
		}
	}
	return false;
}
