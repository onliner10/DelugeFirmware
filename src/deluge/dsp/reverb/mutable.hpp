// Copyright 2023 Katherine Whitlock
// Copyright 2014 Emilie Gillet
// Reverb.

#pragma once
#include "definitions_cxx.hpp"
#include "dsp/reverb/base.hpp"
#include "dsp/util.hpp"
#include "fx_engine.hpp"
#include <array>
#include <limits>

namespace deluge::dsp::reverb {

class Mutable : public Base {
	constexpr static size_t kBufferSize = 32768;

public:
	Mutable() = default;

	~Mutable() override = default;

	void process(std::span<int32_t> in, std::span<StereoSample> output) override {
		// This is the Griesinger topology described in the Dattorro paper
		// (4 AP diffusers on the input, then a loop of 2x 2AP+1Delay).
		// Modulation is applied to the two long delays for a slow shimmer/chorus effect.
		// Upstream also smears the first diffuser AP; that is deliberately not ported.
		typename FxEngine::AllPass ap1(150);
		typename FxEngine::AllPass ap2(214);
		typename FxEngine::AllPass ap3(319);
		typename FxEngine::AllPass ap4(527);

		typename FxEngine::AllPass dap1a(2182);
		typename FxEngine::AllPass dap1b(2690);
		typename FxEngine::AllPass del1(4501);

		typename FxEngine::AllPass dap2a(2525);
		typename FxEngine::AllPass dap2b(2197);
		typename FxEngine::AllPass del2(6312);

		typename FxEngine::Context c;
		FxEngine::ConstructTopology(engine_, //<
		                            {
		                                &ap1, &ap2, &ap3, &ap4, //<
		                                &dap1a, &dap1b, &del1,  //<
		                                &dap2a, &dap2b, &del2,  //<
		                            });

		const float kap = diffusion_;
		const float klp = lp_;
		const float krt = reverb_time_;

		float lp_1 = lp_decay_1_;
		float lp_2 = lp_decay_2_;
		float hp_r = hp_r_;
		float hp_l = hp_l_;
		float lp_r = lp_r_;
		float lp_l = lp_l_;

		// Four frames at a time. Nothing in this topology comes back round in fewer than 149 frames,
		// so each line can take a block's four frames in one go; only the one-pole filters are serial.
		// Lanes run backwards through the block, see FxEngine::Quad.
		size_t frame = 0;
		if (in.size() >= 4) {
			// Branch 1 then branch 2 in each pair: {lp_1, lp_2}, {hp_r, hp_l}, {lp_r, lp_l}.
			float32x2_t loop_lp = {lp_1, lp_2};
			float32x2_t out_hp = {hp_r, hp_l};
			float32x2_t out_lp = {lp_r, lp_l};

			// Coefficients go in as register lanes. Handed over as plain floats, GCC rebuilds a
			// register from the stack for every multiply.
			const float32x2_t diffuse = {kap, -kap};
			const float32x2_t diffuse_negated = {-kap, kap};
			const float32x2_t loop = {krt, klp};
			const float32x2_t filters = {hp_cutoff_, lp_cutoff_};
			const float32x2_t lfo_depth = {50.0f, 40.0f};
			// Dividing by INT32_MAX as a float is a multiply by 2^-31, exactly.
			const float32x2_t gain = {1.f / static_cast<float>(std::numeric_limits<int32_t>::max()), kOutputGain};
			const int32x2_t pan = {getPanLeft(), getPanRight()};
			const float32x4_t lfo_centre_1 = vdupq_n_f32(6261.0f);
			const float32x4_t lfo_centre_2 = vdupq_n_f32(4460.0f);

			for (; frame + 4 <= in.size(); frame += 4) {
				const FxEngine::Quad q = engine_.Advance4();

				float32x4_t c = vmulq_lane_f32(vcvtq_f32_s32(reverse(vld1q_s32(&in[frame]))), gain, 0);

				// Diffuse through 4 allpasses.
				c = ap1.Process(q, c, diffuse);
				c = ap2.Process(q, c, diffuse);
				c = ap3.Process(q, c, diffuse);
				c = ap4.Process(q, c, diffuse);
				const float32x4_t apout = c;

				// Main reverb loop. Each branch reads the other's delay thousands of frames back, so
				// within a block they don't touch and can be interleaved.
				const float32x4_t lfo_offset_1 = vmlaq_lane_f32(lfo_centre_1, q.lfo[LFO_2], lfo_depth, 0);
				const float32x4_t lfo_offset_2 = vmlaq_lane_f32(lfo_centre_2, q.lfo[LFO_1], lfo_depth, 1);
				float32x4_t c1 = vmlaq_lane_f32(apout, del2.Interpolate(q, lfo_offset_1), loop, 0);
				float32x4_t c2 = vmlaq_lane_f32(apout, del1.Interpolate(q, lfo_offset_2), loop, 0);

				forEachFramePair(c1, c2, [&](float32x2_t x) {
					loop_lp = vmla_lane_f32(loop_lp, vsub_f32(x, loop_lp), loop, 1);
					return loop_lp;
				});

				c1 = dap1a.Process(q, c1, diffuse_negated);
				c1 = dap1b.Process(q, c1, diffuse);
				del1.Store(q, 0, c1);
				c1 = vaddq_f32(c1, c1); // c.Multiply(2.0f), exactly

				c2 = dap2a.Process(q, c2, diffuse_negated);
				c2 = dap2b.Process(q, c2, diffuse);
				del2.Store(q, 0, c2);
				c2 = vaddq_f32(c2, c2);

				forEachFramePair(c1, c2, [&](float32x2_t wet) {
					out_hp = vmla_lane_f32(out_hp, vsub_f32(wet, out_hp), filters, 0);
					wet = vsub_f32(wet, out_hp);
					out_lp = vmla_lane_f32(out_lp, vsub_f32(wet, out_lp), filters, 1);
					return out_lp;
				});

				// Mix, as {l, r} pairs in frame order.
				const int32x4x2_t wet = vzipq_s32(multiplyRounded<0>(reverse(toQ31Saturating(c2, gain)), pan),
				                                  multiplyRounded<1>(reverse(toQ31Saturating(c1, gain)), pan));
				int32_t* mix = reinterpret_cast<int32_t*>(&output[frame]);
				vst1q_s32(mix, vaddq_s32(vld1q_s32(mix), wet.val[0]));
				vst1q_s32(mix + 4, vaddq_s32(vld1q_s32(mix + 4), wet.val[1]));
			}

			lp_1 = vget_lane_f32(loop_lp, 0);
			lp_2 = vget_lane_f32(loop_lp, 1);
			hp_r = vget_lane_f32(out_hp, 0);
			hp_l = vget_lane_f32(out_hp, 1);
			lp_r = vget_lane_f32(out_lp, 0);
			lp_l = vget_lane_f32(out_lp, 1);
		}

		// The frames left over, one at a time.
		for (; frame < in.size(); frame++) {
			StereoSample& s = output[frame];
			float wet = 0;
			float apout = 0.0f;
			engine_.Advance();

			// Smear AP1 inside the loop.
			// c.Interpolate(ap1, 10.0f, LFO_1, 80.0f, 1.0f);
			// c.Write(ap1, 100, 0.0f);

			const float input_sample = in[frame] / static_cast<float>(std::numeric_limits<int32_t>::max());

			c.Set(input_sample);

			// Diffuse through 4 allpasses.
			ap1.Process(c, kap);
			ap2.Process(c, kap);
			ap3.Process(c, kap);
			ap4.Process(c, kap);
			apout = c.Get();

			// Main reverb loop.
			c.Set(apout);
			del2.Interpolate(c, 6261.0f, LFO_2, 50.0f, krt);
			c.Lp(lp_1, klp);
			dap1a.Process(c, -kap);
			dap1b.Process(c, kap);
			del1.Write(c, 2.0f);
			wet = c.Get();
			wet = wet - dsp::OnePole(hp_r, wet, hp_cutoff_);
			wet = dsp::OnePole(lp_r, wet, lp_cutoff_);

			auto output_right = dsp::toQ31Saturating(wet * kOutputGain);

			c.Set(apout);
			del1.Interpolate(c, 4460.0f, LFO_1, 40.0f, krt);
			c.Lp(lp_2, klp);
			dap2a.Process(c, -kap);
			dap2b.Process(c, kap);
			del2.Write(c, 2.0f);
			wet = c.Get();
			wet = wet - dsp::OnePole(hp_l, wet, hp_cutoff_);
			wet = dsp::OnePole(lp_l, wet, lp_cutoff_);

			auto output_left = dsp::toQ31Saturating(wet * kOutputGain);

			// Mix
			s.l += multiply_32x32_rshift32_rounded(output_left, getPanLeft());
			s.r += multiply_32x32_rshift32_rounded(output_right, getPanRight());
		}

		lp_decay_1_ = lp_1;
		lp_decay_2_ = lp_2;
		hp_r_ = hp_r;
		hp_l_ = hp_l;
		lp_r_ = lp_r;
		lp_l_ = lp_l;
	}

