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

#include "dsp/delay/saturating_tap_delay.h"
#include "memory/memory_allocator_interface.h"
#include <bit>

int32_t SaturatingTapDelay::capacityLog2For(int32_t tap) {
	auto needed = static_cast<uint32_t>(tap + kCapacityMargin);
	int32_t log2 = 32 - std::countl_zero(needed - 1); // smallest power of two >= needed
	return std::clamp<int32_t>(log2, kMinCapacityLog2, kMaxCapacityLog2);
}

bool SaturatingTapDelay::activate(int32_t rate) {
	discard();
	int32_t tap = tapForRate(rate);
	int32_t capacityLog2 = capacityLog2For(tap);
	ring_ = static_cast<StereoSample*>(allocLowSpeed(sizeof(StereoSample) << capacityLog2));
	if (ring_ == nullptr) {
		return false;
	}
	mask_ = (1u << capacityLog2) - 1;
	writePos_ = 0;
	written_ = 0; // nothing in the ring is valid yet, so nothing needs clearing

	tap = std::min(tap, longestTap());
	glide_ = static_cast<float>(tap); // no glide from a fresh start
	tapOld_ = tap;
	tapNew_ = tap;
	segmentPos_ = 0;
	gainTarget_ = 0;
	samplesSinceRepeat_ = 0;
	loop_ = Loop{};
	return true;
}

void SaturatingTapDelay::discard() {
	if (previousRing_ != nullptr) {
		delugeDealloc(previousRing_);
		previousRing_ = nullptr;
	}
	if (ring_ != nullptr) {
		delugeDealloc(ring_);
		ring_ = nullptr;
	}
}

// Swaps in a bigger ring. The old one stays readable for the taps that reach back past what the new one holds, so
// lengthening the time glides through the existing echoes without copying or clearing anything.
bool SaturatingTapDelay::grow(int32_t capacityLog2) {
	auto* bigger = static_cast<StereoSample*>(allocLowSpeed(sizeof(StereoSample) << capacityLog2));
	if (bigger == nullptr) {
		return false;
	}
	if (previousRing_ != nullptr) {
		delugeDealloc(previousRing_); // grown twice in quick succession: the oldest history goes
	}
	previousRing_ = ring_;
	previousMask_ = mask_;
	previousValid_ = std::min(written_, mask_ + 1);
	ring_ = bigger;
	mask_ = (1u << capacityLog2) - 1;
	written_ = 0;
	return true;
}

void SaturatingTapDelay::startSegment(int32_t targetTap, q31_t feedback, int32_t referenceLog2) {
	if (targetTap > longestTap() && mask_ + 1 < (1u << kMaxCapacityLog2)) {
		grow(capacityLog2For(targetTap));
	}
	targetTap = std::min(targetTap, longestTap());

	// The tap for this segment comes from the glide as the previous segment left it
	tapOld_ = tapNew_;
	tapNew_ = std::clamp(static_cast<int32_t>(glide_), kMinTap, longestTap());
	auto target = static_cast<float>(targetTap);
	glide_ = target + (glide_ - target) * kGlideCoef;

	// Gain ramps linearly from the last segment's target to this one's over the segment. It carries the high-pass's
	// input gain and the scale from the working domain back to the buffer's level.
	constexpr q31_t kHighpassGainQ31 = static_cast<q31_t>(kHighpassGain * 2147483648.0 + 0.5);
	q31_t gainTarget = (multiply_32x32_rshift32(feedback, kHighpassGainQ31) << 1) >> (kUnitLog2 - referenceLog2);
	loop_.gain = gainTarget_;
	loop_.gainStep = (gainTarget - gainTarget_) >> kSegmentLengthLog2;
	gainTarget_ = gainTarget;
}

// A tap that may reach back before ring_'s first frame (just after activating or growing): those frames come from the
// previous ring if it still has them, otherwise they are silence.
StereoSample SaturatingTapDelay::readTap(int32_t tap, int32_t offset) const {
	uint32_t age = static_cast<uint32_t>(tap - offset); // frames back from this sample's write position
	uint32_t pos = writePos_ + offset - tap;
	if (age <= written_) {
		return ring_[pos & mask_];
	}
	if (previousRing_ != nullptr && age <= written_ + previousValid_) {
		return previousRing_[pos & previousMask_];
	}
	return StereoSample{0, 0};
}

int32_t SaturatingTapDelay::upcomingTapPeak(int32_t frames) const {
	if (ring_ == nullptr) {
		return 0;
	}
	int32_t peak = 0;
	for (int32_t i = 0; i < frames; i += 2) {
		StereoSample tap = readTap(tapNew_, i);
		// abs() without the overflow at INT32_MIN
		peak = std::max(peak, tap.l ^ (tap.l >> 31));
		peak = std::max(peak, tap.r ^ (tap.r >> 31));
	}
	return peak;
}

