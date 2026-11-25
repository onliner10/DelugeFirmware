// The ladder LPF without feedback runs its later stages a sample behind the earlier ones (doFilterSkewed()). It has to
// give exactly what the plain per-sample loop (doFilterT<false, false>()) does: same samples, same state after.

#include "cppspec.hpp"
#include "dsp/filter/lpladder.h"
#include "util/lookuptables/lookuptables.h"
#include <array>
#include <cstdint>
#include <random>
#include <vector>

uint32_t z = 362436069, w = 521288629, jcong = 380116160;

// lpladder.cpp's setConfig() needs these, from parts of the firmware the specs don't build. Neither matters for a
// filter without feedback: the log frequency only picks oversampling for the drive ladder.
int32_t quickLog(uint32_t input) {
	return static_cast<int32_t>(input >> 1);
}
namespace AudioEngine {
int32_t cpuDireness = 0;
}
// As in util/functions.cpp, for Filter::curveFrequency()
int32_t instantTan(int32_t input) {
	int32_t which_value = input >> 25;
	int32_t how_much_further = (input << 6) & 2147483647;
	int32_t value1 = tanTable[which_value];
	int32_t value2 = tanTable[which_value + 1];
	return (multiply_32x32_rshift32(value2, how_much_further)
	        + multiply_32x32_rshift32(value1, 2147483647 - how_much_further))
	       << 1;
}

namespace deluge::dsp::filter {
struct LpLadderSpecAccess {
	static void reference(LpLadderFilter& filter, q31_t* start, q31_t* end, int32_t increment) {
		filter.doFilterT<false, false>(start, end, increment);
	}
	static bool hasNoFeedback(LpLadderFilter const& filter) {
		return filter.morph <= 0 && filter.processedResonance == 0
		       && filter.divideByTotalMoveabilityAndProcessedResonance == (1 << 30);
	}
	static bool sameState(LpLadderFilter const& a, LpLadderFilter const& b) {
		auto same = [](LpLadderFilter::LpLadderState const& x, LpLadderFilter::LpLadderState const& y) {
			return x.noiseLastValue == y.noiseLastValue && x.lpfLPF1.memory == y.lpfLPF1.memory
			       && x.lpfLPF2.memory == y.lpfLPF2.memory && x.lpfLPF3.memory == y.lpfLPF3.memory
			       && x.lpfLPF4.memory == y.lpfLPF4.memory;
		};
		return same(a.l, b.l) && same(a.r, b.r);
	}
};
} // namespace deluge::dsp::filter

using deluge::dsp::filter::LpLadderFilter;
using deluge::dsp::filter::LpLadderSpecAccess;

namespace {
struct Result {
	bool configured = true;
	bool samplesMatch = true;
	bool stateMatches = true;
};

Result compare(FilterMode mode, q31_t frequency, int32_t increment, int32_t blockFrames, uint32_t inputSeed) {
	Result result;
	LpLadderFilter reference{};
	reference.resetFilter();
	reference.configure(frequency, 0, mode, 0, 1 << 28);
	LpLadderFilter skewed = reference; // Same state to start with, including what resetFilter() doesn't set
	if (!LpLadderSpecAccess::hasNoFeedback(skewed)) {
		result.configured = false;
		return result;
	}
	std::mt19937 random(inputSeed);
	std::vector<q31_t> a(static_cast<size_t>(blockFrames * increment));
	std::vector<q31_t> b(a.size());
	for (int32_t block = 0; block < 40; block++) {
		for (size_t i = 0; i < a.size(); i++) {
			a[i] = b[i] = static_cast<q31_t>(random()) >> (1 + (block % 4)); // Quieter and louder
		}
		// Both use the global noise seed, so each starts from the same one and must leave the same one
		uint32_t const seedBefore = jcong;
		LpLadderSpecAccess::reference(reference, a.data(), a.data() + a.size(), increment);
		uint32_t const referenceSeedAfter = jcong;
		jcong = seedBefore;
		skewed.doFilter(b.data(), b.data() + b.size(), increment);
		result.samplesMatch = result.samplesMatch && (a == b) && (jcong == referenceSeedAfter);
	}
	result.stateMatches = LpLadderSpecAccess::sameState(reference, skewed);
	return result;
}
} // namespace

describe lpladder(
    "LpLadderFilter without feedback", $ {
	    for (FilterMode mode : {FilterMode::TRANSISTOR_12DB, FilterMode::TRANSISTOR_24DB}) {
		    for (q31_t frequency : {-2000000000, -800000000, 0, 600000000, 1500000000, 2147483647}) {
			    for (int32_t increment : {1, 2}) {
				    for (int32_t frames : {1, 2, 3, 7, 32, 128}) {
					    it(
					        "gives the same samples and state as the per-sample loop", _ {
						        Result r = compare(mode, frequency, increment, frames,
						                           static_cast<uint32_t>(frequency) ^ frames);
						        expect(r.configured).to_equal(true);
						        expect(r.samplesMatch).to_equal(true);
						        expect(r.stateMatches).to_equal(true);
					        });
				    }
			    }
		    }
	    }
    });

CPPSPEC_SPEC(lpladder)
