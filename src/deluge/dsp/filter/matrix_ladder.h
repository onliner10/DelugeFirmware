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
#include <cstdint>

namespace deluge::dsp::filter {

/// A linear resonant filter written as a state-space system: per sample, s' = A s + B x and y = C s + D x, with the
/// filter's (up to 4) memories s in one NEON register. The matrices only depend on cutoff, resonance and topology, so
/// they're worked out once per buffer (closed form, configure()), and samples go four at a time: their four outputs and
/// the state after them come straight from the state before them (two 4x4 matrix-vector products), so no sample waits
/// on the one before. The cost is the same whatever the topology and the resonance.
///
/// Linear by design - no saturation in the loop, so resonance stops short of self-oscillation.
class MatrixLadder {
public:
	enum class Topology : uint8_t {
		LP4,    ///< 4-pole ladder lowpass (24 dB/oct)
		LP2,    ///< 2-pole ladder lowpass
		HP4,    ///< 4-pole ladder highpass
		HP2,    ///< 2-pole ladder highpass
		SVF_LP, ///< 2-pole state-variable lowpass (12 dB/oct)
		SVF_HP, ///< 2-pole state-variable highpass
	};

	/// g: tan(pi * cutoff / sampleRate) (prewarped cutoff). For the ladders, k is the feedback, 0 to just under the
	/// oscillation point (4 for 4 poles, 2 for 2), with the passband held at unity gain; for the SVFs it's the damping
	/// R = 1 / (2 Q), from 0.707 (no resonance) down to a little over 0
	void configure(Topology topology, float g, float k);
	/// The same matrices worked out plainly, element by element. For the tests and the benchmark
	void configureReference(Topology topology, float g, float k);

	/// Filters a block of q31 samples in place, every `stride`th one (1 for mono, 2 for one channel of interleaved
	/// stereo)
	void process(q31_t* buffer, int32_t numSamples, int32_t stride);
	/// Both channels of an interleaved stereo buffer, one set of coefficients, a state each
	void processStereo(q31_t* buffer, int32_t numFrames);
	/// One sample per step. For the tests and the benchmark
	void processTree(q31_t* buffer, int32_t numSamples, int32_t stride);

	void reset() {
		state_ = vdupq_n_f32(0.f);
		stateR_ = vdupq_n_f32(0.f);
	}

	// Coefficients, public for the tests. Single step: s' = A s + B x, y = C s + D x
	float32x4_t a_[4]; ///< columns of A
	float32x4_t b_;
	float32x4_t c_;
	float d_;
	// Four steps: with X the four inputs, Y = O s + T X and s'''' = A^4 s + P X. T is lower-triangular Toeplitz, so
	// it's held as its diagonals: markov_ = {D, CB, CAB, CA^2B}
	float32x4_t o_[4];  ///< columns of O (row k is C A^k)
	float32x4_t a4_[4]; ///< columns of A^4
	float32x4_t p_[4];  ///< columns of P: A^3 B, A^2 B, A B, B
	float32x4_t markov_;

private:
	void configureFourSteps();

	float32x4_t state_;
	float32x4_t stateR_;
	// What configure() was last given: it does nothing when that hasn't changed. Zeroed (by FilterSet's reset) is
	// never a valid g, so the first configure() always does its work
	Topology lastTopology_;
	float lastG_;
	float lastK_;
};

} // namespace deluge::dsp::filter
