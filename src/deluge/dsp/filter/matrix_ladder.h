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

/// A linear ladder filter (2 or 4 one-pole stages with global feedback, solved without a delay in the loop), written
/// as a state-space system: per sample, s' = A s + B x and y = C s + D x, with the up-to-4 stage memories s in one
/// NEON register. The matrices only depend on cutoff, resonance and topology, so they're worked out once per buffer
/// (closed form, see MatrixLadder::configure()), and a sample costs four vector multiplies, whatever the topology.
/// Linear by design: no saturation in the loop, so resonance stops short of self-oscillation.
class MatrixLadder {
public:
	enum class Topology : uint8_t { LP4, LP2, HP4, HP2 };

	/// g: tan(pi * cutoff / sampleRate) (prewarped cutoff). k: feedback, 0 to just under the oscillation point
	/// (4 for the 4-pole ladders, 2 for the 2-pole ones). Passband gain is held at 1 whatever k.
	void configure(Topology topology, float g, float k);
	/// The same matrices worked out plainly, element by element. For the tests and the benchmark
	void configureReference(Topology topology, float g, float k);

	/// Filters a block of q31 samples in place, every `stride`th one (1 for mono, 2 for one channel of interleaved
	/// stereo). Four samples at a time: their four outputs and the state after them come straight from the state before
	/// them (two 4x4 matrix-vector products), so the samples don't wait on each other
	void process(q31_t* buffer, int32_t numSamples, int32_t stride);
	/// Benchmark only: one sample per step, the step's sum as a tree / as one chain of multiply-accumulates
	void processTree(q31_t* buffer, int32_t numSamples, int32_t stride);
	void processChain(q31_t* buffer, int32_t numSamples, int32_t stride);
	/// Benchmark only: process() with separate multiplies and tree sums
	void processMul(q31_t* buffer, int32_t numSamples, int32_t stride);
	/// Both channels of an interleaved stereo buffer, one set of coefficients, a state each
	void processStereo(q31_t* buffer, int32_t numFrames);

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
	void configureFourStepsFast();
	float32x4_t state_ = vdupq_n_f32(0.f);
	float32x4_t stateR_ = vdupq_n_f32(0.f);
	Topology lastTopology_ = Topology::LP4;
	float lastG_ = -1.f;
	float lastK_ = -1.f;
};

} // namespace deluge::dsp::filter
