// The continuous saturation amount (dsp/saturation_setting.h).
//
// - Whole steps 1-15 must sound exactly as the old 0-15 setting did, for voices (tanh 5 + n, gain from step 2) and
//   for clips (3 + n, gain from step 3), per sample and through the block path.
// - The block path (NEON pre/post gain + saturateTanHAntialiased) must match the per-sample path bit for bit, for
//   any amount.
// - In between whole steps the curve must move smoothly: no jump at a whole step, and more amount never means less
//   drive.
//
// On the host the NEON intrinsics run through SIMDe, so this checks the algorithm, not the ARM code generation.

#include "dsp/saturation_setting.h"

#include "cppspec.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <vector>

namespace {
using deluge::dsp::kSaturationMax;
using deluge::dsp::kSaturationStep;
using deluge::dsp::SaturationSetting;

uint32_t lcg(uint32_t& x) {
	x = (x * 1664525u) + 1013904223u;
	return x;
}

// A mix of quiet and very loud samples, as a voice or a clip bus can carry
std::vector<int32_t> testSignal(size_t n, uint32_t seed) {
	std::vector<int32_t> v(n);
	for (size_t i = 0; i < n; i++) {
		int32_t r = static_cast<int32_t>(lcg(seed));
		v[i] = r >> (1 + (i / 64) % 12);
	}
	return v;
}

// The old code: getTanHAntialiased(x, base + n) << max(n - gainFrom, 0)
int32_t oldSaturate(int32_t x, uint32_t* working, int32_t n, int32_t base, int32_t gainFrom) {
	return getTanHAntialiased(x, working, base + n) << std::max<int32_t>(n - gainFrom, 0);
}

size_t wholeStepMismatches(int32_t base, int32_t gainFrom) {
	size_t mismatches = 0;
	std::vector<int32_t> const input = testSignal(4096, 12345);
	for (int32_t n = 1; n <= 15; n++) {
		SaturationSetting const s = SaturationSetting::make(n * kSaturationStep, base, gainFrom);
		std::array<uint32_t, 2> wOld{2147483648u, 2147483648u};
		std::array<uint32_t, 2> wNew = wOld;
		std::array<uint32_t, 2> wBlock = wOld;
		std::vector<int32_t> block = input;
		s.applyBlock<2>(block.data(), block.size(), wBlock.data());
		for (size_t i = 0; i < input.size(); i++) {
			int32_t expected = oldSaturate(input[i], &wOld[i % 2], n, base, gainFrom);
			int32_t scalar = s.apply(input[i], &wNew[i % 2]);
			mismatches += (scalar != expected) + (block[i] != expected);
		}
	}
	return mismatches;
}

size_t blockVsScalarMismatches(int32_t base, int32_t gainFrom) {
	size_t mismatches = 0;
	std::vector<int32_t> const input = testSignal(1027, 777); // not a multiple of 4: covers the scalar tail
	for (int32_t amount = 1; amount <= kSaturationMax; amount += 523) {
		SaturationSetting const s = SaturationSetting::make(amount, base, gainFrom);
		std::array<uint32_t, 2> wScalar{2147483648u, 2147483648u};
		std::array<uint32_t, 2> wBlock = wScalar;
		std::vector<int32_t> block = input;
		s.applyBlock<2>(block.data(), block.size(), wBlock.data());
		for (size_t i = 0; i < input.size(); i++) {
			mismatches += block[i] != s.apply(input[i], &wScalar[i % 2]);
		}
		mismatches += (wScalar != wBlock);
	}
	return mismatches;
}

// RMS of the output for a fixed loud sine, relative to its input: the amount of drive shows as both a gain and how
// hard the peaks are squashed. Returns, for amounts on a fine grid, the largest step-to-step jump in dB.
struct Smoothness {
	double largestJumpDb = 0;
	bool crestNeverRises = true;
};
Smoothness smoothness(int32_t base, int32_t gainFrom) {
	Smoothness result;
	std::vector<int32_t> sine(2048);
	for (size_t i = 0; i < sine.size(); i++) {
		sine[i] = static_cast<int32_t>(std::sin(2 * M_PI * i / 128.0) * 6.0e7);
	}
	double lastRmsDb = 0;
	double lastCrest = 1e9;
	for (int32_t amount = kSaturationStep / 2; amount <= kSaturationMax; amount += 64) {
		SaturationSetting const s = SaturationSetting::make(amount, base, gainFrom);
		std::array<uint32_t, 2> w{2147483648u, 2147483648u};
		double sum = 0;
		double peak = 0;
		for (size_t i = 0; i < sine.size(); i++) {
			double y = s.apply(sine[i], &w[0]);
			if (i >= 256) {
				sum += y * y;
				peak = std::max(peak, std::abs(y));
			}
		}
		double rms = std::sqrt(sum / (sine.size() - 256));
		double rmsDb = 20 * std::log10(rms);
		double crest = peak / rms;
		if (amount > kSaturationStep / 2) {
			result.largestJumpDb = std::max(result.largestJumpDb, std::abs(rmsDb - lastRmsDb));
			// Harder drive squashes the peaks: the crest factor heads from a sine's 1.414 down towards 1
			if (crest > lastCrest + 1e-3) {
				result.crestNeverRises = false;
			}
		}
		lastRmsDb = rmsDb;
		lastCrest = crest;
	}
	return result;
}
} // namespace

describe saturation_setting(
    "Continuous saturation amount", $ {
	    it(
	        "whole steps 1-15 match the old setting bit for bit, voices and clips, per sample and per block", _ {
		        expect(wholeStepMismatches(5, 2)).to_equal(size_t{0});
		        expect(wholeStepMismatches(3, 3)).to_equal(size_t{0});
	        });

	    it(
	        "the block path matches the per-sample path for any amount", _ {
		        expect(blockVsScalarMismatches(5, 2)).to_equal(size_t{0});
		        expect(blockVsScalarMismatches(3, 3)).to_equal(size_t{0});
	        });

	    it(
	        "moves smoothly between whole steps, and more amount never squashes less", _ {
		        Smoothness const voices = smoothness(5, 2);
		        Smoothness const clips = smoothness(3, 3);
		        // 64 units is 1/64 of a 6dB step: anything near a whole-step jump would show as ~0.1dB or more
		        expect(voices.largestJumpDb).to_be_less_than(0.2);
		        expect(clips.largestJumpDb).to_be_less_than(0.2);
		        expect(voices.crestNeverRises).to_be_true();
		        expect(clips.crestNeverRises).to_be_true();
	        });

	    it(
	        "maps the 0-127 menu onto the amount and back, with 0 off and 127 the old maximum", _ {
		        using deluge::dsp::saturationFromMenu;
		        using deluge::dsp::saturationToMenu;
		        bool roundTrips = true;
		        for (int32_t v = 0; v <= 127; v++) {
			        roundTrips = roundTrips && (saturationToMenu(saturationFromMenu(v)) == v);
		        }
		        expect(roundTrips).to_be_true();
		        expect((int32_t)saturationFromMenu(0)).to_equal(0);
		        expect((int32_t)saturationFromMenu(127)).to_equal((int32_t)kSaturationMax);
	        });
    });

CPPSPEC_SPEC(saturation_setting)
