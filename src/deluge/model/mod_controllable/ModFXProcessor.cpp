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

#include "ModFXProcessor.h"
#include "definitions_cxx.hpp"
#include "mem_functions.h"
#include "memory/general_memory_allocator.h"
#include "modulation/params/param_set.h"
#include "processing/engines/audio_engine.h"
#include "util/comparison.h"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <numbers>

/// NOT GRAIN! - this only does the comb filter based mod fx
void ModFXProcessor::processModFX(std::span<StereoSample> buffer, const ModFXType& modFXType, int32_t modFXRate,
                                  int32_t modFXDepth, int32_t* postFXVolume, UnpatchedParamSet* unpatchedParams,
                                  bool anySoundComingIn) {
	// if nothing rendered into the clip then we can just return
	if (anySoundComingIn == false)
		return;
	if (modFXType != ModFXType::NONE) {

		LFOType modFXLFOWaveType{};
		int32_t modFXDelayOffset{};
		int32_t thisModFXDelayDepth{};
		int32_t feedback{};

		if (modFXType == ModFXType::FLANGER || modFXType == ModFXType::PHASER || modFXType == ModFXType::WARBLE) {

			setupModFXWFeedback(modFXType, modFXDepth, postFXVolume, unpatchedParams, modFXLFOWaveType,
			                    modFXDelayOffset, thisModFXDelayDepth, feedback);
		}
		else if (modFXType == ModFXType::CHORUS || modFXType == ModFXType::CHORUS_STEREO
		         || modFXType == ModFXType::DIMENSION) {
			setupChorus(modFXType, modFXDepth, postFXVolume, unpatchedParams, modFXLFOWaveType, modFXDelayOffset,
			            thisModFXDelayDepth);
		}

		switch (modFXType) {

		case ModFXType::NONE:
			break;
		case ModFXType::FLANGER:
			processModFXBuffer<ModFXType::FLANGER>(buffer, modFXRate, modFXDepth, modFXLFOWaveType, modFXDelayOffset,
			                                       thisModFXDelayDepth, feedback, AudioEngine::renderInStereo);
			break;
		case ModFXType::CHORUS:
			processModFXBuffer<ModFXType::CHORUS>(buffer, modFXRate, modFXDepth, modFXLFOWaveType, modFXDelayOffset,
			                                      thisModFXDelayDepth, feedback, AudioEngine::renderInStereo);
			break;
		case ModFXType::PHASER:
			processModFXBuffer<ModFXType::PHASER>(buffer, modFXRate, modFXDepth, modFXLFOWaveType, modFXDelayOffset,
			                                      thisModFXDelayDepth, feedback, AudioEngine::renderInStereo);
			break;
		case ModFXType::CHORUS_STEREO:
			processModFXBuffer<ModFXType::CHORUS_STEREO>(buffer, modFXRate, modFXDepth, modFXLFOWaveType,
			                                             modFXDelayOffset, thisModFXDelayDepth, feedback,
			                                             AudioEngine::renderInStereo);
			break;
		case ModFXType::WARBLE:
			processModFXBuffer<ModFXType::WARBLE>(buffer, modFXRate, modFXDepth, modFXLFOWaveType, modFXDelayOffset,
			                                      thisModFXDelayDepth, feedback, AudioEngine::renderInStereo);
			break;
		case ModFXType::GRAIN:
			break;
		case ModFXType::DIMENSION:
			processModFXBuffer<ModFXType::DIMENSION>(buffer, modFXRate, modFXDepth, modFXLFOWaveType, modFXDelayOffset,
			                                         thisModFXDelayDepth, feedback, AudioEngine::renderInStereo);
			break;
		case ModFXType::TRI_CHORUS: {
			*postFXVolume = multiply_32x32_rshift32(*postFXVolume, 1518500250) << 1; // Divide by sqrt(2), as chorus
			processTriChorusBuffer(buffer, setupTriChorus(modFXRate, modFXDepth, unpatchedParams),
			                       AudioEngine::renderInStereo);
			break;
		}
		}
	}
}