void SaturatingTapDelay::gatherTaps(StereoSample* taps, int32_t n) const {
	bool valid = static_cast<uint32_t>(std::max(tapOld_, tapNew_)) <= written_;
	// Warming up after activating: the whole chunk predates anything written
	auto newestAge = static_cast<uint32_t>(std::min(tapOld_, tapNew_) - (n - 1));
	if (newestAge > written_ + (previousRing_ != nullptr ? previousValid_ : 0)) {
		std::fill_n(taps, n, StereoSample{0, 0});
		return;
	}
	if (tapOld_ == tapNew_) {
		for (int32_t i = 0; i < n; i++) {
			taps[i] = readTap(tapNew_, i);
		}
		return;
	}
	// Crossfade from the old tap to the new one over the segment. Weights are q30 so that 1.0 fits.
	int32_t weightNew = segmentPos_ << (30 - kSegmentLengthLog2);
	constexpr int32_t kWeightStep = 1 << (30 - kSegmentLengthLog2);
	for (int32_t i = 0; i < n; i++, weightNew += kWeightStep) {
		StereoSample a;
		StereoSample b;
		if (valid) {
			a = ring_[(writePos_ + i - tapOld_) & mask_];
			b = ring_[(writePos_ + i - tapNew_) & mask_];
		}
		else {
			a = readTap(tapOld_, i);
			b = readTap(tapNew_, i);
		}
		int32_t weightOld = (1 << 30) - weightNew;
		taps[i].l = (multiply_32x32_rshift32(a.l, weightOld) + multiply_32x32_rshift32(b.l, weightNew)) << 2;
		taps[i].r = (multiply_32x32_rshift32(a.r, weightOld) + multiply_32x32_rshift32(b.r, weightNew)) << 2;
	}
}

// Splits n samples into runs where neither the write position nor (if the taps come straight from the ring) the read
// position wraps.
template <bool kPingPong, int32_t kReferenceLog2>
void SaturatingTapDelay::renderRuns(const StereoSample* tap, StereoSample* io, int32_t n, bool tapIsRing) {
	StereoSample* const ringEnd = ring_ + mask_ + 1;
	StereoSample* write = ring_ + (writePos_ & mask_);
	while (n > 0) {
		int32_t run = std::min<int32_t>(n, ringEnd - write);
		if (tapIsRing) {
			run = std::min<int32_t>(run, ringEnd - tap);
		}
		render<kPingPong, kReferenceLog2>(tap, io, write, run, loop_);
		tap += run;
		io += run;
		write += run;
		n -= run;
		if (write == ringEnd) {
			write = ring_;
		}
		if (tapIsRing && tap == ringEnd) {
			tap = ring_;
		}
	}
}

void SaturatingTapDelay::renderChunk(StereoSample* io, int32_t n, bool pingPong, bool bus) {
	const StereoSample* tap;
	bool tapIsRing;
	alignas(8) StereoSample taps[kSegmentLength];
	if (tapOld_ == tapNew_ && static_cast<uint32_t>(tapNew_) <= written_) {
		// Settled: read straight from the ring
		tap = ring_ + ((writePos_ - tapNew_) & mask_);
		tapIsRing = true;
	}
	else {
		gatherTaps(taps, n);
		tap = taps;
		tapIsRing = false;
	}

	if (bus) {
		if (pingPong) {
			renderRuns<true, kBusReferenceLog2>(tap, io, n, tapIsRing);
		}
		else {
			renderRuns<false, kBusReferenceLog2>(tap, io, n, tapIsRing);
		}
	}
	else {
		if (pingPong) {
			renderRuns<true, kSoundReferenceLog2>(tap, io, n, tapIsRing);
		}
		else {
			renderRuns<false, kSoundReferenceLog2>(tap, io, n, tapIsRing);
		}
	}

	writePos_ += n;
	written_ = std::min<uint32_t>(written_ + n, 0x40000000);
	if (previousRing_ != nullptr && written_ >= static_cast<uint32_t>(longestTap())) {
		// The new ring now holds everything a tap can reach
		delugeDealloc(previousRing_);
		previousRing_ = nullptr;
	}
}

bool SaturatingTapDelay::process(std::span<StereoSample> buffer, int32_t rate, q31_t feedback, int32_t referenceLog2,
                                 bool pingPong) {
	if (ring_ == nullptr) {
		return false;
	}
	bool bus = referenceLog2 > kSoundReferenceLog2;
	referenceLog2 = bus ? kBusReferenceLog2 : kSoundReferenceLog2;
	int32_t targetTap = tapForRate(rate);

	StereoSample* io = buffer.data();
	auto remaining = static_cast<int32_t>(buffer.size());
	while (remaining > 0) {
		if (segmentPos_ == 0) {
			startSegment(targetTap, feedback, referenceLog2);
		}
		int32_t n = std::min(remaining, kSegmentLength - segmentPos_);
		renderChunk(io, n, pingPong, bus);
		io += n;
		remaining -= n;
		segmentPos_ = (segmentPos_ + n) & (kSegmentLength - 1);
	}

	samplesSinceRepeat_ += buffer.size();
	if (samplesSinceRepeat_ >= static_cast<uint32_t>(tapNew_)) {
		samplesSinceRepeat_ = 0;
		return true;
	}
	return false;
}
