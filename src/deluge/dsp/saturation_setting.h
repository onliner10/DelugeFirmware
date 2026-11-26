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

#pragma once

#include "dsp/saturation.hpp"
#include "dsp/saturation_curves.h"
#include "util/fixedpoint.h"
#include <algorithm>
#include <cmath>
#include <cstdint>

namespace deluge::dsp {

/// a * b, both q31, as vqdmulh computes it: floor(2ab / 2^32). Scalar and NEON paths then agree bit for bit.
[[gnu::always_inline]] inline int32_t doublingMultiplyHigh(int32_t a, int32_t b) {
	return static_cast<int32_t>((static_cast<int64_t>(a) * b) >> 31);
}

/// Saturation amount, stored in 1/4096 steps of the original 0..15 setting so the old whole steps stay exact.
constexpr int32_t kSaturationStep = 4096;
constexpr int32_t kSaturationMaxSteps = 15;
constexpr uint16_t kSaturationMax = kSaturationMaxSteps * kSaturationStep;
/// The menu shows 0..127
constexpr int32_t kSaturationMenuMax = 127;

[[nodiscard]] constexpr int32_t saturationToMenu(uint16_t amount) {
	return (amount * kSaturationMenuMax + kSaturationMax / 2) / kSaturationMax;
}
[[nodiscard]] constexpr uint16_t saturationFromMenu(int32_t value) {
	value = std::clamp<int32_t>(value, 0, kSaturationMenuMax);
	return static_cast<uint16_t>((value * kSaturationMax + kSaturationMenuMax / 2) / kSaturationMenuMax);
}

/// How the antialiased saturator runs for one amount and curve type (dsp/saturation_curves.h).
///
/// A whole step n does what the 0..15 setting always did: getTanHAntialiased(x, base + n) << max(n - gainFrom, 0).
/// Each step drives the tanh 6dB harder; from step gainFrom up the output ceiling stays put and the small-signal gain
/// rises, below it the gain stays 1 and the ceiling comes down.
///
/// In between, at n + f, the tanh is driven 2^f harder still: getTanHAntialiased(x * 2^(f-1), base + n + 1), which is
/// half of getTanHAntialiased(x * 2^f, base + n) without overflowing x. That is doubled back, and below gainFrom
/// also scaled by 2^-f so the gain stays 1. Whole steps take none of this, so they are bit-for-bit what they were.
struct SaturationSetting {
	uint16_t amount = 0;
	SaturationType type = SaturationType::CLASSIC;
	int16_t const* table = nullptr; ///< the type's antialiased table
	uint32_t tanhShift = 0;
	int32_t outShift = 0;
	q31_t preGain = 0;  ///< q31; 0 = none
	q31_t postGain = 0; ///< q31, after outShift; 0 = none

	/// base: 5 for a sound's voices, 3 for clips. gainFrom: 2 for voices, 3 for clips.
	static SaturationSetting make(uint16_t amount, int32_t base, int32_t gainFrom,
	                              SaturationType type = SaturationType::CLASSIC) {
		SaturationSetting s;
		s.amount = amount;
		s.type = type;
		s.table = saturationTable(type);
		int32_t n = amount / kSaturationStep;
		int32_t fraction = amount % kSaturationStep;
		if (fraction == 0) {
			s.tanhShift = base + n;
			s.outShift = std::max<int32_t>(n - gainFrom, 0);
			return s;
		}
		float f = static_cast<float>(fraction) / kSaturationStep;
		s.tanhShift = base + n + 1;
		s.preGain = static_cast<q31_t>(std::exp2(f - 1.f) * 2147483648.f);
		if (n >= gainFrom) {
			s.outShift = n - gainFrom + 1;
		}
		else {
			s.outShift = 1;
			s.postGain = static_cast<q31_t>(std::min(std::exp2(-f) * 2147483648.f, 2147483647.f));
		}
		return s;
	}

	/// One sample, with the channel's working value
	[[gnu::always_inline]] q31_t apply(q31_t x, uint32_t* workingValue) const {
		if (preGain != 0) {
			x = doublingMultiplyHigh(x, preGain);
		}
		q31_t y = getTanHAntialiased(x, workingValue, tanhShift, table) << outShift;
		if (postGain != 0) {
			y = doublingMultiplyHigh(y, postGain);
		}
		return y;
	}

	/// A block of kNumChannels interleaved channels, in place
	template <size_t kNumChannels>
	void applyBlock(int32_t* data, size_t count, uint32_t* workingValues) const {
		size_t const vectorCount = count & ~size_t{3};
		if (preGain != 0) {
			for (size_t i = 0; i < vectorCount; i += 4) {
				vst1q_s32(&data[i], vqdmulhq_n_s32(vld1q_s32(&data[i]), preGain));
			}
			for (size_t i = vectorCount; i < count; i++) {
				data[i] = doublingMultiplyHigh(data[i], preGain);
			}
		}
		saturateTanHAntialiased<kNumChannels>(data, count, workingValues, tanhShift, outShift, table);
		if (postGain == 0) {
			return;
		}
		for (size_t i = 0; i < vectorCount; i += 4) {
			vst1q_s32(&data[i], vqdmulhq_n_s32(vld1q_s32(&data[i]), postGain));
		}
		for (size_t i = vectorCount; i < count; i++) {
			data[i] = doublingMultiplyHigh(data[i], postGain);
		}
	}
};

} // namespace deluge::dsp
