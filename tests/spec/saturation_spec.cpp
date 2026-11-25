// Bit-exactness check for the block saturation Voice::render uses when a sound has clipping.
//
// Voice::render used to call Sound::saturate() on each sample, i.e.
//     sample = getTanHAntialiased(sample, &lastWorkingValue[channel], 5 + clippingAmount) << shiftAmount;
// interleaved with the amplitude scaling and the mixing. It now scales the whole block in place, saturates it
// with dsp::saturateTanHAntialiased() (NEON, 4 samples at a time), then mixes. This spec runs the old per-sample
// code (getTanHAntialiased() itself is unchanged, and reads the real tanH2d table) against the new function and
// checks every output sample and the working values left behind for the next call are identical.
//
// RMSFeedbackCompressor::render saturated its output the same way (saturationAmount 3, no shift), one channel
// at a time; it now uses the same block function, which the last case checks the same way.
//
// On the host the NEON intrinsics run through SIMDe, so this checks the algorithm, not the ARM code generation.

#include "dsp/saturation.hpp"

#include "cppspec.hpp"
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace {
using deluge::dsp::saturateTanHAntialiased;

constexpr int32_t kMin = std::numeric_limits<int32_t>::min();
constexpr int32_t kMax = std::numeric_limits<int32_t>::max();

uint32_t lcg(uint32_t& x) {
	x = (x * 1664525u) + 1013904223u;
	return x;
}

// Sound::getShiftAmountForSaturation()
int32_t shiftAmountFor(uint32_t clippingAmount) {
	return (clippingAmount >= 2) ? static_cast<int32_t>(clippingAmount - 2) : 0;
}

// Verbatim per-sample code of Sound::saturate(), channel by channel through the interleaved buffer
template <size_t kNumChannels>
void saturateOld(std::vector<int32_t>& buffer, std::array<uint32_t, 2>& working, uint32_t saturationAmount,
                 int32_t shiftAmount) {
	for (size_t i = 0; i < buffer.size(); i++) {
		int32_t* data = &buffer[i];
		*data = getTanHAntialiased(*data, &working[i % kNumChannels], saturationAmount) << (shiftAmount);
	}
}

struct Tally {
	size_t samples = 0;
	size_t mismatches = 0;
	size_t stateMismatches = 0;
};

// Runs old and new over the same input from the same working values. The new code is called in chunks of the
// given sizes (cycling), carrying its working values between calls, the way consecutive render calls do.
template <size_t kNumChannels>
void compare(Tally& tally, std::vector<int32_t> const& input, std::array<uint32_t, 2> working,
             uint32_t saturationAmount, int32_t shiftAmount, std::vector<size_t> const& chunks) {
	std::vector<int32_t> expected = input;
	std::array<uint32_t, 2> expectedWorking = working;
	saturateOld<kNumChannels>(expected, expectedWorking, saturationAmount, shiftAmount);

	std::vector<int32_t> actual = input;
	size_t position = 0;
	for (size_t c = 0; position < actual.size(); c++) {
		size_t length = chunks[c % chunks.size()] * kNumChannels;
		length = std::min(length, actual.size() - position);
		saturateTanHAntialiased<kNumChannels>(&actual[position], length, working.data(), saturationAmount, shiftAmount);
		position += length;
	}

	tally.samples += input.size();
	for (size_t i = 0; i < input.size(); i++) {
		if (actual[i] != expected[i]) {
			tally.mismatches++;
		}
	}
	for (size_t c = 0; c < kNumChannels; c++) {
		if (working[c] != expectedWorking[c]) {
			tally.stateMismatches++;
		}
	}
}

// Inputs around every edge for this saturation amount: the saturation thresholds +-2^(31-s), the int32 limits,
// zero, and values that put the working value at table index 0 / 127 with fraction 0 / 65535
std::vector<int32_t> edgeInputs(uint32_t saturationAmount) {
	std::vector<int32_t> v = {0, 1, -1, 2, -2, kMin, kMin + 1, kMax, kMax - 1};
	int64_t const limit = int64_t{1} << (31 - std::min<uint32_t>(saturationAmount, 31));
	for (int64_t d = -2; d <= 2; d++) {
		for (int64_t base : {limit, -limit, limit / 2, -limit / 2}) {
			int64_t const x = base + d;
			if (x >= kMin && x <= kMax) {
				v.push_back(static_cast<int32_t>(x));
			}
		}
	}
	// Working value w = (x << s) ^ 2^31 at x index (w >> 25) and fraction ((w >> 9) & 65535) boundaries
	for (uint32_t w : {0x00000000u, 0x01FFFFFFu, 0x01FFFE00u, 0x02000000u, 0x7FFFFFFFu, 0x80000000u, 0x81FFFE00u,
	                   0xFE000000u, 0xFE0001FFu, 0xFFFFFE00u, 0xFFFFFFFFu}) {
		int32_t const shifted = static_cast<int32_t>(w ^ 0x80000000u);
		v.push_back(shifted >> std::min<uint32_t>(saturationAmount, 31));
	}
	return v;
}

// Pseudo-random samples at every magnitude, so that both saturated and unsaturated inputs occur at every s
std::vector<int32_t> randomInputs(uint32_t& seed, size_t count) {
	std::vector<int32_t> v(count);
	for (auto& sample : v) {
		uint32_t const r = lcg(seed);
		uint32_t const magnitude = lcg(seed) >> 27; // 0..31
		sample = static_cast<int32_t>(r) >> magnitude;
	}
	return v;
}

std::vector<uint32_t> edgeWorkingValues() {
	return {0x80000000u, 0x00000000u, 0xFFFFFFFFu, 0x7FFFFFFFu, 0x03FFFFFFu, 0x04000000u, 0xFC000000u, 0x00000001u};
}

// Lengths per channel: everything up to 17, then a few render-sized blocks
std::vector<size_t> lengths() {
	std::vector<size_t> l;
	for (size_t n = 0; n <= 17; n++) {
		l.push_back(n);
	}
	for (size_t n : {31, 32, 33, 63, 64, 127, 128, 129, 255, 256}) {
		l.push_back(n);
	}
	return l;
}

template <size_t kNumChannels>
Tally checkAll(uint32_t firstSaturationAmount, uint32_t lastSaturationAmount) {
	Tally tally;
	uint32_t seed = 2024 + kNumChannels;
	std::vector<std::vector<size_t>> const chunkings = {{1u << 20}, {1}, {3}, {4}, {5, 7, 1, 16}, {13, 2, 8}};
	for (uint32_t s = firstSaturationAmount; s <= lastSaturationAmount; s++) {
		uint32_t const clippingAmount = s - 5; // What Sound passes: s = 5 + clippingAmount
		int32_t const shiftAmount = (s >= 5) ? shiftAmountFor(clippingAmount) : 0;

		// Every edge input, at every position of a block, after every edge working value
		std::vector<int32_t> const edges = edgeInputs(s);
		for (uint32_t w : edgeWorkingValues()) {
			std::vector<int32_t> input;
			for (int32_t a : edges) {
				for (int32_t b : edges) {
					input.push_back(a);
					input.push_back(b);
				}
			}
			std::array<uint32_t, 2> working = {w, ~w};
			compare<kNumChannels>(tally, input, working, s, shiftAmount, chunkings[w % chunkings.size()]);
		}

		// Random blocks of every length, from random working values, in every chunking
		for (size_t length : lengths()) {
			for (auto const& chunks : chunkings) {
				std::vector<int32_t> const input = randomInputs(seed, length * kNumChannels);
				std::array<uint32_t, 2> working = {lcg(seed), lcg(seed)};
				compare<kNumChannels>(tally, input, working, s, shiftAmount, chunks);
			}
		}

		// Long full-range runs, unsaturated runs and slow ramps
		for (size_t run = 0; run < 8; run++) {
			std::vector<int32_t> input(4096 * kNumChannels);
			for (size_t i = 0; i < input.size(); i++) {
				uint32_t const r = lcg(seed);
				switch (run % 4) {
				case 0:
					input[i] = static_cast<int32_t>(r);
					break;
				case 1:
					input[i] = static_cast<int32_t>(r) >> std::min<uint32_t>(s, 31);
					break;
				case 2:
					input[i] = static_cast<int32_t>(static_cast<uint32_t>(i) * 1048573u);
					break;
				default:
					input[i] = static_cast<int32_t>(r) >> (r & 31);
					break;
				}
			}
			std::array<uint32_t, 2> working = {lcg(seed), lcg(seed)};
			compare<kNumChannels>(tally, input, working, s, shiftAmount, chunkings[run % chunkings.size()]);
		}
	}
	return tally;
}

// Verbatim copy of the old stereo "yes clipping" loop in Voice::render, minus the mixing, against the new
// arrangement: amplitude scaling in place, then the block saturation
struct VoiceTally {
	size_t samples = 0;
	size_t mismatches = 0;
};

template <size_t kNumChannels>
void checkVoiceOrder(VoiceTally& tally, uint32_t& seed, uint32_t clippingAmount, bool fm, size_t numSamples) {
	std::vector<int32_t> osc(numSamples * kNumChannels);
	for (auto& s : osc) {
		s = static_cast<int32_t>(lcg(seed)) >> (lcg(seed) & 7);
	}
	int32_t const amplitudeLastTime = static_cast<int32_t>(lcg(seed) >> 1);
	int32_t const increment = static_cast<int32_t>(lcg(seed)) >> 12;
	std::array<uint32_t, 2> workingOld = {lcg(seed), lcg(seed)};
	std::array<uint32_t, 2> workingNew = workingOld;
	int32_t const shiftAmount = shiftAmountFor(clippingAmount);

	// Old
	std::vector<int32_t> expected(osc.size());
	int32_t amplitude = amplitudeLastTime;
	for (size_t i = 0; i < numSamples; i++) {
		if constexpr (kNumChannels == 2) {
			int32_t l = osc[2 * i];
			int32_t r = osc[(2 * i) + 1];
			amplitude += increment;
			if (!fm) {
				l = multiply_32x32_rshift32_rounded(l, amplitude) << 1;
				r = multiply_32x32_rshift32_rounded(r, amplitude) << 1;
			}
			l = getTanHAntialiased(l, &workingOld[0], 5 + clippingAmount) << shiftAmount;
			r = getTanHAntialiased(r, &workingOld[1], 5 + clippingAmount) << shiftAmount;
			expected[2 * i] = l;
			expected[(2 * i) + 1] = r;
		}
		else {
			int32_t output = osc[i];
			if (!fm) {
				amplitude += increment;
				output = multiply_32x32_rshift32_rounded(output, amplitude) << 1;
			}
			expected[i] = getTanHAntialiased(output, &workingOld[0], 5 + clippingAmount) << shiftAmount;
		}
	}

	// New
	std::vector<int32_t> actual = osc;
	if (!fm) {
		amplitude = amplitudeLastTime;
		for (size_t i = 0; i < numSamples; i++) {
			amplitude += increment;
			for (size_t c = 0; c < kNumChannels; c++) {
				int32_t& sample = actual[(i * kNumChannels) + c];
				sample = multiply_32x32_rshift32_rounded(sample, amplitude) << 1;
			}
		}
	}
	saturateTanHAntialiased<kNumChannels>(actual.data(), actual.size(), workingNew.data(), 5 + clippingAmount,
	                                      shiftAmount);

	tally.samples += actual.size();
	for (size_t i = 0; i < actual.size(); i++) {
		if (actual[i] != expected[i]) {
			tally.mismatches++;
		}
	}
	for (size_t c = 0; c < kNumChannels; c++) {
		if (workingNew[c] != workingOld[c]) {
			tally.mismatches++;
		}
	}
}

VoiceTally checkVoice() {
	VoiceTally tally;
	uint32_t seed = 77;
	for (uint32_t clippingAmount = 1; clippingAmount <= 15; clippingAmount++) {
		for (bool fm : {false, true}) {
			for (size_t numSamples : {1, 2, 3, 5, 64, 127, 128}) {
				checkVoiceOrder<1>(tally, seed, clippingAmount, fm, numSamples);
				checkVoiceOrder<2>(tally, seed, clippingAmount, fm, numSamples);
			}
		}
	}
	return tally;
}

// RMSFeedbackCompressor::render: verbatim copy of the old per-channel loop (volume ramp, saturation with
// saturationAmount 3, optional wet/dry blend) against the new arrangement: ramp both channels into the buffer,
// saturate the interleaved block, then blend
struct Frame {
	int32_t l;
	int32_t r;
};

void checkCompressorOnce(VoiceTally& tally, uint32_t& seed, size_t numSamples, bool blend, int32_t inputShift) {
	constexpr uint8_t saturationAmount = 3;
	std::vector<Frame> input(numSamples);
	for (auto& f : input) {
		f.l = static_cast<int32_t>(lcg(seed)) >> inputShift;
		f.r = static_cast<int32_t>(lcg(seed)) >> inputShift;
	}
	std::vector<Frame> const dryBuffer = input;
	int32_t const startVolume[2] = {static_cast<int32_t>(lcg(seed) >> 1), static_cast<int32_t>(lcg(seed) >> 2)};
	int32_t const increment[2] = {static_cast<int32_t>(lcg(seed)) >> 10, static_cast<int32_t>(lcg(seed)) >> 14};
	int32_t const wetAmount = static_cast<int32_t>(lcg(seed) >> 1);
	int32_t const dryAmount = static_cast<int32_t>(lcg(seed) >> 1);
	std::array<uint32_t, 2> const startWorking = {lcg(seed), lcg(seed)};

	// Old
	std::vector<Frame> expected = input;
	int32_t expectedVolume[2] = {startVolume[0], startVolume[1]};
	std::array<uint32_t, 2> expectedWorking = startWorking;
	auto renderChannel = [&](int32_t Frame::* channel, int32_t& currentVolume, int32_t amplitudeIncrement,
	                         uint32_t& lastWorkingValue) {
		int32_t volume = currentVolume;
		uint32_t workingValue = lastWorkingValue;
		Frame const* dry_it = dryBuffer.data();
		for (Frame& sample : expected) {
			volume += amplitudeIncrement;
			int32_t value = multiply_32x32_rshift32(sample.*channel, volume) << 4;
			value = getTanHAntialiased(value, &workingValue, saturationAmount);
			if (blend) {
				value = multiply_32x32_rshift32(value, wetAmount);
				value = multiply_accumulate_32x32_rshift32_rounded(value, (*dry_it).*channel, dryAmount);
				value <<= 1;
				++dry_it;
			}
			sample.*channel = value;
		}
		currentVolume = volume;
		lastWorkingValue = workingValue;
	};
	renderChannel(&Frame::l, expectedVolume[0], increment[0], expectedWorking[0]);
	renderChannel(&Frame::r, expectedVolume[1], increment[1], expectedWorking[1]);

	// New
	std::vector<Frame> actual = input;
	int32_t volumeL = startVolume[0];
	int32_t volumeR = startVolume[1];
	std::array<uint32_t, 2> working = startWorking;
	for (Frame& sample : actual) {
		volumeL += increment[0];
		volumeR += increment[1];
		sample.l = multiply_32x32_rshift32(sample.l, volumeL) << 4;
		sample.r = multiply_32x32_rshift32(sample.r, volumeR) << 4;
	}
	saturateTanHAntialiased<2>(&actual.data()->l, actual.size() * 2, working.data(), saturationAmount, 0);
	if (blend) {
		Frame const* dry_it = dryBuffer.data();
		for (Frame& sample : actual) {
			int32_t l = multiply_32x32_rshift32(sample.l, wetAmount);
			int32_t r = multiply_32x32_rshift32(sample.r, wetAmount);
			l = multiply_accumulate_32x32_rshift32_rounded(l, dry_it->l, dryAmount);
			r = multiply_accumulate_32x32_rshift32_rounded(r, dry_it->r, dryAmount);
			sample.l = l << 1;
			sample.r = r << 1;
			++dry_it;
		}
	}

	tally.samples += actual.size() * 2;
	for (size_t i = 0; i < actual.size(); i++) {
		tally.mismatches += (actual[i].l != expected[i].l) + (actual[i].r != expected[i].r);
	}
	tally.mismatches += (volumeL != expectedVolume[0]) + (volumeR != expectedVolume[1]);
	tally.mismatches += (working[0] != expectedWorking[0]) + (working[1] != expectedWorking[1]);
}

VoiceTally checkCompressor() {
	VoiceTally tally;
	uint32_t seed = 31337;
	for (size_t numSamples : {0, 1, 2, 3, 4, 5, 7, 64, 127, 128}) {
		for (bool blend : {false, true}) {
			for (int32_t inputShift = 0; inputShift < 32; inputShift++) {
				for (size_t run = 0; run < 8; run++) {
					checkCompressorOnce(tally, seed, numSamples, blend, inputShift);
				}
			}
		}
	}
	return tally;
}
} // namespace

