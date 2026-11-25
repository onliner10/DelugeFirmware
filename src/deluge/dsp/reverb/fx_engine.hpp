// Copyright 2023 Katherine Whitlock
// Heavily based on Mutable Instrument's code, copyright 2014 Emilie Gillet
// Base class for building reverb.

#pragma once
#include "cosine_oscillator.hpp"
#include <algorithm>
#include <argon.hpp>
#include <array>
#include <cstdint>
#include <span>

namespace deluge::dsp {
constexpr float OnePole(float& out, float in, float coefficient) {
	out += (coefficient) * ((in)-out);
	return out;
}

template <typename T = float>
constexpr T Interpolate(const T x0, const T x1, float fractional) {
	return static_cast<T>(x0 + (x1 - x0) * fractional);
}
} // namespace deluge::dsp

namespace deluge::dsp::reverb {
constexpr static int32_t TAIL = -1;

#if DELUGE_DSP_BOUNDS_CHECK
namespace debug {
/// Latched when a DelayLine touches a slot outside its own reserved region.
/// Spec-only instrumentation; compiled out of the firmware. See tests/spec/reverb_spec.cpp.
inline bool out_of_bounds_access = false;
} // namespace debug
#endif

enum LFOIndex { LFO_1, LFO_2 };

class FxEngine {
public:
	/// The LFOs only step once every 32 frames, so a configured frequency has to be scaled up
	/// to compensate. Skip this and the normalised frequency stays small enough that the
	/// approximate cosine's `2 - 32f^2` coefficient rounds to exactly 2.0f -- at which point
	/// both lanes collapse onto one much slower oscillation and the modulation stops being
	/// two independent voices.
	constexpr static float kLFOStride = 32.0f;

	FxEngine(std::span<float> signal, std::array<float, 2> lfo_freqs)
	    : buffer_(signal), lfo_{{lfo_freqs[0] * kLFOStride, lfo_freqs[1] * kLFOStride}}, mask(buffer_.size() - 1) {};
	~FxEngine() = default;

	void Clear() {
		std::fill(buffer_.begin(), buffer_.end(), 0);
		write_ptr_ = 0;
	}

	//[gnu::always_inline]
	void SetLFOFrequency(LFOIndex index, float frequency) { lfo_.SetFrequency(index, frequency * kLFOStride); }

	//[gnu::always_inline]
	void Advance() {
		--write_ptr_;
		if (write_ptr_ < 0) {
			write_ptr_ += buffer_.size();
		}
		StepLFO();
	}

	//[gnu::always_inline]
	float& at(size_t index) { return buffer_[(write_ptr_ + index) & mask]; }

	/// Reads the current value; stepping is Advance()'s job, so that a topology with several
	/// modulated delay lines still only advances the oscillators once per frame.
	//[gnu::always_inline]
	float LFO(LFOIndex lfo) {
		switch (lfo) {
		case LFO_1:
			return lfo_.values()[0];
		case LFO_2:
			return lfo_.values()[1];
		}
		__builtin_unreachable();
	}

	/// Four consecutive frames, processed together. Lane j of every vector is frame 3 - j: the write
	/// pointer walks downwards, so that is the order the four frames' slots lie in memory, and a line's
	/// four reads or writes at one index are a single vector load or store.
	///
	/// Only usable by a topology in which nothing written in one frame is read back fewer than four
	/// frames later, so that a line can do all four frames' reads before any of their writes. Every
	/// delay read has to be at least four samples long; one-pole filters stay serial.
	struct Quad {
		float* buffer;
		/// The last frame's write pointer.
		uint32_t write_ptr;
		uint32_t mask;
		/// Each frame's LFO values, the same ones LFO() would have returned after its Advance().
		float32x4_t lfo[2];
	};

	/// Four Advance()s in one go.
	[[gnu::always_inline]] Quad Advance4() {
		const float32x2_t before = lfo_.values().vec();
		float32x2_t after = before;

		// Frame k's write pointer is write_ptr_ - 1 - k, and the LFOs step on the frame where that
		// hits a multiple of 32: at most one frame in four.
		const uint32_t step = static_cast<uint32_t>(write_ptr_ - 1) & 31;
		if (step < 4) {
			lfo_.Next();
			after = lfo_.values().vec();
		}
		write_ptr_ = static_cast<int32_t>(static_cast<uint32_t>(write_ptr_ - 4) & mask);

		// Frames step..3 are lanes 0..3-step. Without a step both values are the same anyway.
		constexpr uint32x4_t kLane = {0, 1, 2, 3};
		const uint32x4_t stepped = vcleq_u32(kLane, vdupq_n_u32(3 - step));
		Quad q{buffer_.data(), static_cast<uint32_t>(write_ptr_), static_cast<uint32_t>(mask), {}};
		q.lfo[LFO_1] = vbslq_f32(stepped, vdupq_lane_f32(after, 0), vdupq_lane_f32(before, 0));
		q.lfo[LFO_2] = vbslq_f32(stepped, vdupq_lane_f32(after, 1), vdupq_lane_f32(before, 1));
		return q;
	}

private:
	//[gnu::always_inline]
	void StepLFO() {
		if ((write_ptr_ & 31) == 0) {
			lfo_.Next();
		}
	}