// Triangle chorus: one linearly interpolated delay tap per channel, no feedback, dry + wet output.
// - LFO: unipolar triangle, f = 0.28 + 2.82 * rate Hz (rate = knob position 0..1)
// - delay: 237 samples (5.37 ms) + depth * tri * 100.15 samples (2.27 ms), so 5.37 .. 7.65 ms, through a one-pole
//   smoother at about 55 Hz that rounds the triangle corners and de-zippers depth/width changes
// - right channel LFO phase = left + width * 180 degrees
// - wet path input: one-pole high-pass at 40 + 961.4 * hp^2 Hz, then a fixed one-pole low-pass at about 5.8 kHz
namespace {
constexpr float kTriChorusSampleRate = 44100.f;
constexpr float kTriChorusSweepSamples = (7.6532f - 5.3822f) * 44.1f; // 100.15 samples
// Rate param neutral phase increment and the +-8 octave span of the exponential rate law (getExp, value >> 2)
constexpr float kModFXRateNeutral = 121739.f;
constexpr float kModFXRateOctavesPerKnob = 16.f;
// Depth param final value at the top of the knob (volume law: 4x the neutral value 526133494)
constexpr float kModFXDepthMax = 2104533952.f;
// Delay smoother: d += (target - d) / 2^7, a one-pole at -ln(1 - 1/128) * 44100 / (2 pi) = 55 Hz
constexpr int32_t kTriChorusSmoothShift = 7;

// Unipolar triangle: 0 at phase 0, peak (2^31 - 1) at phase 1/2
[[gnu::always_inline]] inline int32_t triChorusTriangle(uint32_t phase) {
	return static_cast<int32_t>(phase ^ static_cast<uint32_t>(static_cast<int32_t>(phase) >> 31));
}

// One delay-line read: linear interpolation between the two samples around a delay of
// (base + delay / 2^20) samples. The line holds half-scale signal, so b - a cannot overflow.
template <bool right>
[[gnu::always_inline]] inline int32_t readTriChorusTap(const StereoSample* line, uint32_t writeIndex, int32_t delay) {
	uint32_t index = writeIndex - kModFXTriChorusBaseSamples - static_cast<uint32_t>(delay >> 20);
	const StereoSample& first = line[index & kModFXBufferIndexMask];
	const StereoSample& second = line[(index - 1) & kModFXBufferIndexMask];
	int32_t a = right ? first.r : first.l;
	int32_t b = right ? second.r : second.l;
	int32_t frac = static_cast<int32_t>(static_cast<uint32_t>(delay << 12) >> 1); // Q31
	return a + (multiply_32x32_rshift32(b - a, frac) << 1);
}

// Wet path input filter for one channel: high-pass as x minus its one-pole low-pass (state highPass, coefficient
// highPassCoef in Q32), then a one-pole low-pass with k = 1/2 + 1/16 (-3 dB at 5.8 kHz) whose state settles at
// half the input: lp += (k / 2) * (x - 2 lp). The half scale keeps headroom in the delay line.
[[gnu::always_inline]] inline int32_t filterTriChorusInput(int32_t input, int32_t& highPass, int32_t& lowPass,
                                                           int32_t highPassCoef) {
	highPass += multiply_32x32_rshift32(input - highPass, highPassCoef);
	int32_t diff = (input - highPass) - (lowPass << 1);
	lowPass += (diff >> 2) + (diff >> 5);
	return lowPass;
}

// One channel over the whole buffer. The channels run as separate passes so each loop's state fits in registers.
// With mono set, the left channel's wet signal is added to both outputs.
template <bool right, bool mono>
[[gnu::always_inline]] inline void processTriChorusChannel(std::span<StereoSample> buffer, StereoSample* line,
                                                           uint32_t writeIndex, uint32_t phase, uint32_t phaseIncrement,
                                                           int32_t sweep, int32_t highPassCoef,
                                                           ModFXProcessor::TriChorusChannel& state) {
	int32_t delay = state.delay;
	int32_t highPass = state.highPass;
	int32_t lowPass = state.lowPass;

	for (StereoSample& sample : buffer) {
		phase += phaseIncrement;
		int32_t target = multiply_32x32_rshift32(triChorusTriangle(phase), sweep);
		delay += (target - delay) >> kTriChorusSmoothShift;
		int32_t wet = readTriChorusTap<right>(line, writeIndex, delay) << 1;

		int32_t input = right ? sample.r : sample.l;
		int32_t filtered = filterTriChorusInput(input, highPass, lowPass, highPassCoef);
		if constexpr (right) {
			line[writeIndex].r = filtered;
			sample.r = input + wet;
		}
		else {
			line[writeIndex].l = filtered;
			sample.l = input + wet;
			if constexpr (mono) {
				sample.r += wet;
			}
		}
		writeIndex = (writeIndex + 1) & kModFXBufferIndexMask;
	}

	state.delay = delay;
	state.highPass = highPass;
	state.lowPass = lowPass;
}
} // namespace

