// The saturation curve types (dsp/saturation_curves.h) and their antialiased tables.
//
// - The generator is checked against the one table we didn't make: tanH2d. Running its formula on tanh must give
//   tanH2d back to within rounding, so the tables of the other curves are built the same way.
// - Each generated table's diagonal (current sample == previous sample) is its curve.
// - CLASSIC is tanH2d itself, so old sounds stay bit for bit as they were.
// - Every type runs the same on the block (NEON) and per-sample paths, at whole and fractional amounts.
// - Every type has the same small-signal gain, so switching type doesn't jump in level.
// - Type names save and load.

#include "dsp/saturation_curves.h"
#include "dsp/saturation_setting.h"
#include "util/lookuptables/lookuptables.h"

#include "cppspec.hpp"
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <vector>

namespace {
using deluge::dsp::kNumSaturationTypes;
using deluge::dsp::kSaturationStep;
using deluge::dsp::kSaturationTableColumns;
using deluge::dsp::kSaturationTableRows;
using deluge::dsp::SaturationSetting;
using deluge::dsp::SaturationType;

SaturationType typeAt(int32_t i) {
	return static_cast<SaturationType>(i);
}

// The generator's formula (first-order antiderivative average) on the CLASSIC curve, by direct integration
int32_t largestErrorAgainstTanH2d() {
	constexpr int32_t kFine = 2048;
	std::vector<double> antiderivative(kFine + 1);
	auto u = [](int32_t i) { return -1.0 + 2.0 * i / kFine; };
	double previous = deluge::dsp::saturationCurve(SaturationType::CLASSIC, (float)u(0));
	for (int32_t i = 1; i <= kFine; i++) {
		double v = std::tanh(4 * u(i)) / std::tanh(4.0);
		antiderivative[i] = antiderivative[i - 1] + 0.5 * (previous + v) * (2.0 / kFine);
		previous = v;
	}
	int32_t worst = 0;
	for (int32_t r = 0; r < kSaturationTableRows; r++) {
		for (int32_t c = 0; c < kSaturationTableColumns; c++) {
			int32_t xi = c * 16;
			int32_t yi = r * 32;
			double average = (xi == yi) ? std::tanh(4 * u(xi)) / std::tanh(4.0)
			                            : (antiderivative[xi] - antiderivative[yi]) / (u(xi) - u(yi));
			int32_t value = static_cast<int32_t>(std::lround(average * 32767));
			worst = std::max<int32_t>(worst, std::abs(value - tanH2d[r][c]));
		}
	}
	return worst;
}

// Largest difference, over every generated table, between its diagonal and its curve
int32_t largestDiagonalError() {
	int32_t worst = 0;
	for (int32_t t = 1; t < kNumSaturationTypes; t++) {
		int16_t const* table = deluge::dsp::saturationTable(typeAt(t));
		for (int32_t r = 0; r < kSaturationTableRows; r++) {
			float uu = -1.f + 2.f * r / (kSaturationTableRows - 1);
			int32_t expected = static_cast<int32_t>(std::lround(deluge::dsp::saturationCurve(typeAt(t), uu) * 32767));
			int32_t actual = table[r * kSaturationTableColumns + 2 * r];
			worst = std::max<int32_t>(worst, std::abs(actual - expected));
		}
	}
	return worst;
}

uint32_t lcg(uint32_t& x) {
	x = (x * 1664525u) + 1013904223u;
	return x;
}

size_t blockVsScalarMismatches() {
	size_t mismatches = 0;
	uint32_t seed = 99;
	std::vector<int32_t> input(1027);
	for (size_t i = 0; i < input.size(); i++) {
		input[i] = static_cast<int32_t>(lcg(seed)) >> (1 + (i / 64) % 12);
	}
	for (int32_t t = 0; t < kNumSaturationTypes; t++) {
		for (int32_t amount :
		     {kSaturationStep, 3 * kSaturationStep + 1234, 9 * kSaturationStep, 14 * kSaturationStep + 4000}) {
			for (auto [base, gainFrom] : {std::pair{5, 2}, std::pair{3, 3}}) {
				SaturationSetting const s = SaturationSetting::make(amount, base, gainFrom, typeAt(t));
				std::array<uint32_t, 2> wScalar{2147483648u, 2147483648u};
				std::array<uint32_t, 2> wBlock = wScalar;
				std::vector<int32_t> block = input;
				s.applyBlock<2>(block.data(), block.size(), wBlock.data());
				for (size_t i = 0; i < input.size(); i++) {
					mismatches += block[i] != s.apply(input[i], &wScalar[i % 2]);
				}
				mismatches += (wScalar != wBlock);
			}
		}
	}
	return mismatches;
}

// Small-signal gain of each type relative to CLASSIC, in dB, largest absolute value
double largestSmallSignalGainDifferenceDb() {
	std::vector<int32_t> sine(2048);
	for (size_t i = 0; i < sine.size(); i++) {
		// Low enough that even CRUNCH, whose y (2 - |y|) bends from the very start, is still close to linear
		sine[i] = static_cast<int32_t>(std::sin(2 * M_PI * i / 64.0) * 2.0e4);
	}
	auto rms = [&](SaturationType type) {
		SaturationSetting const s = SaturationSetting::make(2 * kSaturationStep, 5, 2, type);
		std::array<uint32_t, 2> w{2147483648u, 2147483648u};
		double sum = 0;
		for (size_t i = 0; i < sine.size(); i++) {
			double y = s.apply(sine[i], &w[0]);
			sum += (i >= 256) ? y * y : 0;
		}
		return std::sqrt(sum);
	};
	double classic = rms(SaturationType::CLASSIC);
	double worst = 0;
	for (int32_t t = 1; t < kNumSaturationTypes; t++) {
		worst = std::max(worst, std::abs(20 * std::log10(rms(typeAt(t)) / classic)));
	}
	return worst;
}
} // namespace

describe saturation_curves(
    "Saturation curve types", $ {
	    before_all([] { deluge::dsp::generateSaturationTables(); });

	    it(
	        "the generator's formula rebuilds tanH2d from tanh to within rounding",
	        _ { expect(largestErrorAgainstTanH2d()).to_be_less_than(2); });

	    it("each generated table's diagonal is its curve", _ { expect(largestDiagonalError()).to_be_less_than(2); });

	    it(
	        "CLASSIC is tanH2d itself",
	        _ { expect(deluge::dsp::saturationTable(SaturationType::CLASSIC) == &tanH2d[0][0]).to_be_true(); });

	    it(
	        "every type runs the same per block and per sample",
	        _ { expect(blockVsScalarMismatches()).to_equal(size_t{0}); });

	    it(
	        "every type has the same small-signal gain",
	        _ { expect(largestSmallSignalGainDifferenceDb()).to_be_less_than(0.25); });

	    it(
	        "type names save and load", _ {
		        bool ok = true;
		        for (int32_t t = 0; t < kNumSaturationTypes; t++) {
			        ok = ok
			             && deluge::dsp::stringToSaturationType(deluge::dsp::saturationTypeToString(typeAt(t)))
			                    == typeAt(t);
		        }
		        expect(ok).to_be_true();
		        expect(deluge::dsp::stringToSaturationType("nonsense") == SaturationType::CLASSIC).to_be_true();
	        });
    });

CPPSPEC_SPEC(saturation_curves)
