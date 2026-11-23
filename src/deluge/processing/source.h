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
#include "model/sample/sample_controls.h"
#include "storage/multi_range/multi_range_array.h"
#include "util/phase_increment_fine_tuner.h"
#include <bitset>
#include <span>

class Sound;
class ParamManagerForTimeline;
class WaveTable;
class SampleHolder;
class DxPatch;

class Source {
public:
	Source();
	~Source();

	SampleControls sampleControls;

	OscType oscType;

	// These are not valid for Samples
	int16_t transpose;
	int8_t cents;
	PhaseIncrementFineTuner fineTuner;

	MultiRangeArray ranges;

	DxPatch* dxPatch;
	bool dxPatchChanged = false;
	SampleRepeatMode repeatMode;

	int8_t timeStretchAmount;

	int16_t defaultRangeI; // -1 means none yet

	bool renderInStereo(Sound* s, SampleHolder* sampleHolder = nullptr);
	void setCents(int32_t newCents);
	void recalculateFineTuner();
	int32_t getLengthInSamplesAtSystemSampleRate(int32_t note, bool forTimeStretching = false);
	void detachAllAudioFiles();
	Error loadAllSamples(bool mayActuallyReadFiles);
	/// For a multisample with many zones, loads just the zones that would play these notes, leaving the rest for
	/// loadLazySample(). Otherwise loads everything, like loadAllSamples().
	Error loadSamplesForNotes(std::span<const int16_t> notes);
	/// Loads one of the zones loadSamplesForNotes() skipped: one that plays one of these notes, or, if anyOther, any.
	/// Returns whether it loaded one.
	bool loadLazySample(std::span<const int16_t> notes, bool anyOther);
	bool hasLazySamples() const { return lazyPending; }
	void setReversed(bool newReversed);
	int32_t getRangeIndex(int32_t note);
	MultiRange* getRange(int32_t note);
	MultiRange* getOrCreateFirstRange();
	bool hasAtLeastOneAudioFileLoaded();
	void doneReadingFromFile(Sound* sound);
	bool hasAnyLoopEndPoint();
	OscType getOscType();
	void setOscType(OscType newType);

	DxPatch* ensureDxPatch();

private:
	void destructAllMultiRanges();
	bool rangePlaysAnyOf(int32_t e, std::span<const int16_t> notes);
	void loadRange(int32_t e);

	// Lazy loading. Multisamples with more zones than this are just loaded in full.
	static constexpr int32_t kMaxLazyRanges = 256;
	bool lazyPending = false;              ///< Some zones skipped by loadSamplesForNotes() may still need loading
	int16_t lazyNumRanges = 0;             ///< How many zones there were when lazyTried was set up
	std::bitset<kMaxLazyRanges> lazyTried; ///< Zones already loaded or tried, so a missing file isn't retried forever
};
