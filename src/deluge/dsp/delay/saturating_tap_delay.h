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

#include "dsp/stereo_sample.h"
#include "util/fixedpoint.h"
#include <algorithm>
#include <cstdint>
#include <span>

/// The "Warm" delay type: a stereo delay line with whole-sample taps and a saturating, filtered feedback loop.
///
/// Signal flow, per channel (normalised so that the saturator's rail is 1.0):
///
///   tap  = ring[w - d], crossfaded linearly from the previous tap over each 32-sample segment while d moves
///   u    = clip(tap, -1, +1)
///   s    = u * (2 - |u|)                               soft saturator: gain 2 at 0, slope 0 at the rails
///   l[n] = pL * l[n-1] + (1 - pL) * s[n]                one-pole low-pass, DC gain 1
///   y[n] = pH * y[n-1] + (1 + pH) / 2 * (l[n] - l[n-1]) one-pole/one-zero high-pass, gain 1 at Nyquist
///   out += F * y
///   ring[w] = input + F * y          (ping-pong: the input goes to the right ring as (L + R) / 2, like the other
///                                     delay types, and the feedback crosses over)
///
/// F is the delay amount: the loop gain is 2F for small signals, so repeats sustain at F = 0.5 and grow above it until
/// the saturator holds them. The tap d glides to the target with a one-pole smoother (time constant about 0.17 s)
/// updated once per segment; there is no fractional read, the crossfade between whole-sample taps does the glide.
class SaturatingTapDelay {
public:
	/// Control segment: the tap, the glide and the gain ramp are updated once per segment, whatever the render size
	static constexpr int32_t kSegmentLength = 32;
	static constexpr int32_t kSegmentLengthLog2 = 5;

	/// Shortest tap. A segment's taps are gathered before its writes, so the tap must cover a segment.
	static constexpr int32_t kMinTap = kSegmentLength;
	/// Longest tap: 8 s
	static constexpr int32_t kMaxTap = 8 * 44100;

	/// Ring capacity range, in stereo frames (power of two): 32 KiB .. 4 MiB
	static constexpr int32_t kMinCapacityLog2 = 12;
	static constexpr int32_t kMaxCapacityLog2 = 19;
	/// Frames kept free between the oldest tap and the write position, so a segment's writes can't overwrite a frame
	/// that one of its later taps reads
	static constexpr int32_t kCapacityMargin = 2 * kSegmentLength;

	// Loop filter, fixed (the Deluge has no per-delay filter parameters). Poles are exp(-2 pi fc / 44100).
	/// Low-pass at 10 kHz: each repeat gets a little darker
	static constexpr double kLowpassPole = 0.24056653544129622;
	/// High-pass at 15 Hz, so nothing builds up at DC or subsonic frequencies when the loop runs hot
	static constexpr double kHighpassPole = 0.9978651441935537;
	/// The high-pass's (1 + pH) / 2 input gain is folded into the output gain (see render())
	static constexpr double kHighpassGain = (1.0 + kHighpassPole) * 0.5;
	/// pL * 2^32 and (1 - pH) * 2^32, for the multiply-subtract forms in shapeAndFilter()
	static constexpr q31_t kLowpassPoleQ32 = static_cast<q31_t>(kLowpassPole * 4294967296.0 + 0.5);
	static constexpr q31_t kHighpassLeakQ32 = static_cast<q31_t>((1.0 - kHighpassPole) * 4294967296.0 + 0.5);

	/// Per-segment glide coefficient: exp(-32 / (0.1706 s * 44100))
	static constexpr float kGlideCoef = 0.99575618f;

	/// The working domain: the saturator's rail (1.0) is 2^29, leaving 2 bits for the high-pass overshoot
	static constexpr int32_t kUnitLog2 = 29;

	SaturatingTapDelay() = default;
	SaturatingTapDelay(const SaturatingTapDelay&) = delete;
	SaturatingTapDelay& operator=(const SaturatingTapDelay&) = delete;
	~SaturatingTapDelay() { discard(); }