ModFXProcessor::TriChorusSettings ModFXProcessor::setupTriChorus(int32_t modFXRate, int32_t modFXDepth,
                                                                 UnpatchedParamSet* unpatchedParams) {
	TriChorusSettings settings{};

	// The rate arrives as an exponential phase increment; recover the knob position (0..1) from its octave offset.
	// The log and exp below only run when their input changes.
	if (modFXRate != triChorusCachedRate) {
		float rateKnob = 0.5f
		                 + std::log2(static_cast<float>(std::max<int32_t>(modFXRate, 1)) / kModFXRateNeutral)
		                       / kModFXRateOctavesPerKnob;
		rateKnob = std::clamp(rateKnob, 0.f, 1.f);
		float rateHz = 0.28f + 2.82f * rateKnob;
		triChorusCachedPhaseIncrement = static_cast<uint32_t>(rateHz * (4294967296.f / kTriChorusSampleRate));
		triChorusCachedRate = modFXRate;
	}
	settings.phaseIncrement = triChorusCachedPhaseIncrement;

	// Depth uses the volume (square) law; take the square root to get back a linear knob position
	float depthKnob = std::sqrt(static_cast<float>(std::max<int32_t>(modFXDepth, 0)) / kModFXDepthMax);
	depthKnob = std::min(depthKnob, 1.f);
	settings.sweep = static_cast<int32_t>(depthKnob * kTriChorusSweepSamples * static_cast<float>(1 << 21));

	// Offset knob: stereo width, 0..180 degrees of right-channel LFO phase
	uint32_t offsetParam = static_cast<uint32_t>(
	    unpatchedParams->getValue(deluge::modulation::params::UNPATCHED_MOD_FX_OFFSET) + 2147483648u);
	settings.widthPhase = offsetParam >> 1;

	// Feedback knob: wet-path high-pass, 40 Hz .. 1 kHz, quadratic in the knob
	int32_t hpParam = unpatchedParams->getValue(deluge::modulation::params::UNPATCHED_MOD_FX_FEEDBACK);
	if (hpParam != triChorusCachedHighPassParam || triChorusCachedHighPassCoef < 0) {
		float hpKnob = static_cast<float>(static_cast<uint32_t>(hpParam) + 2147483648u) * (1.f / 4294967296.f);
		float hpHz = 40.f + 961.4f * hpKnob * hpKnob;
		float hpCoef = 1.f - std::exp(-2.f * std::numbers::pi_v<float> * hpHz / kTriChorusSampleRate);
		triChorusCachedHighPassCoef = static_cast<int32_t>(hpCoef * 4294967296.f);
		triChorusCachedHighPassParam = hpParam;
	}
	settings.highPassCoef = triChorusCachedHighPassCoef;

	return settings;
}

