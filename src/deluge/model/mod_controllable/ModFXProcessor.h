/*
 * Copyright © 2024 Mark Adams
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
#include "dsp/stereo_sample.h"
#include "modulation/lfo.h"
#include "util/containers.h"
#include <cstdint>
#include <span>

/// Triangle chorus delay when the LFO is at its low point, in whole samples (5.37 ms at 44.1 kHz)
constexpr uint32_t kModFXTriChorusBaseSamples = 237;

class ModFXProcessor {

	void setupChorus(const ModFXType& modFXType, int32_t modFXDepth, int32_t* postFXVolume,
	                 UnpatchedParamSet* unpatchedParams, LFOType& modFXLFOWaveType, int32_t& modFXDelayOffset,
	                 int32_t& thisModFXDelayDepth) const;

	/// flanger, phaser, warble - generally any modulated delay tap based effect with feedback
	void setupModFXWFeedback(const ModFXType& modFXType, int32_t modFXDepth, int32_t* postFXVolume,
	                         UnpatchedParamSet* unpatchedParams, LFOType& modFXLFOWaveType, int32_t& modFXDelayOffset,
	                         int32_t& thisModFXDelayDepth, int32_t& feedback) const;
	// not grain!
	template <ModFXType modFXType>
	void processModFXBuffer(std::span<StereoSample> buffer, int32_t modFXRate, int32_t modFXDepth,
	                        LFOType& modFXLFOWaveType, int32_t modFXDelayOffset, int32_t thisModFXDelayDepth,
	                        int32_t feedback, bool stereo);

	template <ModFXType modFXType>
	std::pair<int32_t, int32_t> processModLFOs(int32_t modFXRate, LFOType modFXLFOWaveType);

	template <ModFXType modFXType, bool stereo>
	StereoSample processOneModFXSample(StereoSample sample, int32_t modFXDelayOffset, int32_t thisModFXDelayDepth,
	                                   int32_t feedback, int32_t lfoOutput, int32_t lfo2Output);
	StereoSample processOnePhaserSample(StereoSample sample, int32_t modFXDepth, int32_t feedback, int32_t lfoOutput);

	/// Per-buffer settings for the triangle chorus, derived from the mod FX params.
	struct TriChorusSettings {
		uint32_t phaseIncrement; ///< LFO phase increment per sample (2^32 = one cycle)
		uint32_t widthPhase;     ///< right-channel LFO phase offset (2^31 = 180 degrees)
		int32_t sweep;           ///< delay sweep at the triangle peak, Q20 samples, times 2
		int32_t highPassCoef;    ///< input high-pass one-pole coefficient, Q32
	};
	TriChorusSettings setupTriChorus(int32_t modFXRate, int32_t modFXDepth, UnpatchedParamSet* unpatchedParams);
	// Kept out of line so its loops get registers to themselves
	[[gnu::noinline]] void processTriChorusBuffer(std::span<StereoSample> buffer, const TriChorusSettings& settings,
	                                              bool stereo);

public:
	/// Triangle chorus state for one channel
	struct TriChorusChannel {
		int32_t delay;    ///< smoothed delay above the base delay, Q20 samples
		int32_t highPass; ///< input high-pass: the one-pole low-pass that gets subtracted
		int32_t lowPass;  ///< wet low-pass, half scale
	};
	TriChorusChannel triChorusChannels[2]{};
	// Last rate -> phase increment and high-pass param -> coefficient conversions (-1: not computed yet)
	int32_t triChorusCachedRate{-1};
	uint32_t triChorusCachedPhaseIncrement{0};
	int32_t triChorusCachedHighPassParam{0};
	int32_t triChorusCachedHighPassCoef{-1};

	ModFXProcessor() {
		// Mod FX
		ModFXProcessor::modFXBuffer = nullptr;
		ModFXProcessor::modFXBufferWriteIndex = 0;
		memset(allpassMemory, 0, sizeof(allpassMemory));
	}
	~ModFXProcessor() {
		// Free the mod fx memory
		if (modFXBuffer) {
			delugeDealloc(modFXBuffer);
		}
	}
	// Phaser
	StereoSample phaserMemory{0, 0};
	StereoSample allpassMemory[kNumAllpassFiltersPhaser];
	StereoSample* modFXBuffer{nullptr};
	uint16_t modFXBufferWriteIndex{0};
	LFO modFXLFO;
	LFO modFXLFOStereo;
	void processModFX(std::span<StereoSample> buffer, const ModFXType& modFXType, int32_t modFXRate, int32_t modFXDepth,
	                  int32_t* postFXVolume, UnpatchedParamSet* unpatchedParams, bool anySoundComingIn);
	void resetMemory();
	void setupBuffer();
	void disableBuffer();
	void tickLFO(int32_t numSamples, int32_t phaseIncrement) {
		// Do Mod FX
		modFXLFO.tick(numSamples, phaseIncrement);
	}
};
// helper functions for the mod fx types
namespace modfx {
deluge::vector<std::string_view> getModNames();

const char* getParamName(ModFXType type, ModFXParam param, bool shortName = false);

const char* modFXToString(ModFXType type);
} // namespace modfx
