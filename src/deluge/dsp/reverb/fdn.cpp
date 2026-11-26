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

#include "dsp/reverb/fdn.hpp"
#include "definitions_cxx.hpp"
#include "memory/memory_allocator_interface.h"
#include "util/fixedpoint.h"
#include <algorithm>
#include <argon.hpp>
#include <cmath>
#include <numbers>

namespace deluge::dsp::reverb {

namespace {

constexpr float kFromQ31 = 1.f / 2147483648.f;
constexpr float kToQ31 = 2147483648.f;

// Decay knob position k = 127 * room size. RT60 = 0.45937 s * e^(0.025586 k) up to k = 116 (x2.27 per 32 steps),
// then a steeper e^(0.18065 (k - 117)) from 9.05 s at 117 to 46 s at 126, and infinite at 127.
constexpr float kDecayKnobMax = 127.f;
constexpr float kRt60Base = 0.45937f;
constexpr float kRt60Slope = 0.025586f;
constexpr float kRt60TopKnee = 117.f;
constexpr float kRt60TopBase = 9.0536f;
constexpr float kRt60TopSlope = 0.18065f;

/// 1 / RT60 at a whole knob position.
float inverseRt60AtKnob(int32_t k) {
	if (k >= 127) {
		return 0.f;
	}
	if (k <= 116) {
		return 1.f / (kRt60Base * std::exp(kRt60Slope * static_cast<float>(k)));
	}
	return 1.f / (kRt60TopBase * std::exp(kRt60TopSlope * (static_cast<float>(k) - kRt60TopKnee)));
}

/// 1 / RT60 for a fractional knob position, linear in 1 / RT60 between whole positions above the knee.
float inverseRt60(float k) {
	if (k <= 116.f) {
		return 1.f / (kRt60Base * std::exp(kRt60Slope * k));
	}
	auto lower = static_cast<int32_t>(k);
	float frac = k - static_cast<float>(lower);
	return inverseRt60AtKnob(lower) + frac * (inverseRt60AtKnob(lower + 1) - inverseRt60AtKnob(lower));
}

// Diffusion amount (scales every allpass coefficient) against the decay knob, every 4 positions from 0 to 60; it
// stays at 0.715 from 60 up.
constexpr std::array<float, 16> kDiffusionCurve{0.6250f, 0.6277f, 0.6307f, 0.6340f, 0.6376f, 0.6417f, 0.6462f, 0.6512f,
                                                0.6567f, 0.6628f, 0.6696f, 0.6771f, 0.6854f, 0.6946f, 0.7048f, 0.7150f};

float diffusionAmount(float k) {
	float pos = k / 4.f;
	if (pos >= static_cast<float>(kDiffusionCurve.size() - 1)) {
		return kDiffusionCurve.back();
	}
	auto i = static_cast<size_t>(pos);
	float frac = pos - static_cast<float>(i);
	return kDiffusionCurve[i] + frac * (kDiffusionCurve[i + 1] - kDiffusionCurve[i]);
}

// Shelf corner (Hz) against a 0..127 shelf position, every 8 positions and at 127. Not log-linear.
constexpr std::array<float, 17> kShelfCorner{974.1f,  1371.0f,  1819.0f,  2323.3f,  2889.3f, 3521.7f,
                                             4224.5f, 4999.9f,  5848.3f,  6766.7f,  7748.5f, 8783.1f,
                                             9856.1f, 10949.7f, 12044.4f, 13120.5f, 14032.6f};

float shelfCorner(float pos) {
	if (pos >= 120.f) {
		float frac = (pos - 120.f) / 7.f;
		return kShelfCorner[15] + std::min(frac, 1.f) * (kShelfCorner[16] - kShelfCorner[15]);
	}
	float x = std::max(pos, 0.f) / 8.f;
	auto i = static_cast<size_t>(x);
	float frac = x - static_cast<float>(i);
	return kShelfCorner[i] + frac * (kShelfCorner[i + 1] - kShelfCorner[i]);
}

/// Pole of a one-pole filter from the normalised cutoff the other models compute (wc = fc / (1 + fc), fc in cycles
/// per sample).
float onePolePole(float wc) {
	float fc = wc / (1.f - wc);
	return std::exp(-2.f * std::numbers::pi_v<float> * fc);
}

// 4x4 transpose: from four vectors of four frames each to four vectors of one frame each, and back
[[gnu::always_inline]] inline void transposeFour(float32x4_t& a, float32x4_t& b, float32x4_t& c, float32x4_t& d) {
#if defined(__arm__)
	// Two vtrn and two vswp in place. Written out because GCC turns the vcombine() form below into a string of
	// register moves
	asm("vtrn.32 %q0, %q1\n\t"
	    "vtrn.32 %q2, %q3\n\t"
	    "vswp %f0, %e2\n\t"
	    "vswp %f1, %e3"
	    : "+w"(a), "+w"(b), "+w"(c), "+w"(d));
#else
	float32x4x2_t ab = vtrnq_f32(a, b);
	float32x4x2_t cd = vtrnq_f32(c, d);
	a = vcombine_f32(vget_low_f32(ab.val[0]), vget_low_f32(cd.val[0]));
	b = vcombine_f32(vget_low_f32(ab.val[1]), vget_low_f32(cd.val[1]));
	c = vcombine_f32(vget_high_f32(ab.val[0]), vget_high_f32(cd.val[0]));
	d = vcombine_f32(vget_high_f32(ab.val[1]), vget_high_f32(cd.val[1]));
#endif
}

constexpr size_t lineMemorySize() {
	size_t total = 0;
	for (uint32_t length : Fdn::kLineLength) {
		total += length + Fdn::kMaxBlock; // each line has a block's overhang after it, see DelayLines
	}
	return total;
}

} // namespace

static_assert(Fdn::kPredelay >= Fdn::kGrain, "the core renders up to kGrain - 1 frames ahead of the input");
static_assert(Fdn::kMaxBlock % Fdn::kGrain == 0);
static_assert(Fdn::kMaxBlock + Fdn::kGrain + Fdn::kPredelay <= 256, "predelay and output rings too small");
static_assert(Fdn::kDiffuserLength[0] >= 16, "sixteen frames of an allpass are computed at once");
static_assert(Fdn::kGrain % 16 == 0, "the allpass loop steps sixteen frames at a time");

Fdn::Fdn() {
	static_assert(diffuserRingsFit());
	line_memory_ = static_cast<float*>(allocLowSpeed(lineMemorySize() * sizeof(float)));
	if (line_memory_ != nullptr) {
		float* next = line_memory_;
		for (size_t i = 0; i < kNumLines; i++) {
			size_t size = kLineLength[i] + kMaxBlock;
			lines_.setBuffer(i, {next, size});
			next += size;
		}
	}
	clear();
	updateDecay();
	setDamping(damping_);
	setWidth(width_);
	setHPF(hpf_);
	setLPF(lpf_);
}

Fdn::~Fdn() {
	if (line_memory_ != nullptr) {
		delugeDealloc(line_memory_);
	}
}

void Fdn::clear() {
	if (line_memory_ != nullptr) {
		lines_.mute();
		for (size_t i = 0; i < kNumLines; i++) {
			lines_.pos[i] = lines_.begin[i];
		}
		lines_.updateFramesToWrap();
	}
	diffuser_memory_.fill(0.f);
	predelay_.fill(0);
	out_l_.fill(0);
	out_r_.fill(0);
	shelf_in_ = {};
	shelf_out_ = {};
	lp_state_ = 0.f;
	hp_state_ = 0.f;
	written_ = 0;
	rendered_ = 0;
}

void Fdn::setRoomSize(float value) {
	room_size_ = std::clamp(value, 0.f, 1.f);
	updateDecay();
}

void Fdn::updateDecay() {
	float k = room_size_ * kDecayKnobMax;
	bool frozen = k >= kDecayKnobMax - 0.01f;

	// Each line loses 60 dB per RT60: G = 10^(-3 * length / (RT60 * fs))
	float inv_rt60 = frozen ? 0.f : inverseRt60(k);
	constexpr float kLn1000 = 6.907755279f;
	for (size_t i = 0; i < kNumLines; i++) {
		gain_[i] = std::exp(-kLn1000 * kDecayLength[i] * inv_rt60 / static_cast<float>(kSampleRate));
	}

	float diffusion = diffusionAmount(k);
	for (size_t i = 0; i < kNumDiffusers; i++) {
		diffuser_[i] = kDiffuserCoefficient[i] * diffusion;
	}

	// A frozen loop is lossless, so anything still fed into it would pile up without bound: stop the input instead
	lp_in_ = frozen ? 0.f : (1.f - lp_pole_);
}

void Fdn::setDamping(float value) {
	damping_ = std::clamp(value, 0.f, 1.f);

	// One position along the shelf's corner/gain diagonal: 0 is flat, the song default (0.72) is a 5.6 kHz corner
	// at -6.5 dB, and 1 is a one-pole lowpass at 974 Hz
	float amount = std::pow(1.f - damping_, 0.6f);
	float hf_gain = amount;
	float corner = shelfCorner(amount * 127.f);

	// Bilinear first-order shelf, unity at DC and hf_gain at Nyquist
	float k = std::tan(std::numbers::pi_v<float> * corner / static_cast<float>(kSampleRate));
	float t = k / (1.f + k);
	shelf_[0] = t + (1.f - t) * hf_gain; // b0
	shelf_[1] = t - (1.f - t) * hf_gain; // b1
	shelf_[2] = 2.f * t - 1.f;           // a1
}

void Fdn::setWidth(float value) {
	width_ = std::clamp(value, 0.f, 1.f);
	float scale = kOutputGain * kToQ31;
	out_mix_[0] = scale * (1.f + width_) * 0.5f;
	out_mix_[1] = scale * (1.f - width_) * 0.5f;
}

void Fdn::setHPF(float f) {
	hpf_ = f;
	hp_pole_ = onePolePole(calcFilterCutoff<FilterType::HighPass>(f));
	hp_in_ = (1.f + hp_pole_) * 0.5f;
}

void Fdn::setLPF(float f) {
	lpf_ = f;
	lp_pole_ = onePolePole(calcFilterCutoff<FilterType::LowPass>(f));
	updateDecay(); // for lp_in_
}

template <bool kFirstGroup>
[[gnu::always_inline]] inline void Fdn::renderGroup(size_t group, const float* x, float* wet_l, float* wet_r,
                                                    uint32_t numFrames) {
	float* p0 = lines_.pos[group * 4 + 0];
	float* p1 = lines_.pos[group * 4 + 1];
	float* p2 = lines_.pos[group * 4 + 2];
	float* p3 = lines_.pos[group * 4 + 3];

	// Coefficients as vector lanes, for the by-scalar multiplies: shelf (b0, b1) and (a1, -), the four decay gains
	// G_k, and -G_k / 2
	const float32x2_t shelf_b = vld1_f32(&shelf_[0]);
	const float32x2_t shelf_a = vld1_f32(&shelf_[2]);
	const float32x4_t gain = vld1q_f32(&gain_[group * 4]);
	const float32x4_t neg_half = vmulq_n_f32(gain, -0.5f);
	const float32x2_t g01 = vget_low_f32(gain);
	const float32x2_t g23 = vget_high_f32(gain);
	const float32x2_t h01 = vget_low_f32(neg_half);
	const float32x2_t h23 = vget_high_f32(neg_half);

	// Frame-major shelf state: one lane per line
	float32x4_t r_prev = vld1q_f32(shelf_in_[group].data());
	float32x4_t z_prev = vld1q_f32(shelf_out_[group].data());

	for (uint32_t i = 0; i < numFrames; i += 4) {
		// The lines stream through external RAM: ask for the cache lines a few vectors ahead
		__builtin_prefetch(p0 + 32);
		__builtin_prefetch(p1 + 32);
		__builtin_prefetch(p2 + 32);
		__builtin_prefetch(p3 + 32);

		// One line per vector, four frames each. The diffused input goes into the group's first line
		float32x4_t r0 = vaddq_f32(vld1q_f32(p0), vld1q_f32(x + i));
		float32x4_t r1 = vld1q_f32(p1);
		float32x4_t r2 = vld1q_f32(p2);
		float32x4_t r3 = vld1q_f32(p3);

		// One frame per vector, to run the shelves along frame by frame
		transposeFour(r0, r1, r2, r3);
		// z = b0 r + b1 r1 - a1 z1. Only the last term is on the frame-to-frame chain
		float32x4_t z0 = vmlaq_lane_f32(vmulq_lane_f32(r0, shelf_b, 0), r_prev, shelf_b, 1);
		float32x4_t z1 = vmlaq_lane_f32(vmulq_lane_f32(r1, shelf_b, 0), r0, shelf_b, 1);
		float32x4_t z2 = vmlaq_lane_f32(vmulq_lane_f32(r2, shelf_b, 0), r1, shelf_b, 1);
		float32x4_t z3 = vmlaq_lane_f32(vmulq_lane_f32(r3, shelf_b, 0), r2, shelf_b, 1);
		z0 = vmlsq_lane_f32(z0, z_prev, shelf_a, 0);
		z1 = vmlsq_lane_f32(z1, z0, shelf_a, 0);
		z2 = vmlsq_lane_f32(z2, z1, shelf_a, 0);
		z3 = vmlsq_lane_f32(z3, z2, shelf_a, 0);
		r_prev = r3;
		z_prev = z3;

		// And back to one line per vector
		transposeFour(z0, z1, z2, z3);

		// Output taps: alternate lines to each side, the other way round in the second group
		if constexpr (kFirstGroup) {
			vst1q_f32(wet_l + i, vaddq_f32(z0, z2));
			vst1q_f32(wet_r + i, vaddq_f32(z1, z3));
		}
		else {
			vst1q_f32(wet_l + i, vaddq_f32(vld1q_f32(wet_l + i), vaddq_f32(z1, z3)));
			vst1q_f32(wet_r + i, vaddq_f32(vld1q_f32(wet_r + i), vaddq_f32(z0, z2)));
		}

		// Feedback: line_k <- G_k z_k - (1/2) sum_j G_j z_j, i.e. (I - J/2) diag(G) z, with I - J/2 orthogonal
		float32x4_t neg_sum = vaddq_f32(vmlaq_lane_f32(vmulq_lane_f32(z0, h01, 0), z1, h01, 1),
		                                vmlaq_lane_f32(vmulq_lane_f32(z2, h23, 0), z3, h23, 1));
		vst1q_f32(p0, vmlaq_lane_f32(neg_sum, z0, g01, 0));
		vst1q_f32(p1, vmlaq_lane_f32(neg_sum, z1, g01, 1));
		vst1q_f32(p2, vmlaq_lane_f32(neg_sum, z2, g23, 0));
		vst1q_f32(p3, vmlaq_lane_f32(neg_sum, z3, g23, 1));
		p0 += 4;
		p1 += 4;
		p2 += 4;
		p3 += 4;
	}

	vst1q_f32(shelf_in_[group].data(), r_prev);
	vst1q_f32(shelf_out_[group].data(), z_prev);
	lines_.pos[group * 4 + 0] = p0;
	lines_.pos[group * 4 + 1] = p1;
	lines_.pos[group * 4 + 2] = p2;
	lines_.pos[group * 4 + 3] = p3;
}

// Renders numFrames (a multiple of kGrain, at most kMaxBlock) frames of output from rendered_ on, into the output
// rings
void Fdn::renderBlock(uint32_t numFrames) {
	auto& x = x_;
	auto& wet_l = wet_l_;
	auto& wet_r = wet_r_;

	// Predelay, then the input low-pass and high-pass (one-pole each)
	{
		float lp = lp_state_;
		float hp = hp_state_;
		const float lp_pole = lp_pole_;
		const float lp_in = lp_in_;
		const float hp_pole = hp_pole_;
		const float hp_in = hp_in_;
		uint32_t read = rendered_ - kPredelay;
		for (uint32_t i = 0; i < numFrames; i++) {
			float in = static_cast<float>(predelay_[(read + i) & kRingMask]) * kFromQ31;
			float v = lp_pole * lp + lp_in * in;
			hp = hp_pole * hp + hp_in * (v - lp);
			lp = v;
			x[i] = hp;
		}
		lp_state_ = lp;
		hp_state_ = hp;
	}

	// Allpasses in series: t = x + a d; y = d - a t (= -a x + (1 - a^2) d), with d the ring's sample from L ago. A
	// ring isn't read back for at least its length (more than four frames), so each does four frames at once. The
	// rings are powers of two long; a vector written at the start is mirrored just past the end, so a read that
	// wraps round can still be one vector load.
	// Four vectors per step, so their multiply chains overlap: fine as every ring is at least sixteen long.
	{
		const uint32_t base = rendered_;
		float* __restrict__ ring = diffuser_memory_.data();
		float* __restrict__ samples = x.data();
		for (size_t k = 0; k < kNumDiffusers; k++) {
			const uint32_t size = kDiffuserRingSize[k];
			const uint32_t mask = size - 1;
			const uint32_t length = kDiffuserLength[k];
			const float32x2_t a = vdup_n_f32(diffuser_[k]);
			for (uint32_t i = 0; i < numFrames; i += 16) {
				const uint32_t write = (base + i) & mask; // a multiple of 16, and the ring longer: no wrap
				const uint32_t read = (base + i - length) & mask;
				float32x4_t d0 = vld1q_f32(ring + read);
				float32x4_t d1 = vld1q_f32(ring + ((read + 4) & mask));
				float32x4_t d2 = vld1q_f32(ring + ((read + 8) & mask));
				float32x4_t d3 = vld1q_f32(ring + ((read + 12) & mask));
				float32x4_t t0 = vmlaq_lane_f32(vld1q_f32(samples + i), d0, a, 0);
				float32x4_t t1 = vmlaq_lane_f32(vld1q_f32(samples + i + 4), d1, a, 0);
				float32x4_t t2 = vmlaq_lane_f32(vld1q_f32(samples + i + 8), d2, a, 0);
				float32x4_t t3 = vmlaq_lane_f32(vld1q_f32(samples + i + 12), d3, a, 0);
				vst1q_f32(samples + i, vmlsq_lane_f32(d0, t0, a, 0));
				vst1q_f32(samples + i + 4, vmlsq_lane_f32(d1, t1, a, 0));
				vst1q_f32(samples + i + 8, vmlsq_lane_f32(d2, t2, a, 0));
				vst1q_f32(samples + i + 12, vmlsq_lane_f32(d3, t3, a, 0));
				vst1q_f32(ring + write, t0);
				vst1q_f32(ring + write + 4, t1);
				vst1q_f32(ring + write + 8, t2);
				vst1q_f32(ring + write + 12, t3);
				if (write == 0) {
					vst1q_f32(ring + size, t0);
				}
			}
			ring += size + kDiffuserGuard;
		}
	}

	// The two networks
	lines_.startBlock(numFrames);
	renderGroup<true>(0, x.data(), wet_l.data(), wet_r.data(), numFrames);
	renderGroup<false>(1, x.data(), wet_l.data(), wet_r.data(), numFrames);
	lines_.endBlock(numFrames);

	// Width, level and conversion to q31 (saturating) into the output rings
	const float32x2_t mix = vld1_f32(&out_mix_[0]); // (direct, cross)
	const uint32_t base = rendered_;
	int32_t* __restrict__ ring_l = out_l_.data();
	int32_t* __restrict__ ring_r = out_r_.data();
	for (uint32_t i = 0; i < numFrames; i += 4) {
		float32x4_t l = vld1q_f32(&wet_l[i]);
		float32x4_t r = vld1q_f32(&wet_r[i]);
		float32x4_t out_l = vmlaq_lane_f32(vmulq_lane_f32(l, mix, 0), r, mix, 1);
		float32x4_t out_r = vmlaq_lane_f32(vmulq_lane_f32(r, mix, 0), l, mix, 1);
		uint32_t index = (base + i) & kRingMask;
		vst1q_s32(ring_l + index, vcvtq_s32_f32(out_l));
		vst1q_s32(ring_r + index, vcvtq_s32_f32(out_r));
	}

	rendered_ += numFrames;
}

[[gnu::noinline]] void Fdn::process(std::span<int32_t> input, std::span<StereoSample> output) {
	if (line_memory_ == nullptr) {
		return;
	}

	const int32_t pan_l = getPanLeft();
	const int32_t pan_r = getPanRight();

	for (size_t start = 0; start < input.size(); start += kMaxBlock) {
		const auto numFrames = static_cast<uint32_t>(std::min<size_t>(kMaxBlock, input.size() - start));

		// Into the predelay ring
		for (uint32_t i = 0; i < numFrames; i++) {
			predelay_[(written_ + i) & kRingMask] = input[start + i];
		}
		const uint32_t first = written_;
		written_ += numFrames;

		// Render in whole grains until the output covers everything written. That can run up to kGrain - 1 frames
		// past the input, which is fine as long as the predelay is at least that long
		const uint32_t target = (written_ + kGrain - 1) & ~(kGrain - 1);
		while (static_cast<int32_t>(target - rendered_) > 0) {
			renderBlock(std::min<uint32_t>(kMaxBlock, target - rendered_));
		}

		// Mix this call's frames in
		for (uint32_t i = 0; i < numFrames; i++) {
			uint32_t index = (first + i) & kRingMask;
			StereoSample& s = output[start + i];
			s.l += multiply_32x32_rshift32_rounded(out_l_[index], pan_l);
			s.r += multiply_32x32_rshift32_rounded(out_r_[index], pan_r);
		}
	}
}

} // namespace deluge::dsp::reverb