void ModFXProcessor::processTriChorusBuffer(std::span<StereoSample> buffer, const TriChorusSettings& settings,
                                            bool stereo) {
	const uint32_t writeIndex = modFXBufferWriteIndex;
	const uint32_t phase = modFXLFO.phase;
	if (stereo) {
		processTriChorusChannel<false, false>(buffer, modFXBuffer, writeIndex, phase, settings.phaseIncrement,
		                                      settings.sweep, settings.highPassCoef, triChorusChannels[0]);
		processTriChorusChannel<true, false>(buffer, modFXBuffer, writeIndex, phase + settings.widthPhase,
		                                     settings.phaseIncrement, settings.sweep, settings.highPassCoef,
		                                     triChorusChannels[1]);
	}
	else {
		processTriChorusChannel<false, true>(buffer, modFXBuffer, writeIndex, phase, settings.phaseIncrement,
		                                     settings.sweep, settings.highPassCoef, triChorusChannels[0]);
	}
	modFXBufferWriteIndex = (writeIndex + buffer.size()) & kModFXBufferIndexMask;
	modFXLFO.phase = phase + settings.phaseIncrement * static_cast<uint32_t>(buffer.size());
}

void ModFXProcessor::setupChorus(const ModFXType& modFXType, int32_t modFXDepth, int32_t* postFXVolume,
                                 UnpatchedParamSet* unpatchedParams, LFOType& modFXLFOWaveType,
                                 int32_t& modFXDelayOffset, int32_t& thisModFXDelayDepth) const {
	modFXDelayOffset = multiply_32x32_rshift32(
	    kModFXMaxDelay,
	    (unpatchedParams->getValue(deluge::modulation::params::UNPATCHED_MOD_FX_OFFSET) >> 1) + 1073741824);
	thisModFXDelayDepth = multiply_32x32_rshift32(modFXDelayOffset, modFXDepth) << 2;
	if (modFXType == ModFXType::DIMENSION) {
		modFXLFOWaveType = LFOType::TRIANGLE;
	}
	else {
		modFXLFOWaveType = LFOType::SINE;
	}

	*postFXVolume = multiply_32x32_rshift32(*postFXVolume, 1518500250) << 1; // Divide by sqrt(2)
}
void ModFXProcessor::setupModFXWFeedback(const ModFXType& modFXType, int32_t modFXDepth, int32_t* postFXVolume,
                                         UnpatchedParamSet* unpatchedParams, LFOType& modFXLFOWaveType,
                                         int32_t& modFXDelayOffset, int32_t& thisModFXDelayDepth,
                                         int32_t& feedback) const {
	int32_t a = unpatchedParams->getValue(deluge::modulation::params::UNPATCHED_MOD_FX_FEEDBACK) >> 1;
	int32_t b = 2147483647 - ((a + 1073741824) >> 2) * 3;
	int32_t c = multiply_32x32_rshift32(b, b);
	int32_t d = multiply_32x32_rshift32(b, c);

	feedback = 2147483648 - (d << 2);

	// Adjust volume for flanger feedback
	int32_t squared = multiply_32x32_rshift32(feedback, feedback) << 1;
	int32_t squared2 = multiply_32x32_rshift32(squared, squared) << 1;
	squared2 = multiply_32x32_rshift32(squared2, squared) << 1;
	squared2 = (multiply_32x32_rshift32(squared2, squared2) >> 4)
	           * 23; // 22 // Make bigger to have more of a volume cut happen at high resonance
	//*postFXVolume = (multiply_32x32_rshift32(*postFXVolume, 2147483647 - squared2) >> 1) * 3;
	*postFXVolume = multiply_32x32_rshift32(*postFXVolume, 2147483647 - squared2);

	if (modFXType == ModFXType::FLANGER) {
		*postFXVolume <<= 1;
		modFXDelayOffset = kFlangerOffset;
		thisModFXDelayDepth = kFlangerAmplitude;
		modFXLFOWaveType = LFOType::TRIANGLE;
	}
	else if (modFXType == ModFXType::WARBLE) {
		*postFXVolume <<= 1;
		modFXDelayOffset = kFlangerOffset;
		modFXDelayOffset += multiply_32x32_rshift32(
		    kFlangerOffset, (unpatchedParams->getValue(deluge::modulation::params::UNPATCHED_MOD_FX_OFFSET)));
		thisModFXDelayDepth = multiply_32x32_rshift32(modFXDelayOffset, modFXDepth) << 1;
		modFXLFOWaveType = LFOType::WARBLER;
	}

	else { // Phaser
		modFXLFOWaveType = LFOType::SINE;
	}
}
template <ModFXType modFXType>
void ModFXProcessor::processModFXBuffer(std::span<StereoSample> buffer, int32_t modFXRate, int32_t modFXDepth,
                                        LFOType& modFXLFOWaveType, int32_t modFXDelayOffset,
                                        int32_t thisModFXDelayDepth, int32_t feedback, bool stereo) {
	if constexpr (modFXType == ModFXType::PHASER) {
		for (StereoSample& sample : buffer) {
			int32_t lfo = modFXLFO.render(1, modFXLFOWaveType, modFXRate);
			sample = processOnePhaserSample(sample, modFXDepth, feedback, lfo);
		}
		return;
	}

	for (StereoSample& sample : buffer) {
		auto [lfo1, lfo2] = processModLFOs<modFXType>(modFXRate, modFXLFOWaveType);
		sample = stereo ? processOneModFXSample<modFXType, true>(sample, modFXDelayOffset, thisModFXDelayDepth,
		                                                         feedback, lfo1, lfo2)
		                : processOneModFXSample<modFXType, false>(sample, modFXDelayOffset, thisModFXDelayDepth,
		                                                          feedback, lfo1, -lfo1);
	}
}

