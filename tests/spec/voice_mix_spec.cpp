#include "dsp/voice_mix.h"

#include "cppspec.hpp"
#include <cstdint>
#include <vector>

namespace {
using namespace deluge::dsp;

constexpr int32_t kCanary = 0x5A5A5A5A;

// smmulr, written out: the high word of x·a, rounded
int32_t smmulr(int32_t x, int32_t a) {
	return static_cast<int32_t>((static_cast<int64_t>(x) * a + 0x80000000LL) >> 32);
}

// Voice::render's per-sample loop before the kernel, with its wrapping made explicit
void referenceRamp(int32_t* out, int32_t const* in, int32_t numSamples, int32_t amplitude, int32_t increment) {
	uint32_t amplitudeNow = amplitude;
	for (int32_t i = 0; i < numSamples; i++) {
		amplitudeNow += static_cast<uint32_t>(increment);
		uint32_t const output = static_cast<uint32_t>(smmulr(in[i], static_cast<int32_t>(amplitudeNow))) << 1;
		out[i] = static_cast<int32_t>(static_cast<uint32_t>(out[i]) + output);
	}
}

struct Random {
	uint32_t state;
	int32_t next() {
		state = state * 1664525u + 1013904223u;
		return static_cast<int32_t>(state);
	}
};

int32_t const kExtremes[] = {INT32_MIN,     INT32_MAX,     0,          1,           -1,
                             INT32_MIN + 1, INT32_MAX - 1, 0x40000000, -0x40000000, 0x00008000};

// Samples from the generator, with the extremes mixed in at a stride so every lane meets them
std::vector<int32_t> makeSamples(size_t size, Random& random, bool extremes) {
	std::vector<int32_t> samples(size);
	for (size_t i = 0; i < size; i++) {
		samples[i] = random.next();
		if (extremes && i % 3 == 0) {
			samples[i] = kExtremes[(i / 3 + random.state) % std::size(kExtremes)];
		}
	}
	return samples;
}

// The oscillator samples the kernel multiplies. Off ARM, SIMDe (v0.8.2) stands in for vqdmulh, and its
// simde_vqdmulls_s32 saturates every product whose first operand is INT32_MIN, where the instruction saturates only
// INT32_MIN · INT32_MIN. On the host those samples are moved off INT32_MIN; on ARM they are kept.
std::vector<int32_t> makeInputs(size_t size, Random& random, bool extremes) {
	std::vector<int32_t> samples = makeSamples(size, random, extremes);
#if !defined(__ARM_NEON)
	for (int32_t& sample : samples) {
		if (sample == INT32_MIN) {
			sample = INT32_MIN + 1;
		}
	}
#endif
	return samples;
}

// Runs the kernel and the reference on the same buffers, starting `offset` words in (so the NEON loads and stores
// are unaligned for some cases), with canaries before and after the range the kernel may touch.
template <typename Kernel, typename Reference>
bool matches(Kernel kernel, Reference reference, int32_t numSamples, size_t offset, Random& random, bool extremes) {
	size_t const size = offset + numSamples + 4;
	std::vector<int32_t> const in = makeInputs(size, random, extremes);
	std::vector<int32_t> out = makeSamples(size, random, extremes);
	for (size_t i = 0; i < offset; i++) {
		out[i] = kCanary;
	}
	for (size_t i = offset + numSamples; i < size; i++) {
		out[i] = kCanary;
	}
	std::vector<int32_t> expected = out;
	kernel(out.data() + offset, in.data() + offset);
	reference(expected.data() + offset, in.data() + offset);
	return out == expected;
}

bool rampMatches(int32_t numSamples, int32_t amplitude, int32_t increment, size_t offset, Random& random,
                 bool extremes) {
	return matches(
	    [&](int32_t* out, int32_t const* in) { addMonoWithAmplitudeRamp(out, in, numSamples, amplitude, increment); },
	    [&](int32_t* out, int32_t const* in) { referenceRamp(out, in, numSamples, amplitude, increment); }, numSamples,
	    offset, random, extremes);
}

} // namespace

// clang-format off
describe voice_mix("Voice mono mix", $ {
	it("matches the per-sample ramp for every length from 1 to 130, random samples and ramps", _ {
		Random random{1};
		bool allMatch = true;
		for (int32_t n = 1; n <= 130; n++) {
			for (size_t offset = 0; offset < 4; offset++) {
				allMatch = allMatch && rampMatches(n, random.next(), random.next() >> (n % 32), offset, random, false);
			}
		}
		expect(allMatch).to_be_true();
	});

	it("matches the per-sample ramp on extreme samples and amplitudes", _ {
		Random random{2};
		bool allMatch = true;
		for (int32_t amplitude : kExtremes) {
			for (int32_t increment : {0, 1, -1, INT32_MIN, INT32_MAX, 0x10000000, -0x10000000}) {
				for (int32_t n : {1, 3, 4, 5, 7, 8, 9, 12, 16, 17, 33, 128}) {
					allMatch = allMatch && rampMatches(n, amplitude, increment, n % 4, random, true);
				}
			}
		}
		expect(allMatch).to_be_true();
	});

	it("matches the per-sample ramp where the amplitude wraps mid-block", _ {
		Random random{3};
		bool allMatch = true;
		// Steps of 2^28 from just under INT32_MAX wrap to negative on the first step, and wrap around every 16
		for (int32_t n : {4, 8, 15, 16, 31, 64, 130}) {
			allMatch = allMatch && rampMatches(n, INT32_MAX - 5, 0x10000000, 0, random, true);
			allMatch = allMatch && rampMatches(n, INT32_MIN + 5, -0x10000000, 1, random, true);
		}
		expect(allMatch).to_be_true();
	});

#if defined(__ARM_NEON)
	// Needs INT32_MIN samples, which SIMDe's vqdmulh gets wrong (see makeInputs)
	it("matches smmulr on the one product vqdmulh saturates", _ {
		// INT32_MIN · INT32_MIN: smmulr gives 2^30, so the output is out + 2^31, wrapping
		std::vector<int32_t> in(4, INT32_MIN);
		std::vector<int32_t> out{0, 5, -7, INT32_MAX};
		std::vector<int32_t> expected = out;
		addMonoWithAmplitudeRamp(out.data(), in.data(), 4, INT32_MIN, 0);
		referenceRamp(expected.data(), in.data(), 4, INT32_MIN, 0);
		expect(out == expected).to_be_true();
		expect(out[0]).to_equal(INT32_MIN);
	});
#endif
});

CPPSPEC_SPEC(voice_mix)