	inline void Clear() { engine_.Clear(); }

	/// Lifts the normalised loop signal back into q31. Long spelled UINT32_MAX * 0xF; kept at
	/// exactly that value, since it is what the models were balanced around.
	static constexpr float kOutputGain = 15.f * 4294967296.f;

	static constexpr float kReverbTimeMin = 0.01f;
	static constexpr float kReverbTimeMax = 0.98f;
	static constexpr float kWidthMin = 0.1f;
	static constexpr float kWidthMax = 0.9f;
	/// Range of the one-pole coefficient driving the damping filter inside the reverb loop. kLpMax is fully
	/// transparent, kLpMin is a cutoff of roughly 70Hz, past which there is nothing left of the tail to damp.
	static constexpr float kLpMin = 0.01f;
	static constexpr float kLpMax = 1.f;

	// Reverb Base Overrides
	void setRoomSize(float value) override {
		reverb_time_ = util::map(value, 0.f, 1.f, kReverbTimeMin, kReverbTimeMax);
	}
	[[nodiscard]] float getRoomSize() const override {
		return util::map(reverb_time_, kReverbTimeMin, kReverbTimeMax, 0.f, 1.f);
	};

	/// 0 is no damping at all, 1 is as dark as the loop filter goes. The curve is logarithmic so the audible
	/// change is spread evenly over the parameter's range.
	void setDamping(float value) override {
		lp_val_ = value;
		const float damping = std::clamp(std::log2((value * 50.f) + 1.f) / 5.7f, 0.f, 1.f);
		lp_ = util::map(1.f - damping, 0.f, 1.f, kLpMin, kLpMax);
	}
	[[nodiscard]] float getDamping() const override { return lp_val_; }

