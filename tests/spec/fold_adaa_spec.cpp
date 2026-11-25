// Checks the antialiased wavefolder (foldBufferPolyApproximationAntialiased) against the plain one
// (foldBufferPolyApproximation), which it has to match in shape and level.

#include "dsp/util.hpp"

#include "cppspec.hpp"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <numbers>
#include <vector>

namespace {
using deluge::dsp::FOLD_ADAA_LIMIT;
using deluge::dsp::foldBufferPolyApproximation;
using deluge::dsp::foldBufferPolyApproximationAntialiased;

// With the fold level at full, the polynomial input is x = 128 * c (as fractions of one), so c = x / 128
constexpr q31_t kFullLevel = INT32_MAX;
constexpr double kOne = 2147483648.0;
// The fold's output for P = 1, after the >> 7 volume compensation
constexpr double kOutOne = kOne / 128;

q31_t inputFor(double x) {
	return static_cast<q31_t>(std::lround(x * kOne / 128));
}

// The polynomial input the fold computes from a sample, at full level
q31_t polynomialInput(q31_t sample) {
	return lshiftAndSaturateUnknown(multiply_32x32_rshift32(add_saturate(kFullLevel, deluge::dsp::FOLD_MIN), sample),
	                                8);
}

std::vector<q31_t> plainFold(std::vector<q31_t> in, q31_t level = kFullLevel) {
	foldBufferPolyApproximation(in.data(), in.data() + in.size(), level);
	return in;
}

std::vector<q31_t> antialiasedFold(std::vector<q31_t> in, q31_t level = kFullLevel, q31_t firstX = 0) {
	q31_t last_x = firstX;
	foldBufferPolyApproximationAntialiased(in.data(), in.data() + in.size(), level, 1, last_x);
	return in;
}

// Largest difference between two outputs, as a fraction of the fold's full output
double maxError(std::vector<q31_t> const& a, std::vector<q31_t> const& b) {
	double worst = 0;
	for (size_t i = 0; i < a.size(); i++) {
		worst = std::max(worst, std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])) / kOutOne);
	}
	return worst;
}

double rms(std::vector<q31_t> const& v) {
	double sum = 0;
	for (q31_t s : v) {
		sum += static_cast<double>(s) * static_cast<double>(s);
	}
	return std::sqrt(sum / static_cast<double>(v.size()));
}

// A band-limited saw, peaking at xPeak at the polynomial input
std::vector<q31_t> saw(double frequency, double xPeak, size_t length) {
	std::vector<double> s(length, 0.0);
	auto const harmonics = static_cast<int32_t>(22050 / frequency);
	for (size_t n = 0; n < length; n++) {
		double const phase = 2 * std::numbers::pi * frequency * static_cast<double>(n) / 44100;
		for (int32_t k = 1; k <= harmonics; k++) {
			s[n] += ((k % 2 == 1) ? 1.0 : -1.0) * std::sin(k * phase) / k;
		}
	}
	double const peak = *std::ranges::max_element(s, {}, [](double v) { return std::abs(v); });
	std::vector<q31_t> out(length);
	for (size_t n = 0; n < length; n++) {
		out[n] = inputFor(xPeak * std::abs(s[n]) / std::abs(peak) * (s[n] < 0 ? -1 : 1));
	}
	return out;
}

// Each constant input from -limit to +limit, as a run of samples, against the plain fold of that input
double constantInputError() {
	double worst = 0;
	for (int32_t i = -1000; i <= 1000; i++) {
		std::vector<q31_t> const in(4, inputFor(0.7099 * i / 1000));
		worst = std::max(worst, maxError(antialiasedFold(in, kFullLevel, polynomialInput(in[0])), plainFold(in)));
	}
	return worst;
}

// A slow sine across the whole range, against the plain fold of the midpoint between each pair of inputs
double slowInputError() {
	std::vector<q31_t> in(8192);
	for (size_t n = 0; n < in.size(); n++) {
		in[n] = inputFor(0.7099 * std::sin(2 * std::numbers::pi * static_cast<double>(n) / 8192));
	}
	std::vector<q31_t> midpoints(in.size());
	midpoints[0] = in[0] / 2;
	for (size_t n = 1; n < in.size(); n++) {
		midpoints[n] = (in[n - 1] / 2) + (in[n] / 2);
	}
	return maxError(antialiasedFold(in), plainFold(midpoints));
}

// Inputs either side of FOLD_ADAA_LIMIT and up to saturation
struct EdgeResult {
	size_t samplesPastLimit = 0;
	size_t pastLimitMismatches = 0;
	double largestOutput = 0;
};