	[[nodiscard]] constexpr bool isActive() const { return ring_ != nullptr; }

	/// Allocates a ring big enough for this rate and resets everything. Returns false if out of memory.
	bool activate(int32_t rate);
	void discard();

	/// Tap length in samples for a delay rate. Same law as the other delay types: 2^38 / rate.
	static int32_t tapForRate(int32_t rate) {
		if (rate <= 0) {
			return kMaxTap;
		}
		float tap = 274877906944.0f / static_cast<float>(rate);
		return std::clamp<int32_t>(static_cast<int32_t>(std::min(tap, static_cast<float>(kMaxTap))), kMinTap, kMaxTap);
	}

	/// Mixes the delay into `buffer` in place.
	/// feedback: the delay amount, q31 with 2^31 = 1.0 (F above; the loop gain is twice this).
	/// referenceLog2: the level of the saturator's rail in `buffer` is 2^referenceLog2: kSoundReferenceLog2, or
	/// kBusReferenceLog2 for anything higher.
	/// Returns true when another tap length's worth of samples has gone by, i.e. once per repeat.
	bool process(std::span<StereoSample> buffer, int32_t rate, q31_t feedback, int32_t referenceLog2, bool pingPong);

	/// The loudest of the taps the next `frames` will read, at the buffer's level, looking at every other frame. Each
	/// repeat comes back at about this times the loop gain, so Safe Delay Feedback can level the loop from it.
	[[nodiscard]] int32_t upcomingTapPeak(int32_t frames) const;

	// Inner loop, public for testing ----------------------------------------------------------------------------

	/// Levels of the saturator's rail that render() is built for (see Delay::State)
	static constexpr int32_t kSoundReferenceLog2 = 27;
	static constexpr int32_t kBusReferenceLog2 = 29;

	struct Loop {
		q31_t lowpassL;
		q31_t lowpassR;
		q31_t highpassL; ///< high-pass state z = y / ((1 + pH) / 2)
		q31_t highpassR;
		q31_t gain;     ///< F * (1 + pH) / 2, in the buffer's level: q31 >> (kUnitLog2 - referenceLog2)
		q31_t gainStep; ///< per sample
	};

	/// One channel: clip, saturate, low-pass, high-pass. Returns z = y / ((1 + pH) / 2) in the working domain
	/// (2^kUnitLog2 = 1.0).
	template <int32_t kReferenceLog2>
	[[gnu::always_inline]] static q31_t shapeAndFilter(q31_t tap, q31_t& lowpass, q31_t& highpass) {
		static_assert(kReferenceLog2 <= kUnitLog2);
		constexpr int32_t kShift = kUnitLog2 - kReferenceLog2; // buffer level -> working domain
		// t = clip(tap, +-rail); u = t * 2^kShift is in the working domain
		q31_t t = signed_saturate<kReferenceLog2 + 1>(tap);
		q31_t magnitude = t ^ (t >> 31); // |t|, one LSB short for negatives
		// u * (2 - |u|) = 2u - u|u| / 2^kUnitLog2, with u|u| taken from t|t| (scaled by 2^(2 kShift))
		q31_t shaped = (t << (kShift + 1)) - (multiply_32x32_rshift32(t, magnitude) << (32 - kUnitLog2 + 2 * kShift));
		// l[n] = s - pL * (s - l[n-1])
		q31_t lowpassNew = multiply_subtract_32x32_rshift32_rounded(shaped, shaped - lowpass, kLowpassPoleQ32);
		q31_t step = lowpassNew - lowpass; // l[n] - l[n-1]
		lowpass = lowpassNew;
		// z[n] = pH * z[n-1] + (l[n] - l[n-1]) = z[n-1] + step - (1 - pH) * z[n-1]
		highpass = multiply_subtract_32x32_rshift32_rounded(highpass + step, highpass, kHighpassLeakQ32);
		return highpass;
	}

