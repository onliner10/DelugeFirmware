#pragma once

#include "deluge/util/misc.h"
#include <argon.hpp>
#include <array>
#include <cstdint>

namespace deluge::dsp {
class [[gnu::hot]] SineOsc {
	static constexpr size_t kSineTableSizeMagnitude = 8;
	static std::array<int16_t, 512> sineWaveDiff;

public:
	static inline int32_t doFMNew(uint32_t carrierPhase, uint32_t phaseShift) {
		// return getSineNew((((*carrierPhase += carrierPhaseIncrement) >> 8) + phaseShift) & 16777215, 24);

		uint32_t phaseSmall = (carrierPhase >> 8) + phaseShift;
		int32_t strength2 = phaseSmall & 65535;

		uint32_t readOffset = (phaseSmall >> (24 - 8 - 1)) & 0b0111111110;

		uint32_t readValue = *(uint32_t*)&sineWaveDiff[readOffset];
		int32_t value = readValue << 16;
		int32_t diff = (int32_t)readValue >> 16;
		return value + diff * strength2;
	}

	// Test and verified to be identical to original function 11/22/24
	static inline Argon<int32_t> getSineVector(uint32_t* thisPhase, uint32_t phaseIncrement) {
		// create a vector containing a ramp of incrementing phases:
		// {phase + (phaseIncrement * 1), phase + (phaseIncrement * 2), phase + (phaseIncrement * 3), phase +
		// (phaseIncrement * 4)}
		auto phaseVector = Argon<uint32_t>::LoadCopy(thisPhase).MultiplyAdd(uint32x4_t{1, 2, 3, 4}, phaseIncrement);
		*thisPhase = *thisPhase + phaseIncrement * 4;

		return render(phaseVector);
	}

	// Test and verified to be identical to original function 11/22/24
	static inline Argon<int32_t> doFMVector(Argon<uint32_t> phaseVector, Argon<uint32_t> phaseShift) {
		return render(phaseVector + (phaseShift << 8));
	}

private:
	static inline Argon<int32_t> render(Argon<uint32_t> phase) {
		// interpolation fractional component
		ArgonHalf<int16_t> strength2 = phase.ShiftRightNarrow<32 - 16 - kSineTableSizeMagnitude + 1>()
		                                   .BitwiseAnd(std::numeric_limits<int16_t>::max())
		                                   .As<int16_t>();

		// Each {value, diff} pair in the interleaved table is one aligned 32-bit word, value in the low
		// half (the same layout doFMNew() reads). Gather the 4 words with plain 32-bit lane loads and split
		// them by narrowing. This replaces LoadGatherOffsetIndexInterleaved<2>, whose int16x4x2_t lane loads
		// GCC 14 wraps in 3 redundant vorr register shuffles per lane. Bit-exact with the old gather
		// (see tests/spec/sine_osc_spec.cpp).
		// LoadGatherOffsetIndex scales the offsets by the word size, so `indices` is the pair index (0-255).
		Argon<uint32_t> indices = phase >> (32 - kSineTableSizeMagnitude);
		Argon<uint32_t> pairs =
		    Argon<uint32_t>::LoadGatherOffsetIndex(reinterpret_cast<const uint32_t*>(sineWaveDiff.data()), indices);

		ArgonHalf<int16_t> sine = pairs.Narrow().As<int16_t>();               // low halves
		ArgonHalf<int16_t> diff = pairs.ShiftRightNarrow<16>().As<int16_t>(); // high halves

		// Essentially a MultiplyAddFixedPoint, but without the reduction back down to q31
		return sine.ShiftLeftLong<16>().MultiplyDoubleAddSaturateLong(diff, strength2);
	}

}; // namespace deluge::dsp
} // namespace deluge::dsp
