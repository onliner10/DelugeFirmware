// Bit-exactness check for the block saturation Voice::render uses when a sound has clipping.
//
// Voice::render used to call Sound::saturate() on each sample, i.e.
//     sample = getTanHAntialiased(sample, &lastWorkingValue[channel], 5 + clipping_amount) << shift_amount;
// interleaved with the amplitude scaling and the mixing. It now scales the whole block in place, saturates it
// with dsp::saturateTanHAntialiased() (NEON, 4 samples at a time), then mixes. This spec runs the old per-sample
// code (getTanHAntialiased() itself is unchanged, and reads the real tanH2d table) against the new function and
// checks every output sample and the working values left behind for the next call are identical.
//
// RMSFeedbackCompressor::render saturated its output the same way (saturation_amount 3, no shift), one channel
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
int32_t shiftAmountFor(uint32_t clipping_amount) {
	return (clipping_amount >= 2) ? static_cast<int32_t>(clipping_amount - 2) : 0;
}

// Verbatim per-sample code of Sound::saturate(), channel by channel through the interleaved buffer
template <size_t kNumChannels>
void saturateOld(std::vector<int32_t>& buffer, std::array<uint32_t, 2>& working, uint32_t saturation_amount,
                 int32_t shift_amount) {
	for (size_t i = 0; i < buffer.size(); i++) {
		int32_t* data = &buffer[i];
		*data = getTanHAntialiased(*data, &working[i % kNumChannels], saturation_amount) << (shift_amount);
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
             uint32_t saturation_amount, int32_t shift_amount, std::vector<size_t> const& chunks) {
	std::vector<int32_t> expected = input;
	std::array<uint32_t, 2> expected_working = working;
	saturateOld<kNumChannels>(expected, expected_working, saturation_amount, shift_amount);

	std::vector<int32_t> actual = input;
	size_t position = 0;
	for (size_t c = 0; position < actual.size(); c++) {
		size_t length = chunks[c % chunks.size()] * kNumChannels;
		length = std::min(length, actual.size() - position);
		saturateTanHAntialiased<kNumChannels>(&actual[position], length, working.data(), saturation_amount,
		                                      shift_amount);
		position += length;
	}

	tally.samples += input.size();
	for (size_t i = 0; i < input.size(); i++) {
		if (actual[i] != expected[i]) {
			tally.mismatches++;
		}
	}
	for (size_t c = 0; c < kNumChannels; c++) {
		if (working[c] != expected_working[c]) {
			tally.stateMismatches++;
		}
	}
}

// Inputs around every edge for this saturation amount: the saturation thresholds +-2^(31-s), the int32 limits,
// zero, and values that put the working value at table index 0 / 127 with fraction 0 / 65535
std::vector<int32_t> edgeInputs(uint32_t saturation_amount) {
	std::vector<int32_t> v = {0, 1, -1, 2, -2, kMin, kMin + 1, kMax, kMax - 1};
	int64_t const limit = int64_t{1} << (31 - std::min<uint32_t>(saturation_amount, 31));
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
		auto const shifted = static_cast<int32_t>(w ^ 0x80000000u);
		v.push_back(shifted >> std::min<uint32_t>(saturation_amount, 31));
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
		uint32_t const clipping_amount = s - 5; // What Sound passes: s = 5 + clipping_amount
		int32_t const shift_amount = (s >= 5) ? shiftAmountFor(clipping_amount) : 0;

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
			compare<kNumChannels>(tally, input, working, s, shift_amount, chunkings[w % chunkings.size()]);
		}

		// Random blocks of every length, from random working values, in every chunking
		for (size_t length : lengths()) {
			for (auto const& chunks : chunkings) {
				std::vector<int32_t> const input = randomInputs(seed, length * kNumChannels);
				std::array<uint32_t, 2> working = {lcg(seed), lcg(seed)};
				compare<kNumChannels>(tally, input, working, s, shift_amount, chunks);
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
			compare<kNumChannels>(tally, input, working, s, shift_amount, chunkings[run % chunkings.size()]);
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
void checkVoiceOrder(VoiceTally& tally, uint32_t& seed, uint32_t clipping_amount, bool fm, size_t num_samples) {
	std::vector<int32_t> osc(num_samples * kNumChannels);
	for (auto& s : osc) {
		s = static_cast<int32_t>(lcg(seed)) >> (lcg(seed) & 7);
	}
	auto const amplitude_last_time = static_cast<int32_t>(lcg(seed) >> 1);
	int32_t const increment = static_cast<int32_t>(lcg(seed)) >> 12;
	std::array<uint32_t, 2> working_old = {lcg(seed), lcg(seed)};
	std::array<uint32_t, 2> working_new = working_old;
	int32_t const shift_amount = shiftAmountFor(clipping_amount);

	// Old
	std::vector<int32_t> expected(osc.size());
	int32_t amplitude = amplitude_last_time;
	for (size_t i = 0; i < num_samples; i++) {
		if constexpr (kNumChannels == 2) {
			int32_t l = osc[2 * i];
			int32_t r = osc[(2 * i) + 1];
			amplitude += increment;
			if (!fm) {
				l = multiply_32x32_rshift32_rounded(l, amplitude) << 1;
				r = multiply_32x32_rshift32_rounded(r, amplitude) << 1;
			}
			l = getTanHAntialiased(l, &working_old[0], 5 + clipping_amount) << shift_amount;
			r = getTanHAntialiased(r, &working_old[1], 5 + clipping_amount) << shift_amount;
			expected[2 * i] = l;
			expected[(2 * i) + 1] = r;
		}
		else {
			int32_t output = osc[i];
			if (!fm) {
				amplitude += increment;
				output = multiply_32x32_rshift32_rounded(output, amplitude) << 1;
			}
			expected[i] = getTanHAntialiased(output, &working_old[0], 5 + clipping_amount) << shift_amount;
		}
	}

	// New
	std::vector<int32_t> actual = osc;
	if (!fm) {
		amplitude = amplitude_last_time;
		for (size_t i = 0; i < num_samples; i++) {
			amplitude += increment;
			for (size_t c = 0; c < kNumChannels; c++) {
				int32_t& sample = actual[(i * kNumChannels) + c];
				sample = multiply_32x32_rshift32_rounded(sample, amplitude) << 1;
			}
		}
	}
	saturateTanHAntialiased<kNumChannels>(actual.data(), actual.size(), working_new.data(), 5 + clipping_amount,
	                                      shift_amount);

	tally.samples += actual.size();
	for (size_t i = 0; i < actual.size(); i++) {
		if (actual[i] != expected[i]) {
			tally.mismatches++;
		}
	}
	for (size_t c = 0; c < kNumChannels; c++) {
		if (working_new[c] != working_old[c]) {
			tally.mismatches++;
		}
	}
}

VoiceTally checkVoice() {
	VoiceTally tally;
	uint32_t seed = 77;
	for (uint32_t clipping_amount = 1; clipping_amount <= 15; clipping_amount++) {
		for (bool fm : {false, true}) {
			for (size_t num_samples : {1, 2, 3, 5, 64, 127, 128}) {
				checkVoiceOrder<1>(tally, seed, clipping_amount, fm, num_samples);
				checkVoiceOrder<2>(tally, seed, clipping_amount, fm, num_samples);
			}
		}
	}
	return tally;
}

// RMSFeedbackCompressor::render: verbatim copy of the old per-channel loop (volume ramp, saturation with
// saturation_amount 3, optional wet/dry blend) against the new arrangement: ramp both channels into the buffer,
// saturate the interleaved block, then blend
struct Frame {
	int32_t l;
	int32_t r;
};

void checkCompressorOnce(VoiceTally& tally, uint32_t& seed, size_t num_samples, bool blend, int32_t input_shift) {
	constexpr uint8_t saturation_amount = 3;
	std::vector<Frame> input(num_samples);
	for (auto& f : input) {
		f.l = static_cast<int32_t>(lcg(seed)) >> input_shift;
		f.r = static_cast<int32_t>(lcg(seed)) >> input_shift;
	}
	std::vector<Frame> const dry_buffer = input;
	std::array<int32_t, 2> const start_volume = {static_cast<int32_t>(lcg(seed) >> 1),
	                                             static_cast<int32_t>(lcg(seed) >> 2)};
	std::array<int32_t, 2> const increment = {static_cast<int32_t>(lcg(seed)) >> 10,
	                                          static_cast<int32_t>(lcg(seed)) >> 14};
	auto const wet_amount = static_cast<int32_t>(lcg(seed) >> 1);
	auto const dry_amount = static_cast<int32_t>(lcg(seed) >> 1);
	std::array<uint32_t, 2> const start_working = {lcg(seed), lcg(seed)};

	// Old
	std::vector<Frame> expected = input;
	std::array<int32_t, 2> expected_volume = start_volume;
	std::array<uint32_t, 2> expected_working = start_working;
	auto render_channel = [&](int32_t Frame::* channel, int32_t& currentVolume, int32_t amplitudeIncrement,
	                          uint32_t& lastWorkingValue) {
		int32_t volume = currentVolume;
		uint32_t working_value = lastWorkingValue;
		Frame const* dry_it = dry_buffer.data();
		for (Frame& sample : expected) {
			volume += amplitudeIncrement;
			int32_t value = multiply_32x32_rshift32(sample.*channel, volume) << 4;
			value = getTanHAntialiased(value, &working_value, saturation_amount);
			if (blend) {
				value = multiply_32x32_rshift32(value, wet_amount);
				value = multiply_accumulate_32x32_rshift32_rounded(value, (*dry_it).*channel, dry_amount);
				value <<= 1;
				++dry_it;
			}
			sample.*channel = value;
		}
		currentVolume = volume;
		lastWorkingValue = working_value;
	};
	render_channel(&Frame::l, expected_volume[0], increment[0], expected_working[0]);
	render_channel(&Frame::r, expected_volume[1], increment[1], expected_working[1]);

	// New
	std::vector<Frame> actual = input;
	int32_t volume_l = start_volume[0];
	int32_t volume_r = start_volume[1];
	std::array<uint32_t, 2> working = start_working;
	for (Frame& sample : actual) {
		volume_l += increment[0];
		volume_r += increment[1];
		sample.l = multiply_32x32_rshift32(sample.l, volume_l) << 4;
		sample.r = multiply_32x32_rshift32(sample.r, volume_r) << 4;
	}
	saturateTanHAntialiased<2>(&actual.data()->l, actual.size() * 2, working.data(), saturation_amount, 0);
	if (blend) {
		Frame const* dry_it = dry_buffer.data();
		for (Frame& sample : actual) {
			int32_t l = multiply_32x32_rshift32(sample.l, wet_amount);
			int32_t r = multiply_32x32_rshift32(sample.r, wet_amount);
			l = multiply_accumulate_32x32_rshift32_rounded(l, dry_it->l, dry_amount);
			r = multiply_accumulate_32x32_rshift32_rounded(r, dry_it->r, dry_amount);
			sample.l = l << 1;
			sample.r = r << 1;
			++dry_it;
		}
	}

	tally.samples += actual.size() * 2;
	for (size_t i = 0; i < actual.size(); i++) {
		tally.mismatches += (actual[i].l != expected[i].l) + (actual[i].r != expected[i].r);
	}
	tally.mismatches += static_cast<size_t>(volume_l != expected_volume[0]);
	tally.mismatches += static_cast<size_t>(volume_r != expected_volume[1]);
	tally.mismatches += (working[0] != expected_working[0]) + (working[1] != expected_working[1]);
}

VoiceTally checkCompressor() {
	VoiceTally tally;
	uint32_t seed = 31337;
	for (size_t num_samples : {0, 1, 2, 3, 4, 5, 7, 64, 127, 128}) {
		for (bool blend : {false, true}) {
			for (int32_t input_shift = 0; input_shift < 32; input_shift++) {
				for (size_t run = 0; run < 8; run++) {
					checkCompressorOnce(tally, seed, num_samples, blend, input_shift);
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