describe saturation(
    "Block tanh saturation", $ {
	    it(
	        "matches Sound::saturate() per sample, mono, for every clipping amount 1-15 (s = 6-20)", _ {
		        Tally const tally = checkAll<1>(6, 20);
		        expect(tally.samples).to_be_greater_than(size_t{900'000});
		        expect(tally.mismatches).to_equal(size_t{0});
		        expect(tally.stateMismatches).to_equal(size_t{0});
	        });

	    it(
	        "matches Sound::saturate() per sample, stereo, for every clipping amount 1-15 (s = 6-20)", _ {
		        Tally const tally = checkAll<2>(6, 20);
		        expect(tally.samples).to_be_greater_than(size_t{1'500'000});
		        expect(tally.mismatches).to_equal(size_t{0});
		        expect(tally.stateMismatches).to_equal(size_t{0});
	        });

	    it(
	        "matches outside the range Sound uses: s = 1-5 on the vector path, 21-31 on the scalar fallback", _ {
		        Tally const low1 = checkAll<1>(1, 5);
		        Tally const low2 = checkAll<2>(1, 5);
		        Tally const high1 = checkAll<1>(21, 31);
		        Tally const high2 = checkAll<2>(21, 31);
		        expect(low1.mismatches + low2.mismatches + high1.mismatches + high2.mismatches).to_equal(size_t{0});
		        expect(low1.stateMismatches + low2.stateMismatches + high1.stateMismatches + high2.stateMismatches)
		            .to_equal(size_t{0});
	        });

	    it(
	        "matches the old Voice::render order: amplitude, then saturation, per sample, mono and stereo, FM or not",
	        _ {
		        VoiceTally const tally = checkVoice();
		        expect(tally.samples).to_be_greater_than(size_t{10'000});
		        expect(tally.mismatches).to_equal(size_t{0});
	        });

	    it(
	        "matches the old RMSFeedbackCompressor::render loop: volume ramp, saturation, wet/dry blend", _ {
		        VoiceTally const tally = checkCompressor();
		        expect(tally.samples).to_be_greater_than(size_t{100'000});
		        expect(tally.mismatches).to_equal(size_t{0});
	        });
    });

CPPSPEC_SPEC(saturation)