	void setWidth(float value) override { diffusion_ = util::map(value, 0.f, 1.f, kWidthMin, kWidthMax); }
	[[nodiscard]] float getWidth() const override { return util::map(diffusion_, kWidthMin, kWidthMax, 0.f, 1.f); };

	void setHPF(float f) override {
		hp_cutoff_val_ = f;
		hp_cutoff_ = calcFilterCutoff<FilterType::HighPass>(f);
	}

	[[nodiscard]] float getHPF() const override { return hp_cutoff_val_; }

	void setLPF(float f) override {
		lp_cutoff_val_ = f;
		lp_cutoff_ = calcFilterCutoff<FilterType::LowPass>(f);
	}

	[[nodiscard]] float getLPF() const override { return lp_cutoff_val_; }

protected:
	static constexpr float sample_rate = kSampleRate;

	[[gnu::always_inline]] static float32x4_t reverse(float32x4_t v) {
		v = vrev64q_f32(v);
		return vcombine_f32(vget_high_f32(v), vget_low_f32(v));
	}

	[[gnu::always_inline]] static int32x4_t reverse(int32x4_t v) {
		v = vrev64q_s32(v);
		return vcombine_s32(vget_high_s32(v), vget_low_s32(v));
	}

	/// Runs `step` over a block's frames in order, on one frame of `a` and `b` at a time, replacing
	/// each with what it returns. Lets two independent serial filters share their instructions.
	template <typename Step>
	[[gnu::always_inline]] static void forEachFramePair(float32x4_t& a, float32x4_t& b, Step step) {
		// Lane 3 is the block's first frame.
		const float32x4x2_t pairs = vzipq_f32(a, b);
		const float32x2_t frame_0 = step(vget_high_f32(pairs.val[1]));
		const float32x2_t frame_1 = step(vget_low_f32(pairs.val[1]));
		const float32x2_t frame_2 = step(vget_high_f32(pairs.val[0]));
		const float32x2_t frame_3 = step(vget_low_f32(pairs.val[0]));
		const float32x4x2_t lanes = vuzpq_f32(vcombine_f32(frame_3, frame_2), vcombine_f32(frame_1, frame_0));
		a = lanes.val[0];
		b = lanes.val[1];
	}

	/// dsp::toQ31Saturating(wet * kOutputGain), four at a time, with kOutputGain in lane 1 of `gain`.
	/// NEON's own saturation would stop at INT32_MAX rather than at the clamp.
	[[gnu::always_inline]] static int32x4_t toQ31Saturating(float32x4_t wet, float32x2_t gain) {
		const float32x4_t scaled = vmulq_lane_f32(wet, gain, 1);
		return vcvtq_s32_f32(vminq_f32(vmaxq_f32(scaled, vdupq_n_f32(-2147483648.f)), vdupq_n_f32(2147483520.f)));
	}

	/// multiply_32x32_rshift32_rounded(a, b[lane]), four at a time: smmulr rounds by adding 2^31
	/// before taking the top word, which is what a rounding narrow by 32 does.
	template <int lane>
	[[gnu::always_inline]] static int32x4_t multiplyRounded(int32x4_t a, int32x2_t b) {
		return vcombine_s32(vrshrn_n_s64(vmull_lane_s32(vget_low_s32(a), b, lane), 32),
		                    vrshrn_n_s64(vmull_lane_s32(vget_high_s32(a), b, lane), 32));
	}

	std::array<float, kBufferSize> buffer_{};
	FxEngine engine_{buffer_, {0.5f / sample_rate, 0.3f / sample_rate}};

	// size
	float reverb_time_ = 0.665f;

	// width
	float diffusion_ = 0.625f;

	// damping
	float lp_{0.7f};
	float lp_val_{0.7f};

	// These are the state variables for the low-pass filters
	float lp_decay_1_{0};
	float lp_decay_2_{0};

	// High-pass
	float hp_cutoff_val_{0.f};
	// corresponds to 20Hz
	float hp_cutoff_{calcFilterCutoff<FilterType::HighPass>(0)};
	float hp_l_{0.0}; // HP state variable
	float hp_r_{0.0}; // HP state variable

	// Low-pass
	float lp_cutoff_val_{0.f};
	// corresponds to 0Hz
	float lp_cutoff_{calcFilterCutoff<FilterType::LowPass>(0)};
	float lp_l_{0.0}; // LP state variable
	float lp_r_{0.0}; // LP state variable
};

} // namespace deluge::dsp::reverb
