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

#include "util/functions.h"
#include "util/lookuptables/lookuptables.h"
#include <argon.hpp>
#include <cstddef>
#include <cstdint>

namespace deluge::dsp {

/// The per-sample antialiased tanh saturation, applied in place to a block: what calling
/// getTanHAntialiased(sample, &lastWorkingValues[channel], saturationAmount) << shiftAmount on every sample does.
/// The buffer holds kNumChannels interleaved channels, each carrying its own working value in lastWorkingValues.
template <size_t kNumChannels>
inline void saturateTanHAntialiasedScalar(int32_t* buffer, size_t count, uint32_t* lastWorkingValues,
                                          uint32_t saturationAmount, int32_t shiftAmount) {
	for (size_t i = 0; i < count; i++) {
		buffer[i] = getTanHAntialiased(buffer[i], &lastWorkingValues[i % kNumChannels], saturationAmount)
		            << shiftAmount;
	}
}

/// Same output as saturateTanHAntialiasedScalar(), bit for bit, four samples at a time.
///
/// Each output depends only on its own input and on the previous *input* working value of its channel, which
/// is kNumChannels samples back in the interleaved buffer, so 4 samples are independent. Two passes:
///  1. NEON: replace every input with its working value, (ssat(x, 32 - s) << s) ^ 0x80000000. A saturating
///     shift saturates to INT32_MAX rather than to (2^(31-s) - 1) << s, so its low s bits get cleared.
///  2. The table index of each sample is read from the buffer with plain core loads (moving NEON lanes to core
///     registers stalls the Cortex-A9), each {t[x], t[x+1]} pair is gathered as one 32-bit lane (2-byte aligned
///     only: the firmware runs with alignment checking off), and the interpolation runs in NEON. The working
///     values of the previous 4 samples are carried in registers, since their slots are already overwritten.
///
/// Only valid while signed_saturate_operand_unknown() saturates to exactly 32 - saturationAmount bits, i.e.
/// saturationAmount 1..20 (Sound uses 6..20). Anything else runs the scalar code.
template <size_t kNumChannels>
inline void saturateTanHAntialiased(int32_t* buffer, size_t count, uint32_t* lastWorkingValues,
                                    uint32_t saturationAmount, int32_t shiftAmount) {
	static_assert(kNumChannels == 1 || kNumChannels == 2);

	if (saturationAmount < 1 || saturationAmount > 20) [[unlikely]] {
		saturateTanHAntialiasedScalar<kNumChannels>(buffer, count, lastWorkingValues, saturationAmount, shiftAmount);
		return;
	}

	size_t const vectorCount = count & ~size_t{3};
	auto* const words = reinterpret_cast<uint32_t*>(buffer);

	// Pass 1: working values
	{
		int32x4_t const shift = vdupq_n_s32(static_cast<int32_t>(saturationAmount));
		uint32x4_t const lowBits = vdupq_n_u32((1u << saturationAmount) - 1);
		uint32x4_t const signBit = vdupq_n_u32(2147483648u);
		for (size_t i = 0; i < vectorCount; i += 4) {
			uint32x4_t saturated = vreinterpretq_u32_s32(vqshlq_s32(vld1q_s32(&buffer[i]), shift));
			vst1q_u32(&words[i], veorq_u32(vbicq_u32(saturated, lowBits), signBit));
		}
	}

	// Pass 2: the 2d table lookup, as in interpolateTableSigned2d(workingValue, lastWorkingValue, 32, 32,
	// tanH2d, 7, 6) >> (saturationAmount + 1), then << shiftAmount
	{
		constexpr size_t kRowBytes = sizeof(tanH2d[0]);
		auto const* const table = reinterpret_cast<uint8_t const*>(&tanH2d[0][0]);
		int32x4_t const finalRShift = vdupq_n_s32(-static_cast<int32_t>(saturationAmount + 1));
		int32x4_t const finalLShift = vdupq_n_s32(shiftAmount);
		int32x4_t const maxStrength = vdupq_n_s32(2147483647);

		// The working values kNumChannels samples back, as a vector (its last kNumChannels lanes) and as scalars
		uint32_t carried[kNumChannels];
		uint32x4_t carriedVector = vdupq_n_u32(lastWorkingValues[0]);
		for (size_t c = 0; c < kNumChannels; c++) {
			carried[c] = lastWorkingValues[c];
		}
		if constexpr (kNumChannels == 2) {
			carriedVector = vsetq_lane_u32(lastWorkingValues[1], carriedVector, 3);
		}

		for (size_t i = 0; i < vectorCount; i += 4) {
			uint32_t const w0 = words[i];
			uint32_t const w1 = words[i + 1];
			uint32_t const w2 = words[i + 2];
			uint32_t const w3 = words[i + 3];
			uint32_t p0, p1, p2, p3;
			if constexpr (kNumChannels == 1) {
				p0 = carried[0];
				p1 = w0;
				p2 = w1;
				p3 = w2;
			}
			else {
				p0 = carried[0];
				p1 = carried[1];
				p2 = w0;
				p3 = w1;
			}

			// Byte address of t[y][x]; t[y + 1][x] is one row further on
			auto address = [table](uint32_t workingValue, uint32_t previous) {
				return table + ((previous >> 26) * kRowBytes) + ((workingValue >> 25) << 1);
			};
			uint8_t const* a0 = address(w0, p0);
			uint8_t const* a1 = address(w1, p1);
			uint8_t const* a2 = address(w2, p2);
			uint8_t const* a3 = address(w3, p3);

			// {t[x] in the low half, t[x + 1] in the high half} for row y and row y + 1
			uint32x4_t pairs1 = vld1q_dup_u32(reinterpret_cast<uint32_t const*>(a0));
			pairs1 = vld1q_lane_u32(reinterpret_cast<uint32_t const*>(a1), pairs1, 1);
			pairs1 = vld1q_lane_u32(reinterpret_cast<uint32_t const*>(a2), pairs1, 2);
			pairs1 = vld1q_lane_u32(reinterpret_cast<uint32_t const*>(a3), pairs1, 3);
			uint32x4_t pairs2 = vld1q_dup_u32(reinterpret_cast<uint32_t const*>(a0 + kRowBytes));
			pairs2 = vld1q_lane_u32(reinterpret_cast<uint32_t const*>(a1 + kRowBytes), pairs2, 1);
			pairs2 = vld1q_lane_u32(reinterpret_cast<uint32_t const*>(a2 + kRowBytes), pairs2, 2);
			pairs2 = vld1q_lane_u32(reinterpret_cast<uint32_t const*>(a3 + kRowBytes), pairs2, 3);

			uint32x4_t const working = vld1q_u32(&words[i]);
			uint32x4_t const previous = vextq_u32(carriedVector, working, 4 - kNumChannels);

			// Along x: t[x] * (65536 - f) + t[x + 1] * f == (t[x] << 16) + (t[x + 1] - t[x]) * f, mod 2^32,
			// and the true value fits in an int32
			int32x4_t const fraction = vreinterpretq_s32_u32(vmovl_u16(vshrn_n_u32(working, 9)));
			int16x4_t const low1 = vmovn_s32(vreinterpretq_s32_u32(pairs1));
			int16x4_t const high1 = vshrn_n_s32(vreinterpretq_s32_u32(pairs1), 16);
			int32x4_t const value1 = vmlaq_s32(vshll_n_s16(low1, 16), vsubl_s16(high1, low1), fraction);
			int16x4_t const low2 = vmovn_s32(vreinterpretq_s32_u32(pairs2));
			int16x4_t const high2 = vshrn_n_s32(vreinterpretq_s32_u32(pairs2), 16);
			int32x4_t const value2 = vmlaq_s32(vshll_n_s16(low2, 16), vsubl_s16(high2, low2), fraction);

			// Along y: strength2 = (previous << 5) & INT32_MAX, strength1 = INT32_MAX - strength2.
			// smmul(a, b) == vqdmulh(a, b) >> 1 (floor of a floor), with no saturation as the strengths are never
			// INT32_MIN. (Strength first: SIMDe's host vqdmulh, which the spec runs, wrongly saturates when its
			// first operand is INT32_MIN, as a value can be.)
			int32x4_t const strength2 = vreinterpretq_s32_u32(vshrq_n_u32(vshlq_n_u32(previous, 6), 1));
			int32x4_t const strength1 = vsubq_s32(maxStrength, strength2);
			int32x4_t result = vshrq_n_s32(vqdmulhq_s32(strength1, value1), 1);
			result = vsraq_n_s32(result, vqdmulhq_s32(strength2, value2), 1);
			result = vshlq_s32(vshlq_s32(result, finalRShift), finalLShift);
			vst1q_s32(&buffer[i], result);

			carriedVector = working;
			if constexpr (kNumChannels == 1) {
				carried[0] = w3;
			}
			else {
				carried[0] = w2;
				carried[1] = w3;
			}
		}

		for (size_t c = 0; c < kNumChannels; c++) {
			lastWorkingValues[c] = carried[c];
		}
	}

	// The last 1-3 samples
	saturateTanHAntialiasedScalar<kNumChannels>(&buffer[vectorCount], count - vectorCount, lastWorkingValues,
	                                            saturationAmount, shiftAmount);
}

} // namespace deluge::dsp
