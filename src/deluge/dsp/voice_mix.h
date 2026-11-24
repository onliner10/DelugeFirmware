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

#include "util/fixedpoint.h"
#include <argon.hpp>
#include <array>
#include <cstdint>

// Mixing a voice's mono oscillator buffer into its sound's mono buffer, as Voice::render does when the sound renders
// in mono, doesn't clip and isn't FM. Bit for bit the plain per-sample loop it replaces: every sum, and the amplitude
// ramp, wraps modulo 2^32.
namespace deluge::dsp {

namespace detail {
/// out[i] += smmulr(in[i], amplitude[i]) << 1 for four samples.
/// @details `smmulr(x, a)` is floor((x·a + 2^31) / 2^32). `vqdmulh` gives q = floor(x·a / 2^31), and with
/// x·a = q·2^31 + r, 0 <= r < 2^31, floor((x·a + 2^31) / 2^32) = floor((q + 1) / 2): the r / 2^32 left over is under
/// a half. A rounding shift right by one (`vrsra #1`, which rounds without overflowing) gives exactly that, so adding
/// it to out twice adds smmulr(x, a) << 1, wrapping as the scalar sum does. The one product `vqdmulh` saturates,
/// INT32_MIN · INT32_MIN, gives q = INT32_MAX in place of 2^31; both round to 2^30, the scalar's smmulr.
/// This keeps the 32-bit multiply at one `vqdmulh` per four samples, the A9's cheapest form, rather than `vmull` and
/// a narrow per half.
[[gnu::always_inline]] inline void addFourWithAmplitudes(int32_t* __restrict__ out, int32_t const* __restrict__ in,
                                                         Argon<int32_t> amplitudes) {
	Argon<int32_t> const halfProducts = Argon<int32_t>::Load(in).MultiplyFixedQMax(amplitudes);
	Argon<int32_t>::Load(out)
	    .ShiftRightAccumulateRound<1>(halfProducts)
	    .ShiftRightAccumulateRound<1>(halfProducts)
	    .StoreTo(out);
}
} // namespace detail

/// For each sample: amplitude += increment, then out += smmulr(in, amplitude) << 1.
/// @details Eight samples per iteration in NEON, then four, then up to three scalar. On the A9, NEON's 32-bit
/// multiply runs at half rate and a NEON store costs about 6.5 cycles per 16 bytes, but one `vqdmulh` covers four
/// samples where the scalar loop issues four `smmulr`s, each with its own loads, add and store. Two vectors per
/// iteration keep two independent multiply chains in flight for the in-order NEON pipeline.
/// @param numSamples Must be at least 1.
[[gnu::always_inline]] inline void addMonoWithAmplitudeRamp(int32_t* __restrict__ out, int32_t const* __restrict__ in,
                                                            int32_t numSamples, int32_t amplitude, int32_t increment) {
	uint32_t const step = increment;
	uint32_t amplitudeNow = amplitude;
	int32_t remaining = numSamples;

	if (remaining >= 4) {
		std::array<uint32_t, 4> const firstAmplitudes{amplitudeNow + step, amplitudeNow + step * 2,
		                                              amplitudeNow + step * 3, amplitudeNow + step * 4};
		Argon<int32_t> amplitudes = Argon<uint32_t>::Load(firstAmplitudes.data()).As<int32_t>();
		Argon<int32_t> const step4(static_cast<int32_t>(step * 4));
		Argon<int32_t> const step8(static_cast<int32_t>(step * 8));

		for (; remaining >= 8; remaining -= 8) {
			// Written out stage by stage, both vectors at each, so the in-order NEON pipeline has the other chain to
			// issue while one waits on its multiply.
			Argon<int32_t> const inputA = Argon<int32_t>::Load(in);
			Argon<int32_t> const inputB = Argon<int32_t>::Load(in + 4);
			Argon<int32_t> outputA = Argon<int32_t>::Load(out);
			Argon<int32_t> outputB = Argon<int32_t>::Load(out + 4);
			Argon<int32_t> const halfProductsA = inputA.MultiplyFixedQMax(amplitudes);
			Argon<int32_t> const halfProductsB = inputB.MultiplyFixedQMax(amplitudes + step4);
			outputA = outputA.ShiftRightAccumulateRound<1>(halfProductsA);
			outputB = outputB.ShiftRightAccumulateRound<1>(halfProductsB);
			outputA = outputA.ShiftRightAccumulateRound<1>(halfProductsA);
			outputB = outputB.ShiftRightAccumulateRound<1>(halfProductsB);
			outputA.StoreTo(out);
			outputB.StoreTo(out + 4);
			amplitudes = amplitudes + step8;
			in += 8;
			out += 8;
		}
		if (remaining >= 4) {
			detail::addFourWithAmplitudes(out, in, amplitudes);
			in += 4;
			out += 4;
			remaining -= 4;
		}
		amplitudeNow += step * static_cast<uint32_t>(numSamples - remaining);
	}

	for (; remaining > 0; remaining--) {
		amplitudeNow += step;
		int32_t const product = multiply_32x32_rshift32_rounded(*in++, static_cast<int32_t>(amplitudeNow));
		*out = static_cast<int32_t>(static_cast<uint32_t>(*out) + (static_cast<uint32_t>(product) << 1));
		out++;
	}
}

} // namespace deluge::dsp
