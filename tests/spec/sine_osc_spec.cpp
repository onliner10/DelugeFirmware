// Bit-exactness check for SineOsc's vector renderer (getSineVector / doFMVector).
//
// The vector table lookup used to gather {value, diff} int16 pairs with
// ArgonHalf<int16_t>::LoadGatherOffsetIndexInterleaved<2>. It now gathers each pair as one 32-bit word and
// splits it by narrowing. This spec keeps a verbatim copy of the old expression and checks that the new code
// gives identical int32 lanes, and also checks both against a plain scalar model of the arithmetic.
//
// The table is private, so it is rebuilt from the scalar doFMNew(), which reads the same table and is not
// touched by the change. The reference therefore never runs through the code under test.

#include "dsp/oscillators/sine_osc.h"

#include "cppspec.hpp"
#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

namespace {
using deluge::dsp::SineOsc;

using Lanes = std::array<uint32_t, 4>;
using Out = std::array<int32_t, 4>;

// The {value, diff} pairs, rebuilt through the scalar doFMNew()
struct Table {
	alignas(16) std::array<int16_t, 512> pairs{};
	bool ok = true; // value[i] + diff[i] == value[i + 1] for every i, wrapping
};

Table const& table() {
	static Table const t = [] {
		Table t;
		for (uint32_t i = 0; i < 256; i++) {
			// Fraction 0 returns value << 16; fraction 1 adds diff once
			auto const v16 = static_cast<uint32_t>(SineOsc::doFMNew(i << 24, 0));
			auto const v16_plus_diff = static_cast<uint32_t>(SineOsc::doFMNew((i << 24) | (1u << 8), 0));
			t.pairs[2 * i] = static_cast<int16_t>(v16 >> 16);
			t.pairs[(2 * i) + 1] = static_cast<int16_t>(static_cast<int32_t>(v16_plus_diff - v16));
			t.ok = t.ok && (v16 & 0xFFFF) == 0;
		}
		for (uint32_t i = 0; i < 256; i++) {
			int32_t const next = t.pairs[2 * i] + t.pairs[(2 * i) + 1];
			t.ok = t.ok && next == t.pairs[((2 * i) + 2) % 512];
		}
		return t;
	}();
	return t;
}

Argon<uint32_t> toVector(Lanes const& lanes) {
	return Argon<uint32_t>::Load(lanes.data());
}

Out toArray(Argon<int32_t> v) {
	Out out{};
	v.StoreTo(out.data());
	return out;
}

// Verbatim copy of SineOsc::render() before the change, run against the rebuilt table
Out legacyRender(Lanes const& lanes) {
	constexpr size_t sine_table_size_magnitude = 8;
	Argon<uint32_t> phase = toVector(lanes);
	ArgonHalf<int16_t> strength2 = phase.ShiftRightNarrow<32 - 16 - sine_table_size_magnitude + 1>()
	                                   .BitwiseAnd(std::numeric_limits<int16_t>::max())
	                                   .As<int16_t>();
	Argon<uint32_t> indices = phase >> (32 - sine_table_size_magnitude);
	auto [sine, diff] = ArgonHalf<int16_t>::LoadGatherOffsetIndexInterleaved<2>(table().pairs.data(), indices.Narrow());
	return toArray(sine.ShiftLeftLong<16>().MultiplyDoubleAddSaturateLong(diff, strength2));
}

// Plain scalar model: (value << 16) + 2 * diff * fraction15, saturating like vqdmlal
Out scalarModel(Lanes const& lanes) {
	Out out{};
	for (size_t i = 0; i < 4; i++) {
		uint32_t const index = lanes[i] >> 24;
		int64_t const fraction = (lanes[i] >> 9) & 0x7FFF;
		int64_t const value = table().pairs[2 * index];
		int64_t const diff = table().pairs[(2 * index) + 1];
		int64_t const sum = (value * 65536) + (2 * diff * fraction);
		out[i] = static_cast<int32_t>(std::clamp<int64_t>(sum, INT32_MIN, INT32_MAX));
	}
	return out;
}

// Counts compared vectors and mismatches across all three implementations
struct Tally {
	size_t vectors = 0;
	size_t mismatches = 0;