EdgeResult checkEdges() {
	std::vector<q31_t> in;
	for (double x : {0.70, 0.7099, 0.7101, 0.72, 0.75, 0.9, 1.0, 2.0, 50.0}) {
		for (double sign : {1.0, -1.0}) {
			in.push_back(inputFor(0.3 * sign));
			in.push_back(inputFor(x * sign));
			in.push_back(inputFor(x * sign));
			in.push_back(inputFor(-x * sign));
		}
	}
	in.push_back(INT32_MAX);
	in.push_back(INT32_MIN);
	in.push_back(INT32_MIN);
	in.push_back(0);

	std::vector<q31_t> const plain = plainFold(in);
	std::vector<q31_t> const antialiased = antialiasedFold(in);
	EdgeResult result;
	q31_t last_x = 0;
	for (size_t n = 0; n < in.size(); n++) {
		q31_t const x = polynomialInput(in[n]);
		if (std::abs(static_cast<int64_t>(x)) > FOLD_ADAA_LIMIT
		    || std::abs(static_cast<int64_t>(last_x)) > FOLD_ADAA_LIMIT) {
			result.samplesPastLimit++;
			result.pastLimitMismatches += (antialiased[n] != plain[n]) ? 1 : 0;
		}
		result.largestOutput = std::max(result.largestOutput, std::abs(static_cast<double>(antialiased[n])) / kOutOne);
		last_x = x;
	}
	return result;
}

// Two channels interleaved with stride 2 give the same as each channel on its own
bool stereoMatchesMono() {
	std::vector<q31_t> const left = saw(440, 0.6, 512);
	std::vector<q31_t> const right = saw(660, 0.4, 512);
	std::vector<q31_t> interleaved(1024);
	for (size_t n = 0; n < 512; n++) {
		interleaved[2 * n] = left[n];
		interleaved[(2 * n) + 1] = right[n];
	}
	std::array<q31_t, 2> last_x{};
	q31_t const level = ONE_Q31 / 3;
	foldBufferPolyApproximationAntialiased(interleaved.data(), interleaved.data() + 1024, level, 2, last_x[0]);
	foldBufferPolyApproximationAntialiased(interleaved.data() + 1, interleaved.data() + 1024, level, 2, last_x[1]);

	std::vector<q31_t> const l = antialiasedFold(left, level);
	std::vector<q31_t> const r = antialiasedFold(right, level);
	for (size_t n = 0; n < 512; n++) {
		if (interleaved[2 * n] != l[n] || interleaved[(2 * n) + 1] != r[n]) {
			return false;
		}
	}
	return true;
}

// Rendering in blocks, carrying last_x across, gives the same as one long render
bool blocksMatchOneRender() {
	std::vector<q31_t> in = saw(1000, 0.65, 1024);
	std::vector<q31_t> const whole = antialiasedFold(in);
	q31_t last_x = 0;
	for (size_t start = 0; start < in.size(); start += 128) {
		foldBufferPolyApproximationAntialiased(in.data() + start, in.data() + start + 128, kFullLevel, 1, last_x);
	}
	return in == whole;
}
} // namespace

describe fold_adaa(
    "Antialiased wavefolder", $ {
	    it(
	        "gives the plain fold's output for a constant input", _ {
		        // float32 against the integer polynomial: about 6e-5 of full scale at worst
		        expect(constantInputError()).to_be_less_than(1.5e-4);
	        });

	    it(
	        "follows the plain fold of the midpoint for a slowly moving input",
	        _ { expect(slowInputError()).to_be_less_than(1.5e-4); });

	    it(
	        "keeps the plain fold's level on a bright saw", _ {
		        for (double frequency : {110.0, 440.0, 1760.0}) {
			        for (double peak : {0.15, 0.3, 0.6, 0.7}) {
				        std::vector<q31_t> const in = saw(frequency, peak, 4096);
				        double const db = 20 * std::log10(rms(antialiasedFold(in)) / rms(plainFold(in)));
				        expect(std::abs(db)).to_be_less_than(1.0);
			        }
		        }
	        });

	    it(
	        "folds as before past the limit, and stays in range up to saturation", _ {
		        EdgeResult const result = checkEdges();
		        expect(result.samplesPastLimit).to_be_greater_than(size_t{40});
		        expect(result.pastLimitMismatches).to_equal(size_t{0});
		        expect(result.largestOutput).to_be_less_than(1.000001);
	        });

	    it(
	        "folds the two channels of an interleaved buffer independently",
	        _ { expect(stereoMatchesMono()).to_be_true(); });

	    it("carries its state across render blocks", _ { expect(blocksMatchOneRender()).to_be_true(); });
    });

CPPSPEC_SPEC(fold_adaa)
