// Bit-exactness check for the NEON wavefolder, dsp::foldBufferPolyApproximation().
//
// It used to run the scalar loop that is now foldBufferPolyApproximationScalar() (still used for the last 1-3
// samples). This spec keeps a verbatim copy of that loop and checks the new function gives identical samples:
//  - for every value x the polynomial can see (x = ssat24(.) << 8 has 2^24 values), via fold_level INT32_MAX,
//  - for edge and pseudo-random inputs at edge and pseudo-random levels,
//  - for every buffer length 1..17 and some longer ones, so the scalar tail runs at every offset.
//
// On the host the NEON intrinsics run through SIMDe, so this checks the algorithm, not the ARM code generation.

#include "dsp/util.hpp"

#include "cppspec.hpp"
#include <cstdint>
#include <limits>
#include <vector>

namespace {
using deluge::dsp::foldBufferPolyApproximation;

constexpr int32_t kMin = std::numeric_limits<int32_t>::min();
constexpr int32_t kMax = std::numeric_limits<int32_t>::max();

uint32_t lcg(uint32_t& x) {
	x = (x * 1664525u) + 1013904223u;
	return x;
}

// Verbatim copy of the old foldBufferPolyApproximation()
void foldOld(q31_t* startSample, q31_t const* endSample, q31_t level) {
	q31_t* current_sample = startSample;
	q31_t fold_level = add_saturate(level, deluge::dsp::FOLD_MIN);

	do {
		q31_t c = *current_sample;

		q31_t x = lshiftAndSaturateUnknown(multiply_32x32_rshift32(fold_level, c), 8);

		// volume compensation
		*current_sample = deluge::dsp::polynomialOscillatorApproximation(x) >> 7;

		current_sample += 1;
	} while (current_sample < endSample);
}

struct Tally {
	size_t samples = 0;
	size_t mismatches = 0;
};

void compare(Tally& tally, std::vector<int32_t> const& input, int32_t level) {
	if (input.empty()) {
		return;
	}
	std::vector<int32_t> expected = input;
	std::vector<int32_t> actual = input;
	foldOld(expected.data(), expected.data() + expected.size(), level);
	foldBufferPolyApproximation(actual.data(), actual.data() + actual.size(), level);
	tally.samples += input.size();
	for (size_t i = 0; i < input.size(); i++) {
		if (actual[i] != expected[i]) {
			tally.mismatches++;
		}
	}
}

// fold_level saturates to INT32_MAX, and multiply_32x32_rshift32(INT32_MAX, c) takes every value in
// [-2^23 - 1, 2^23] as c runs over [-2^24 - 4, 2^24 + 4], so every x reaches the polynomial, saturated ones too
Tally checkEveryX() {
	Tally tally;
	constexpr int32_t block_size = 1 << 16;
	std::vector<int32_t> input;
	input.reserve(block_size);
	for (int64_t c = -(int64_t{1} << 24) - 4; c <= (int64_t{1} << 24) + 4; c++) {
		input.push_back(static_cast<int32_t>(c));
		if (input.size() == block_size) {
			compare(tally, input, kMax);
			input.clear();
		}
	}
	input.push_back(kMin);
	input.push_back(kMax);
	compare(tally, input, kMax);
	return tally;
}

std::vector<int32_t> edgeLevels() {
	return {1,
	        2,
	        0x00FFFFFF,
	        0x10000000,
	        0x3FFFFFFF,
	        0x40000000,
	        kMax - deluge::dsp::FOLD_MIN - 1,
	        kMax - deluge::dsp::FOLD_MIN,
	        kMax - deluge::dsp::FOLD_MIN + 1,
	        kMax - 1,
	        kMax};
}

// Inputs at the int32 limits and at both sides of the 24-bit saturation of multiply_32x32_rshift32(level, c)
std::vector<int32_t> edgeInputs(int32_t level) {
	std::vector<int32_t> v = {0, 1, -1, 2, -2, kMin, kMin + 1, kMax, kMax - 1};
	int64_t const folded_level = add_saturate(level, deluge::dsp::FOLD_MIN);
	for (int64_t t : {int64_t{1} << 23, -(int64_t{1} << 23), int64_t{1} << 22, -(int64_t{1} << 22)}) {
		int64_t const c = (t << 32) / folded_level;
		for (int64_t d = -3; d <= 3; d++) {
			int64_t const x = c + d;
			if (x >= kMin && x <= kMax) {
				v.push_back(static_cast<int32_t>(x));
			}
		}
	}
	return v;
}

Tally checkLevelsAndLengths() {
	Tally tally;
	uint32_t seed = 4242;
	std::vector<int32_t> levels = edgeLevels();
	for (size_t i = 0; i < 2000; i++) {
		levels.push_back(static_cast<int32_t>(lcg(seed) >> 1) | 1);
	}
	std::vector<size_t> lengths;
	for (size_t n = 1; n <= 17; n++) {
		lengths.push_back(n);
	}
	for (size_t n : {63, 64, 65, 127, 128, 129, 256}) {
		lengths.push_back(n);
	}
	for (int32_t level : levels) {
		// Edge inputs, in a buffer whose length is not a multiple of 4
		std::vector<int32_t> edges = edgeInputs(level);
		std::vector<int32_t> both = edges;
		both.insert(both.end(), edges.rbegin(), edges.rend());
		both.push_back(kMin);
		compare(tally, both, level);

		// Pseudo-random inputs of every magnitude, at every length
		for (size_t length : lengths) {
			std::vector<int32_t> input(length);
			for (auto& sample : input) {
				sample = static_cast<int32_t>(lcg(seed)) >> (lcg(seed) >> 27);
			}
			compare(tally, input, level);
		}
	}
	return tally;
}
} // namespace

describe wavefold(
    "NEON wavefolder", $ {
	    it(
	        "matches the old scalar fold for every value the polynomial can see", _ {
		        Tally const tally = checkEveryX();
		        expect(tally.samples).to_be_greater_than(size_t{33'000'000});
		        expect(tally.mismatches).to_equal(size_t{0});
	        });

	    it(
	        "matches the old scalar fold for edge and random inputs, levels and every length 1-17", _ {
		        Tally const tally = checkLevelsAndLengths();
		        expect(tally.samples).to_be_greater_than(size_t{1'000'000});
		        expect(tally.mismatches).to_equal(size_t{0});
	        });
    });

CPPSPEC_SPEC(wavefold)