	/// a + 2b, saturating (QDADD)
	[[gnu::always_inline]] static q31_t addDoubled(q31_t a, q31_t b) {
#if defined(__arm__)
		q31_t result;
		asm("qdadd %0, %1, %2" : "=r"(result) : "r"(a), "r"(b));
		return result;
#else
		return add_saturate(a, add_saturate(b, b));
#endif
	}

	/// Reads n taps, mixes the delay into io and writes the ring. tap, io and ring must not wrap within n.
	template <bool kPingPong, int32_t kReferenceLog2>
	[[gnu::always_inline]] static void render(const StereoSample* tap, StereoSample* io, StereoSample* ring, int32_t n,
	                                          Loop& loop) {
		q31_t lowpassL = loop.lowpassL;
		q31_t lowpassR = loop.lowpassR;
		q31_t highpassL = loop.highpassL;
		q31_t highpassR = loop.highpassR;
		q31_t gain = loop.gain;
		const q31_t gainStep = loop.gainStep;
		for (const StereoSample* const end = tap + n; tap != end; tap++, io++, ring++) {
			q31_t zL = shapeAndFilter<kReferenceLog2>(tap->l, lowpassL, highpassL);
			q31_t zR = shapeAndFilter<kReferenceLog2>(tap->r, lowpassR, highpassR);
			// F * y / 2, back in the buffer's level
			q31_t halfWetL = multiply_32x32_rshift32(zL, gain);
			q31_t halfWetR = multiply_32x32_rshift32(zR, gain);
			gain += gainStep;

			StereoSample in = *io;
			StereoSample out{addDoubled(in.l, halfWetL), addDoubled(in.r, halfWetR)};
			*io = out;
			if constexpr (kPingPong) {
				ring->l = halfWetR << 1;
				ring->r = addDoubled((in.l >> 1) + (in.r >> 1), halfWetL); // mono input, (L + R) / 2
			}
			else {
				*ring = out; // input + F * y, as heard
			}
		}
		loop.lowpassL = lowpassL;
		loop.lowpassR = lowpassR;
		loop.highpassL = highpassL;
		loop.highpassR = highpassR;
		loop.gain = gain;
	}

private:
	void startSegment(int32_t targetTap, q31_t feedback, int32_t referenceLog2);
	void renderChunk(StereoSample* io, int32_t n, bool pingPong, bool bus);
	void gatherTaps(StereoSample* taps, int32_t n) const;
	[[nodiscard]] StereoSample readTap(int32_t tap, int32_t offset) const;
	bool grow(int32_t capacityLog2);
	[[nodiscard]] int32_t longestTap() const { return static_cast<int32_t>(mask_) + 1 - kCapacityMargin; }
	template <bool kPingPong, int32_t kReferenceLog2>
	void renderRuns(const StereoSample* tap, StereoSample* io, int32_t n, bool tapIsRing);

	static int32_t capacityLog2For(int32_t tap);

	// Ring being written. Frames are addressed by a free-running write counter, masked.
	StereoSample* ring_ = nullptr;
	uint32_t mask_ = 0;
	uint32_t writePos_ = 0;
	/// Frames written to ring_ since it was allocated (saturates). Taps further back than this read zero, or come
	/// from the previous ring.
	uint32_t written_ = 0;

	// After the ring has grown, the previous one is kept (read-only) until the new one has filled up
	StereoSample* previousRing_ = nullptr;
	uint32_t previousMask_ = 0;
	/// Frames in previousRing_ ending just before the first frame of ring_
	uint32_t previousValid_ = 0;

	float glide_ = 0; ///< smoothed tap, in samples
	int32_t tapOld_ = kMinTap;
	int32_t tapNew_ = kMinTap;
	int32_t segmentPos_ = 0;
	q31_t gainTarget_ = 0;
	uint32_t samplesSinceRepeat_ = 0;

	Loop loop_{};
};
