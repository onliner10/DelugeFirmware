/*
 * Copyright © 2015-2023 Synthstrom Audible Limited
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
#include "deluge/util/fixedpoint.h"
#include "deluge/util/functions.h"
#include <algorithm>
#include <argon.hpp>
#include <cmath>
#include <limits>

namespace deluge::dsp {
/**
 * Fold reduces the input by the amount it's over the level
 */
constexpr q31_t FOLD_MIN = 0.1 * ONE_Q31;
constexpr q31_t THREE_FOURTHS = 0.75 * ONE_Q31;
inline q31_t fold(q31_t input, q31_t level) {
	// no folding occurs if max is 0 or if max is greater than input
	// to keep the knob range consistent fold starts from 0 and
	// increases, decreasing would lead to a large deadspace until
	// suddenly clipping occured
	// note 9db loss
	q31_t extra = 0;
	q31_t max = level >> 8;
	if (input > max) {
		extra = input - max;
	}
	else if (input < -max) {
		extra = input + max;
	}
	// this avoids inverting the wave
	return 2 * extra - input;
}
/**
 * This approximates wavefolding by taking an input between -1 and 1
 * and producing output that flips around zero several times
 */
inline q31_t polynomialOscillatorApproximation(q31_t x) {
	// requires 1 to be ONE_Q31

	q31_t x2 = 2 * multiply_32x32_rshift32(x, x);
	q31_t x3 = 2 * multiply_32x32_rshift32(x2, x);
	// this is 4(3*x/4 - x^3) which is a nice shape
	q31_t r1 = 8 * (multiply_32x32_rshift32(THREE_FOURTHS, x) - x3);

	q31_t r2 = 2 * multiply_32x32_rshift32(r1, r1);
	q31_t r3 = 2 * multiply_32x32_rshift32(r2, r1);
	// at this point we've applied the polynomial twice
	q31_t out = 8 * (multiply_32x32_rshift32(THREE_FOURTHS, r1) - r3);

	return out;
}

inline void foldBufferPolyApproximationScalar(q31_t* startSample, q31_t const* endSample, q31_t level) {
	q31_t* currentSample = startSample;
	q31_t fold_level = add_saturate(level, FOLD_MIN);

	do {
		q31_t c = *currentSample;

		q31_t x = lshiftAndSaturateUnknown(multiply_32x32_rshift32(fold_level, c), 8);

		// volume compensation
		*currentSample = polynomialOscillatorApproximation(x) >> 7;

		currentSample += 1;
	} while (currentSample < endSample);
}

namespace fold_detail {
/// multiply_32x32_rshift32 on 4 lanes, exact for every input: the high words of the 64-bit products
[[gnu::always_inline]] inline int32x4_t multiplyHigh(int32x4_t a, int32x4_t b) {
	int64x2_t const low = vmull_s32(vget_low_s32(a), vget_low_s32(b));
	int64x2_t const high = vmull_s32(vget_high_s32(a), vget_high_s32(b));
	return vcombine_s32(vshrn_n_s64(low, 32), vshrn_n_s64(high, 32));
}

/// multiply_32x32_rshift32 on 4 lanes, by a factor that is never INT32_MIN: vqdmulh only saturates when both
/// are INT32_MIN, and otherwise floor(2ab / 2^32) >> 1 == floor(ab / 2^32).
/// (The factor goes first: SIMDe's host vqdmulh, which the specs run, wrongly saturates when its *first* operand
/// is INT32_MIN. The NEON instruction does not care about the order.)
[[gnu::always_inline]] inline int32x4_t multiplyHighByNotMin(int32x4_t a, int32x4_t notMin) {
	return vshrq_n_s32(vqdmulhq_s32(notMin, a), 1);
}
} // namespace fold_detail

/// Same output as foldBufferPolyApproximationScalar(), bit for bit, 4 samples at a time. Every sample is
/// independent. The "2 *" and "8 *" of polynomialOscillatorApproximation() wrap, and so do the shifts here.
/// The products of two signals (x * x, x2 * x, r1 * r1, r2 * r1) can have both factors INT32_MIN, so they use
/// full 64-bit products; the ones by fold_level (> 0, from add_saturate(level > 0, FOLD_MIN)) and THREE_FOURTHS
/// use vqdmulh.
inline void foldBufferPolyApproximation(q31_t* startSample, q31_t* endSample, q31_t level) {
	using namespace fold_detail;
	q31_t fold_level = add_saturate(level, FOLD_MIN);
	if (fold_level == std::numeric_limits<q31_t>::min()) [[unlikely]] {
		foldBufferPolyApproximationScalar(startSample, endSample, level);
		return;
	}

	int32x4_t const fold_level_vector = vdupq_n_s32(fold_level);
	int32x4_t const three_fourths = vdupq_n_s32(THREE_FOURTHS);
	q31_t* current_sample = startSample;
	for (; endSample - current_sample >= 4; current_sample += 4) {
		int32x4_t const c = vld1q_s32(current_sample);

		// lshiftAndSaturateUnknown(.., 8): a saturating shift saturates to INT32_MAX, not to 0x7FFFFF00
		int32x4_t x = vqshlq_n_s32(multiplyHighByNotMin(c, fold_level_vector), 8);
		x = vbicq_s32(x, vdupq_n_s32(0xFF));

		int32x4_t const x2 = vshlq_n_s32(multiplyHigh(x, x), 1);
		int32x4_t const x3 = vshlq_n_s32(multiplyHigh(x2, x), 1);
		int32x4_t const r1 = vshlq_n_s32(vsubq_s32(multiplyHighByNotMin(x, three_fourths), x3), 3);

		int32x4_t const r2 = vshlq_n_s32(multiplyHigh(r1, r1), 1);
		int32x4_t const r3 = vshlq_n_s32(multiplyHigh(r2, r1), 1);
		int32x4_t const out = vshlq_n_s32(vsubq_s32(multiplyHighByNotMin(r1, three_fourths), r3), 3);

		// volume compensation
		vst1q_s32(current_sample, vshrq_n_s32(out, 7));
	}
	if (current_sample < endSample) {
		foldBufferPolyApproximationScalar(current_sample, endSample, level);
	}
}
/**
 * foldBuffer folds a whole buffer. Works for stereo too
 */