template <ModFXType modFXType>
std::pair<int32_t, int32_t> ModFXProcessor::processModLFOs(int32_t modFXRate, LFOType modFXLFOWaveType) {
	int32_t lfo1 = modFXLFO.render(1, modFXLFOWaveType, modFXRate);
	int32_t lfo2 = 0;
	// anymore and they get audibly out of sync, this just sounds wobblier
	constexpr q31_t width = 0.97 * ONE_Q31;
	if constexpr (modFXType == ModFXType::WARBLE) {
		// this needs a second lfo because it's a random process - we can't flip it to make a second sample but
		// these will always be different anyway

		lfo2 = modFXLFOStereo.render(1, modFXLFOWaveType, multiply_32x32_rshift32(modFXRate, width) << 1);
	}
	else {
		lfo2 = -lfo1;
	}
	return {lfo1, lfo2};
}

template <ModFXType modFXType, bool stereo>
StereoSample ModFXProcessor::processOneModFXSample(StereoSample sample, int32_t modFXDelayOffset,
                                                   int32_t thisModFXDelayDepth, int32_t feedback, int32_t lfoOutput,
                                                   int32_t lfo2Output) {
	int32_t delayTime = multiply_32x32_rshift32(lfoOutput, thisModFXDelayDepth) + modFXDelayOffset;

	int32_t strength2 = (delayTime & 65535) << 15;
	int32_t strength1 = (65535 << 15) - strength2;
	int32_t sample1Pos = modFXBufferWriteIndex - ((delayTime) >> 16);

	int32_t scaledValue1L =
	    multiply_32x32_rshift32_rounded(modFXBuffer[sample1Pos & kModFXBufferIndexMask].l, strength1);
	int32_t scaledValue2L =
	    multiply_32x32_rshift32_rounded(modFXBuffer[(sample1Pos - 1) & kModFXBufferIndexMask].l, strength2);
	int32_t modFXOutputL = scaledValue1L + scaledValue2L;

	if constexpr (stereo || modFXType == ModFXType::DIMENSION || modFXType == ModFXType::WARBLE) {
		delayTime = multiply_32x32_rshift32(lfo2Output, thisModFXDelayDepth) + modFXDelayOffset;
		strength2 = (delayTime & 65535) << 15;
		strength1 = (65535 << 15) - strength2;
		sample1Pos = modFXBufferWriteIndex - ((delayTime) >> 16);
	}

	int32_t scaledValue1R =
	    multiply_32x32_rshift32_rounded(modFXBuffer[sample1Pos & kModFXBufferIndexMask].r, strength1);
	int32_t scaledValue2R =
	    multiply_32x32_rshift32_rounded(modFXBuffer[(sample1Pos - 1) & kModFXBufferIndexMask].r, strength2);
	int32_t modFXOutputR = scaledValue1R + scaledValue2R;

	// feedback also controls the mix? Weird but ok, I guess it makes it work on one knob
	if constexpr (modFXType == ModFXType::FLANGER) {
		modFXOutputL = multiply_32x32_rshift32_rounded(modFXOutputL, feedback) << 2;
		modFXBuffer[modFXBufferWriteIndex].l = modFXOutputL + sample.l; // Feedback
		modFXOutputR = multiply_32x32_rshift32_rounded(modFXOutputR, feedback) << 2;
		modFXBuffer[modFXBufferWriteIndex].r = modFXOutputR + sample.r; // Feedback
	}
	else if constexpr (modFXType == ModFXType::WARBLE) {
		auto fback = multiply_32x32_rshift32_rounded(modFXOutputL, feedback);
		modFXBuffer[modFXBufferWriteIndex].l = fback + sample.l; // Feedback
		fback = multiply_32x32_rshift32_rounded(modFXOutputR, feedback);
		modFXBuffer[modFXBufferWriteIndex].r = fback + sample.r; // Feedback

		modFXOutputL <<= 1;
		modFXOutputR <<= 1;
	}
	else { // Chorus, Dimension
		modFXOutputL <<= 1;
		modFXBuffer[modFXBufferWriteIndex].l = sample.l; // Feedback
		modFXOutputR <<= 1;
		modFXBuffer[modFXBufferWriteIndex].r = sample.r; // Feedback
	}

	if constexpr (modFXType == ModFXType::DIMENSION || modFXType == ModFXType::WARBLE) {
		sample.l = modFXOutputL << 1;
		sample.r = modFXOutputR << 1;
	}
	else {
		sample.l += modFXOutputL;
		sample.r += modFXOutputR;
	}

	modFXBufferWriteIndex = (modFXBufferWriteIndex + 1) & kModFXBufferIndexMask;
	return sample;
}