	int32_t write_ptr_ = 0;
	std::span<float> buffer_;
	DualCosineOscillator lfo_;

	size_t mask;

public: /******************** INNER CLASSES ****************/
	class Context {
	public:
		[[nodiscard]] constexpr float Get() const { return accumulator_; }
		constexpr void Set(float value) { accumulator_ = value; }
		constexpr void Add(float value) { accumulator_ += value; }
		constexpr void Multiply(float value) { accumulator_ *= value; }
		constexpr void Reset() { Set(0); }

		constexpr void Lp(float& state, float coefficient) { accumulator_ = OnePole(state, accumulator_, coefficient); }

		constexpr void Hp(float& state, float coefficient) {
			accumulator_ -= OnePole(state, accumulator_, coefficient);
		}

	private:
		float accumulator_ = 0.f;
	};

	struct DelayLine {
		DelayLine(size_t length) : length(length) {};

		// Store and Fetch
		//[gnu::always_inline]
		void Process(Context& c, size_t offset = 0) {
			this->at(offset) = c.Get();
			c.Set(this->at(length - offset));
		}

		//[gnu::always_inline]
		float& at(int32_t index) {
			if (index == TAIL) {
				index = length - 1;
			}
#if DELUGE_DSP_BOUNDS_CHECK
			// ConstructTopology reserves length + 1 slots per line, so indices 0..length are ours:
			// index `length` is the slot written `length` frames ago. Anything past that is the
			// next line's memory. Interpolated reads are the usual culprit, since they also
			// touch offset + 1.
			if (index < 0 || static_cast<size_t>(index) > length) {
				debug::out_of_bounds_access = true;
			}
#endif
			return engine_->at(this->base + index);
		}

		//[gnu::always_inline]
		float Read(int32_t offset) {
			// STATIC_ASSERT(d.base + d.length <= size, delay_memory_full);
			return this->at(offset);
		}

		//[gnu::always_inline]
		float Interpolate(float offset) {
			auto offset_integral = static_cast<int32_t>(offset);
			float offset_fractional = offset - static_cast<float>(offset_integral);
			const float a = this->at(offset_integral);
			const float b = this->at(offset_integral + 1);
			return dsp::Interpolate(a, b, offset_fractional);
		}

		/// writes to the delayline
		//[gnu::always_inline]
		void Write(int32_t offset, float value) { this->at(offset) = value; }

		// Four frames at a time, see Quad. Same slots and arithmetic as the scalar calls above.

		/// Slot of `index` in the block's last frame; the other frames' follow it upwards.
		[[gnu::always_inline]] uint32_t Slot(const Quad& q, int32_t index) const {
#if DELUGE_DSP_BOUNDS_CHECK
			if (index < 0 || static_cast<size_t>(index) > length) {
				debug::out_of_bounds_access = true;
			}
#endif
			return (q.write_ptr + base + index) & q.mask;
		}

		[[gnu::always_inline]] float32x4_t Load(const Quad& q, int32_t index) const {
			const uint32_t slot = Slot(q, index);
			if (slot <= q.mask - 3) [[likely]] {
				return vld1q_f32(q.buffer + slot);
			}
			return LoadWrapped(q.buffer, q.mask, slot);
		}

		[[gnu::always_inline]] void Store(const Quad& q, int32_t index, float32x4_t value) const {
			const uint32_t slot = Slot(q, index);
			if (slot <= q.mask - 3) [[likely]] {
				vst1q_f32(q.buffer + slot, value);
				return;
			}
			StoreWrapped(q.buffer, q.mask, slot, value);
		}

		/// Interpolated read at a per-frame offset.
		[[gnu::always_inline]] float32x4_t Interpolate(const Quad& q, float32x4_t offset) const {
			const int32x4_t integral = vcvtq_s32_f32(offset);
			const float32x4_t fractional = vsubq_f32(offset, vcvtq_f32_s32(integral));
			const int32_t last = vgetq_lane_s32(integral, 0);
			// The offsets come from an LFO that holds still for the block or steps once within it, so
			// they take at most two values, each over a run of neighbouring frames. Equal ends mean
			// they all agree, and both taps are plain vectors.
			if (last == vgetq_lane_s32(integral, 3)) [[likely]] {
				const float32x4_t a = Load(q, last);
				const float32x4_t b = Load(q, last + 1);
				return vmlaq_f32(a, vsubq_f32(b, a), fractional);
			}
			return InterpolateLanes(q.buffer, q.write_ptr, q.mask, integral, fractional);
		}

	private:
		// The rare paths, for four slots that straddle the end of the buffer or offsets that differ
		// between frames. They take the Quad apart so that calling them doesn't force it to the stack.