inline void foldBuffer(q31_t* startSample, q31_t* endSample, q31_t foldLevel) {
	q31_t* currentSample = startSample;
	do {
		q31_t outs = fold(*currentSample, foldLevel);
		// volume compensation
		*currentSample = outs + 4 * multiply_32x32_rshift32(outs, foldLevel);

		currentSample += 1;
	} while (currentSample < endSample);
}

/**
 * Converts a float already scaled into q31 range, clamping rather than overflowing.
 *
 * Letting an out-of-range float reach a narrowing cast is undefined behaviour, and the two
 * targets disagree about it in the worst possible way: ARM's VCVT saturates, so an overdriven
 * tail clips to full-scale positive, while x86 yields INT32_MIN, flipping the same sample to
 * full-scale negative. Anything specced on the host would misreport what the hardware does.
 */
inline q31_t toQ31Saturating(float scaled) {
	// (float)INT32_MAX rounds up to 2^31, which is itself out of range, so the ceiling has to
	// be the largest float below it.
	constexpr float kMax = 2147483520.f; // 0x7FFFFF80
	constexpr float kMin = -2147483648.f;
	return static_cast<q31_t>(std::clamp(scaled, kMin, kMax));
}

/**
 * polynomialOscillatorApproximation() applies T(u) = 3u - 8u^3 twice, so for real x it computes
 * P(x) = 9x - 240x^3 + 1728x^5 - 4608x^7 + 4096x^9 (which is T9(sqrt(2) x) / sqrt(2), a 9th-order Chebyshev
 * fold). Past |x| ~ 0.7106 the integer steps wrap, so the antialiased fold below only takes over inside this limit.
 */
constexpr q31_t FOLD_ADAA_LIMIT = 0.71 * ONE_Q31;

/**
 * The mean of P over [x0, x1]: (F(x1) - F(x0)) / (x1 - x0), with F the antiderivative of P. Since F is a
 * polynomial the division cancels out, which leaves no special case for x0 == x1 (where this is P(x0)).
 *
 * F(x) = G(x^2) with G(y) = 4.5y - 60y^2 + 288y^3 - 576y^4 + 409.6y^5, so the divided difference splits into
 * (x0 + x1) * G[y0, y1], and G[y0, y1] is a polynomial in a = y0 + y1 and b = y0 * y1.
 */
inline float foldPolynomialMean(float x0, float x1) {
	float s = x0 + x1;
	float a = (x0 * x0) + (x1 * x1);
	float p = x0 * x1;
	float b = p * p;
	float a2 = a * a;
	// Grouped in pairs rather than nested, which keeps the chain of dependent multiplies short
	float termsA = (4.5f - (60.f * a)) + (a2 * ((288.f - (576.f * a)) + (409.6f * a2)));
	float termsB = (-288.f + (1152.f * a)) - (1228.8f * a2);
	return s * (termsA + (b * (termsB + (409.6f * b))));
}

/**
 * foldBufferPolyApproximation() with first-order antiderivative antialiasing (ADAA1): each output is the mean of
 * the fold over the line from the previous input to this one, rather than the fold of this input alone. That
 * removes much of the aliasing of the degree-9 polynomial, for half a sample of delay and a slight treble cut.
 *
 * Folds every stride-th sample, so a channel of an interleaved stereo buffer is folded with stride 2. lastX is
 * that channel's previous polynomial input, kept between calls. Where either input is past FOLD_ADAA_LIMIT, the
 * sample is the same as foldBufferPolyApproximation() gives.
 */
inline void foldBufferPolyApproximationAntialiased(q31_t* startSample, q31_t* endSample, q31_t level, int32_t stride,
                                                   q31_t& lastX) {
	q31_t fold_level = add_saturate(level, FOLD_MIN);
	q31_t x0 = lastX;

	for (q31_t* currentSample = startSample; currentSample < endSample; currentSample += stride) {
		q31_t x1 = lshiftAndSaturateUnknown(multiply_32x32_rshift32(fold_level, *currentSample), 8);

		if (x0 >= -FOLD_ADAA_LIMIT && x0 <= FOLD_ADAA_LIMIT && x1 >= -FOLD_ADAA_LIMIT && x1 <= FOLD_ADAA_LIMIT)
		    [[likely]] {
			float mean = foldPolynomialMean(static_cast<float>(x0) * 0x1p-31f, static_cast<float>(x1) * 0x1p-31f);
			// The same volume compensation as polynomialOscillatorApproximation(x) >> 7. Inside the limit |mean| < 1,
			// so this can't overflow.
			*currentSample = static_cast<q31_t>(mean * 0x1p24f);
		}
		else {
			*currentSample = polynomialOscillatorApproximation(x1) >> 7;
		}

		x0 = x1;
	}
	lastX = x0;
}
} // namespace deluge::dsp