	void check(Lanes const& phase, Out const& actual) {
		Out const legacy = legacyRender(phase);
		vectors++;
		if (actual != legacy || legacy != scalarModel(phase)) {
			mismatches++;
		}
	}
};

// The new render, through the public doFMVector() with no phase shift
Out renderNew(Lanes const& lanes) {
	return toArray(SineOsc::doFMVector(toVector(lanes), Argon<uint32_t>{0u}));
}

uint32_t lcg(uint32_t& x) {
	x = (x * 1664525u) + 1013904223u;
	return x;
}

// Fractional parts (low 24 bits) at every boundary the renderer cares about, plus pseudo-random ones
std::vector<uint32_t> fractions() {
	std::vector<uint32_t> f = {0x000000, 0x000001, 0x0000FF, 0x000100, 0x0001FF, 0x000200, 0x000201,
	                           0x0003FF, 0x7FFDFF, 0x7FFE00, 0x7FFFFF, 0x800000, 0x800001, 0x8001FF,
	                           0x800200, 0xFFFC00, 0xFFFDFF, 0xFFFE00, 0xFFFEFF, 0xFFFF00, 0xFFFFFF};
	for (uint32_t bit = 0; bit < 24; bit++) {
		f.push_back(1u << bit);
		f.push_back(0xFFFFFFu ^ (1u << bit));
	}
	uint32_t x = 12345;
	while (f.size() < 128) {
		f.push_back(lcg(x) & 0xFFFFFF);
	}
	return f;
}

// Whole phases at the wrap points and table-index boundaries
std::vector<uint32_t> edgePhases() {
	return {0x00000000, 0x00000001, 0x000001FF, 0x00000200, 0x00FFFFFF, 0x01000000, 0x01000001,
	        0x3FFFFFFF, 0x40000000, 0x7FFFFFFF, 0x80000000, 0x80000001, 0xBFFFFFFF, 0xC0000000,
	        0xFEFFFFFF, 0xFF000000, 0xFFFFFE00, 0xFFFFFFFE, 0xFFFFFFFF};
}
// Every table index in every lane, each over 128 fractions
Tally checkEveryIndex() {
	auto const f = fractions();
	Tally tally;
	for (uint32_t index = 0; index < 256; index++) {
		for (size_t k = 0; k < f.size(); k++) {
			// Each lane gets a different index and fraction, and every index visits every lane
			Lanes phase{};
			for (uint32_t lane = 0; lane < 4; lane++) {
				uint32_t const lane_index = (index + (lane * 67)) & 255;
				phase[lane] = (lane_index << 24) | f[(k + (lane * 37)) % f.size()];
			}
			tally.check(phase, renderNew(phase));
		}
	}
	return tally;
}

// Every combination of the edge phases across the 4 lanes
Tally checkEdgeCombinations() {
	auto const e = edgePhases();
	Tally tally;
	for (uint32_t a : e) {
		for (uint32_t b : e) {
			for (uint32_t c : e) {
				for (uint32_t d : e) {
					Lanes const phase{a, b, c, d};
					tally.check(phase, renderNew(phase));
				}
			}
		}
	}
	return tally;
}

size_t edgeCombinationCount() {
	auto const n = edgePhases().size();
	return n * n * n * n;
}

Tally checkRandomPhases(size_t count) {
	Tally tally;
	uint32_t x = 1;
	for (size_t n = 0; n < count; n++) {
		Lanes const phase{lcg(x), lcg(x), lcg(x), lcg(x)};
		tally.check(phase, renderNew(phase));
	}
	return tally;
}

std::vector<uint32_t> fmShifts() {
	return {0x00000000, 0x00000001, 0x00FFFFFF, 0x01000000, 0x7FFFFFFF, 0x80000000, 0xFF000000, 0xFFFFFFFF};
}

// doFMVector(carrier, shift) against the old render of carrier + (shift << 8)
void checkFm(Tally& tally, Lanes const& carrier, Lanes const& shift) {
	Lanes shifted{};
	for (uint32_t lane = 0; lane < 4; lane++) {
		shifted[lane] = carrier[lane] + (shift[lane] << 8);
	}
	tally.check(shifted, toArray(SineOsc::doFMVector(toVector(carrier), toVector(shift))));
}

// The FM path, with random carriers and shifts (every 4th vector using the edge shifts), then edge carriers
Tally checkFmPath(size_t randomCount) {
	auto const e = edgePhases();
	auto const shifts = fmShifts();
	Tally tally;
	uint32_t x = 7;
	for (size_t n = 0; n < randomCount; n++) {
		Lanes const carrier{lcg(x), lcg(x), lcg(x), lcg(x)};
		Lanes shift{lcg(x), lcg(x), lcg(x), lcg(x)};
		if (n % 4 == 0) {
			for (uint32_t lane = 0; lane < 4; lane++) {
				shift[lane] = shifts[((n / 4) + lane) % shifts.size()];
			}
		}
		checkFm(tally, carrier, shift);
	}
	for (uint32_t c : e) {
		for (uint32_t s : shifts) {
			checkFm(tally, Lanes{c, c + 1, c - 1, ~c}, Lanes{s, s + 1, s - 1, ~s});
		}
	}
	return tally;
}

struct SineVectorTally {
	Tally tally;
	size_t phase_errors = 0;
};

// getSineVector() from each start phase and increment, for 64 blocks, checking the phase it advances too
SineVectorTally checkGetSineVector(size_t runs) {
	std::vector<uint32_t> const increments = {0, 1, 0x200, 0x00FFFFFF, 0x01000000, 0x40000000, 0x7FFFFFFF, 0xFFFFFFFF};
	SineVectorTally result;
	uint32_t x = 99;
	for (size_t run = 0; run < runs; run++) {
		uint32_t increment = 0;
		uint32_t phase = 0xFFFFFFF0u;
		if (run < increments.size()) {
			increment = increments[run];
		}
		else {
			increment = lcg(x);
			phase = lcg(x);
		}
		for (size_t block = 0; block < 64; block++) {
			Lanes expected{};
			for (uint32_t lane = 0; lane < 4; lane++) {
				expected[lane] = phase + (increment * (lane + 1));
			}
			uint32_t const before = phase;
			Out const actual = toArray(SineOsc::getSineVector(&phase, increment));
			result.tally.check(expected, actual);
			if (phase != before + (increment * 4)) {
				result.phase_errors++;
			}
		}
	}
	return result;
}
} // namespace