		[[gnu::noinline]] static float32x4_t LoadWrapped(const float* buffer, uint32_t mask, uint32_t slot) {
			float lanes[4];
			for (uint32_t j = 0; j < 4; ++j) {
				lanes[j] = buffer[(slot + j) & mask];
			}
			return vld1q_f32(lanes);
		}

		[[gnu::noinline]] static void StoreWrapped(float* buffer, uint32_t mask, uint32_t slot, float32x4_t value) {
			float lanes[4];
			vst1q_f32(lanes, value);
			for (uint32_t j = 0; j < 4; ++j) {
				buffer[(slot + j) & mask] = lanes[j];
			}
		}

		[[gnu::noinline]] float32x4_t InterpolateLanes(float* buffer, uint32_t write_ptr, uint32_t mask,
		                                               int32x4_t integral, float32x4_t fractional) const {
			const Quad q{buffer, write_ptr, mask, {}};
			int32_t offsets[4];
			float fractions[4];
			float lanes[4];
			vst1q_s32(offsets, integral);
			vst1q_f32(fractions, fractional);
			for (uint32_t j = 0; j < 4; ++j) {
				// Lane j is the frame j slots above the last one.
				const float a = q.buffer[(Slot(q, offsets[j]) + j) & q.mask];
				const float b = q.buffer[(Slot(q, offsets[j] + 1) + j) & q.mask];
				lanes[j] = dsp::Interpolate(a, b, fractions[j]);
			}
			return vld1q_f32(lanes);
		}

	public:
		const size_t length = 0;
		size_t base = 0;
		FxEngine* engine_ = nullptr;
	};

	struct AllPass : public DelayLine {
		AllPass(size_t length) : DelayLine(length) {};

		using DelayLine::Interpolate;

		//[gnu::always_inline]
		float Read(Context& c, int32_t offset, float scale) {
			const float r = DelayLine::Read(offset);
			c.Add(r * scale);
			return r;
		}

		// inline float Read(Context& c, float scale) { return Read(c, scale); }

		//[gnu::always_inline]
		void Write(Context& c, int32_t offset, float scale) {
			DelayLine::Write(offset, c.Get());
			c.Multiply(scale);
		}

		//[gnu::always_inline]
		void Write(Context& c, float scale) { Write(c, (int32_t)0, scale); }

		//[gnu::always_inline]
		void Write(Context& c, int32_t offset, float scale, float input) {
			Write(c, offset, scale);
			c.Add(input);
		}

		//[gnu::always_inline]
		void Write(Context& c, float scale, float input) { Write(c, 0, scale, input); }

		/** @brief Can be used in place of any AllPass::Read calls */
		//[gnu::always_inline]
		float Interpolate(Context& c, float offset, float scale) {
			// STATIC_ASSERT(d.base + d.length <= size, delay_memory_full);
			const float r = DelayLine::Interpolate(offset);
			c.Add(r * scale);
			return r;
		}

		//[gnu::always_inline]
		float Interpolate(Context& c, float offset, LFOIndex index, float amplitude, float scale) {
			offset += amplitude * this->engine_->LFO(index);
			return Interpolate(c, offset, scale);
		}

		//[gnu::always_inline]
		void ProcessInterpolate(Context& c, float offset, LFOIndex index, float amplitude, float scale) {
			const float read = this->Interpolate(c, offset, index, amplitude, scale);
			this->Write(c, 0, -scale, read);
		}

		// Simple Schroeder allpass section
		//
		//        ------[*-scale]-----,
		//       |    -----------     ∨
		//  ---+-'-->| Delayline |--,-+--->
		//     ∧      -----------   |
		//     '------[*scale]------
		//
		//[gnu::always_inline]
		void Process(Context& c, float scale) {
			const float head = c.Get();
			const float tail = this->at(TAIL);

			const float feedback = head + (tail * scale);
			this->at(0) = feedback; // feedback into delayline

			const float feedforward = (feedback * -scale) + tail;
			c.Set(feedforward); // output into pipeline

			// c.Add(tail * scale);
			// this->at(0) = c.Get();

			// c.Multiply(-scale);
			// c.Add(tail);
		}

		/// Process() for four frames: takes the inputs, returns the outputs. `scale` is {scale, -scale},
		/// kept in a register pair so the multiplies can take them as lanes.
		[[gnu::always_inline]] float32x4_t Process(const Quad& q, float32x4_t head, float32x2_t scale) const {
			const float32x4_t tail = this->Load(q, this->length - 1);
			const float32x4_t feedback = vmlaq_lane_f32(head, tail, scale, 0);
			this->Store(q, 0, feedback);
			return vmlaq_lane_f32(tail, feedback, scale, 1);
		}
	};

	static void ConstructTopology(FxEngine& e, std::initializer_list<DelayLine*> delays) {
		size_t base = 0;
		for (DelayLine* d : delays) {
			d->engine_ = &e;
			d->base = base;
			base += d->length + 1;
		}
	}
};

} // namespace deluge::dsp::reverb