StereoSample ModFXProcessor::processOnePhaserSample(StereoSample sample, int32_t modFXDepth, int32_t feedback,
                                                    int32_t lfoOutput) { // "1" is sorta represented by 1073741824 here
	int32_t _a1 =
	    1073741824 - multiply_32x32_rshift32_rounded((((uint32_t)lfoOutput + (uint32_t)2147483648) >> 1), modFXDepth);

	phaserMemory.l = sample.l + (multiply_32x32_rshift32_rounded(phaserMemory.l, feedback) << 1);
	phaserMemory.r = sample.r + (multiply_32x32_rshift32_rounded(phaserMemory.r, feedback) << 1);

	// Do the allpass filters
	for (auto& sample : allpassMemory) {
		StereoSample whatWasInput = phaserMemory;

		phaserMemory.l = (multiply_32x32_rshift32_rounded(phaserMemory.l, -_a1) << 2) + sample.l;
		sample.l = (multiply_32x32_rshift32_rounded(phaserMemory.l, _a1) << 2) + whatWasInput.l;

		phaserMemory.r = (multiply_32x32_rshift32_rounded(phaserMemory.r, -_a1) << 2) + sample.r;
		sample.r = (multiply_32x32_rshift32_rounded(phaserMemory.r, _a1) << 2) + whatWasInput.r;
	}

	sample.l += phaserMemory.l;
	sample.r += phaserMemory.r;

	return sample;
}

void ModFXProcessor::resetMemory() {
	if (modFXBuffer) {
		memset(modFXBuffer, 0, kModFXBufferSize * sizeof(StereoSample));
		triChorusChannels[0] = {};
		triChorusChannels[1] = {};
	}

	else {
		memset(allpassMemory, 0, sizeof(allpassMemory));
		memset(&phaserMemory, 0, sizeof(phaserMemory));
	}
}

void ModFXProcessor::setupBuffer() {
	if (!modFXBuffer) {
		modFXBuffer = (StereoSample*)delugeAlloc(kModFXBufferSize * sizeof(StereoSample));
		if (modFXBuffer) {
			memset(modFXBuffer, 0, kModFXBufferSize * sizeof(StereoSample));
		}
	}
}
void ModFXProcessor::disableBuffer() {
	if (modFXBuffer) {
		delugeDealloc(modFXBuffer);
		modFXBuffer = nullptr;
	}
}
