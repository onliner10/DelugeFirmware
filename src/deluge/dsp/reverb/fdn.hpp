/*
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

#include "dsp/reverb/base.hpp"
#include "dsp/reverb/freeverb/delay_lines.hpp"
#include "dsp/reverb/freeverb/tuning.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

namespace deluge::dsp::reverb {

/// Feedback delay network reverb: two independent groups of four delay lines, fed by one mono, band-limited,
/// allpass-diffused input. Per sample:
///
///     x = HP(LP(predelay(in)))           one-pole each
///     x = AP8(...AP1(x))                 Schroeder allpasses in series, coefficients scale with the decay
///     per group g of four lines k:
///         r_k = line_k(t - L_k);  r_0 += x
///         z_k = shelf(r_k)               first-order high shelf, unity at DC, the same on all eight lines
///         line_k(t) = G_k z_k - (1/2) sum_j G_j z_j   (Householder reflection I - J/2, times diag(G))
///     left  = zA0 + zA2 + zB1 + zB3
///     right = zA1 + zA3 + zB0 + zB2
///
/// The reflection is orthogonal and the shelf never boosts, so the loop gain is at most max(G_k), and with every
/// G_k = 1 the network holds what is in it indefinitely (freeze). There is no modulation anywhere in the loop.
///
/// Processing is in blocks of whole groups of kGrain frames, all float, four frames per NEON vector. The core runs up
/// to kGrain - 1 frames ahead of the input, which the fixed predelay absorbs, so any render size works.
///
/// The eight long lines (about 126 KiB) are allocated in external RAM when the model is selected; everything else
/// lives in the object.
class Fdn final : public Base {
public:
	Fdn();
	~Fdn() override;
	Fdn(const Fdn&) = delete;
	Fdn& operator=(const Fdn&) = delete;
	Fdn(Fdn&&) = delete;
	Fdn& operator=(Fdn&&) = delete;

	void process(std::span<int32_t> input, std::span<StereoSample> output) override;

	/// Decay time, 0.46 s at 0 to about 46 s just below 1; exactly 1 freezes the tail.
	void setRoomSize(float value) override;
	[[nodiscard]] float getRoomSize() const override { return room_size_; }

	/// High-frequency loss per pass through the loop. 0 is flat, 1 is a one-pole lowpass at about 1 kHz.
	void setDamping(float value) override;
	[[nodiscard]] float getDamping() const override { return damping_; }

	/// Stereo width of the return: 1 is the network's own decorrelated pair, 0 is mono.
	void setWidth(float value) override;
	[[nodiscard]] float getWidth() const override { return width_; }

	/// One-pole high-pass on the input, same law as the other models' HPF.
	void setHPF(float f) override;
	[[nodiscard]] float getHPF() const override { return hpf_; }

	/// One-pole low-pass on the input, same law as the other models' LPF (0 closes it).
	void setLPF(float f) override;
	[[nodiscard]] float getLPF() const override { return lpf_; }

	/// False if the line memory could not be allocated, in which case process() adds nothing.
	[[nodiscard]] bool isReady() const { return line_memory_ != nullptr; }

	/// Clears every delay line and filter state.
	void clear();

	static constexpr size_t kNumLines = 8;
	static constexpr size_t kNumDiffusers = 8;

	/// Line lengths in samples. Primes, mutually coprime; group A then group B.
	static constexpr std::array<uint32_t, kNumLines> kLineLength{2203, 3089, 4337, 6427, 1933, 2857, 4139, 6163};

	/// The length each line's decay gain is computed for, in samples: the line's own length, except line 0 (decays as
	/// if 6% longer) and lines 3 and 7 (as if 3 samples shorter).
	static constexpr std::array<float, kNumLines> kDecayLength{2331.6f, 3089.f, 4337.f, 6424.2f,
	                                                           1933.f,  2857.f, 4139.f, 6160.2f};

	/// Allpass lengths in samples (primes).
	static constexpr std::array<uint32_t, kNumDiffusers> kDiffuserLength{47, 67, 103, 149, 229, 337, 449, 743};

	/// Allpass base coefficients, alternating in sign; each is scaled by the diffusion amount.
	static constexpr std::array<float, kNumDiffusers> kDiffuserCoefficient{0.9921f, -0.9891f, 0.9832f, -0.9758f,
	                                                                       0.963f,  -0.9464f, 0.929f,  -0.8857f};

	/// Fixed predelay in samples (0.77 ms). Must be at least kGrain, see process().
	static constexpr uint32_t kPredelay = 34;

	/// The core renders in multiples of this many frames.
	static constexpr uint32_t kGrain = 16;

	/// Largest block the core renders at once; the long lines' overhang is sized for it.
	static constexpr uint32_t kMaxBlock = ::maxblockframes;

	/// Scales the network's float output into q31 (input full scale = 1.0) before the pan/level multiply. The network
	/// itself has a high wet gain (unity injection into two lines, four lines summed per side), but the send bus runs
	/// far below full scale, so it still needs lifting. 16 puts the return within about 1.5 dB of the Mutable model at
	/// the default song settings (steady noise and tone bursts), and the output saturates for a network level of 1/16,
	/// which is a little more headroom than Mutable's 1/30.
	static constexpr float kOutputGain = 16.f;

private:
	void renderBlock(uint32_t numFrames);
	template <bool kFirstGroup>
	void renderGroup(size_t group, const float* x, float* wet_l, float* wet_r, uint32_t numFrames);
	void updateDecay();

	// Sizes of the allpass rings (powers of two, each at least its length plus kGrain) and of the
	// predelay / output rings
	static constexpr std::array<uint32_t, kNumDiffusers> kDiffuserRingSize{64, 128, 128, 256, 256, 512, 512, 1024};
	static constexpr uint32_t kDiffuserGuard = 4; // a mirror of each ring's first vector, for reads that wrap
	static constexpr uint32_t kDiffuserStorage =
	    64 + 128 + 128 + 256 + 256 + 512 + 512 + 1024 + kNumDiffusers * kDiffuserGuard;
	static constexpr bool diffuserRingsFit() {
		uint32_t total = 0;
		for (size_t i = 0; i < kNumDiffusers; i++) {
			// A ring is read back kGrain frames at a time at its length, while the same frames are written
			if (kDiffuserLength[i] + kGrain > kDiffuserRingSize[i]
			    || (kDiffuserRingSize[i] & (kDiffuserRingSize[i] - 1)) != 0) {
				return false;
			}
			total += kDiffuserRingSize[i] + kDiffuserGuard;
		}
		return total == kDiffuserStorage;
	}
	static constexpr uint32_t kRingSize = 256;
	static constexpr uint32_t kRingMask = kRingSize - 1;

	// Parameters as last set (0..1)
	float room_size_ = 0.6f;
	float damping_ = 0.72f;
	float width_ = 1.f;
	float hpf_ = 0.f;
	float lpf_ = 1.f;

	// Derived coefficients
	alignas(16) std::array<float, kNumLines> gain_{};           // G_k
	std::array<float, kNumDiffusers> diffuser_{};               // allpass coefficients
	alignas(8) std::array<float, 4> shelf_{1.f, 0.f, 0.f, 0.f}; // b0, b1, a1: y = b0 x + b1 x1 - a1 y1
	float lp_pole_ = 0.f;                                       // v = lp_pole v1 + lp_in x
	float lp_in_ = 1.f;                                         // (1 - lp_pole), or 0 while frozen
	float hp_pole_ = 0.f;                                       // y = hp_pole y1 + hp_in (v - v1)
	float hp_in_ = 1.f;
	alignas(8) std::array<float, 2> out_mix_{}; // width matrix (direct, cross), with kOutputGain and the q31 scale

	// Filter states
	float lp_state_ = 0.f;
	float hp_state_ = 0.f;
	alignas(16) std::array<std::array<float, 4>, 2> shelf_in_{};  // last shelf input per line, per group
	alignas(16) std::array<std::array<float, 4>, 2> shelf_out_{}; // last shelf output per line, per group

	// Frame counters (wrapping): how many input frames have been written, and how far the core has rendered
	uint32_t written_ = 0;
	uint32_t rendered_ = 0;

	alignas(16) std::array<int32_t, kRingSize> predelay_{};
	alignas(16) std::array<int32_t, kRingSize> out_l_{};
	alignas(16) std::array<int32_t, kRingSize> out_r_{};
	alignas(16) std::array<float, kDiffuserStorage> diffuser_memory_{};

	// Scratch for one block: the diffused input, then the two sides of the network's output
	alignas(16) std::array<float, kMaxBlock> x_{};
	alignas(16) std::array<float, kMaxBlock> wet_l_{};
	alignas(16) std::array<float, kMaxBlock> wet_r_{};

	freeverb::DelayLines<kNumLines, float> lines_{};
	float* line_memory_ = nullptr;
};

} // namespace deluge::dsp::reverb