describe sine_osc(
    "SineOsc vector render", $ {
	    it(
	        "rebuilds a consistent table from the scalar doFMNew", _ {
		        expect(table().ok).to_be_true();
		        expect(static_cast<int32_t>(table().pairs[0])).to_equal(0);
		        expect(static_cast<int32_t>(table().pairs[128])).to_equal(32767);
	        });

	    it(
	        "matches the old gather for every table index in every lane, over 128 fractions", _ {
		        Tally const tally = checkEveryIndex();
		        expect(tally.vectors).to_equal(size_t{256 * 128});
		        expect(tally.mismatches).to_equal(size_t{0});
	        });

	    it(
	        "matches the old gather for every combination of edge phases across the 4 lanes", _ {
		        Tally const tally = checkEdgeCombinations();
		        expect(tally.vectors).to_equal(edgeCombinationCount());
		        expect(tally.mismatches).to_equal(size_t{0});
	        });

	    it(
	        "matches the old gather for 1M pseudo-random phase vectors", _ {
		        Tally const tally = checkRandomPhases(1'000'000);
		        expect(tally.mismatches).to_equal(size_t{0});
	        });

	    it(
	        "matches on the FM path: doFMVector with a phase shift, including wrap-around", _ {
		        Tally const tally = checkFmPath(200'000);
		        expect(tally.vectors).to_equal(200'000 + (edgePhases().size() * fmShifts().size()));
		        expect(tally.mismatches).to_equal(size_t{0});
	        });

	    it(
	        "matches through getSineVector, including the phase it advances", _ {
		        SineVectorTally const result = checkGetSineVector(2'000);
		        expect(result.tally.vectors).to_equal(size_t{2'000 * 64});
		        expect(result.tally.mismatches).to_equal(size_t{0});
		        expect(result.phase_errors).to_equal(size_t{0});
	        });
    });

CPPSPEC_SPEC(sine_osc)
