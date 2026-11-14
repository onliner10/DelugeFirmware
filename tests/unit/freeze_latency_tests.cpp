#include "CppUTest/TestHarness.h"
#include "model/clip/freeze_latency.h"
#include <cmath>
#include <cstdint>
#include <random>
#include <vector>

using deluge::freeze::blockLevels;
using deluge::freeze::estimateLatency;
using deluge::freeze::kLatencyBlockSize;
using deluge::freeze::LatencyEstimate;

namespace {

constexpr int32_t kRate = 44100;
constexpr int32_t kMaxLag = 2048; // What a freeze recording has past the loop's end

struct Synth {
	float attackSeconds = 0.001f;
	float decaySeconds = 0.15f;
	float noise = 0.0f;  // Background noise level, relative to full scale
	float drone = 0.0f;  // A constant tone under everything
	float tailLevel = 0; // Tail from the previous pass, wrapping around the loop
};

/// A loop of notes as some gear would play them, arriving `latency` samples late, plus maxLag past the end
std::vector<int32_t> render(std::vector<uint32_t> const& starts, int32_t loopSamples, int32_t latency, Synth s,
                            uint32_t seed = 1) {
	std::vector<float> out(loopSamples + kMaxLag, 0.f);
	std::mt19937 rng(seed);
	std::normal_distribution<float> gauss(0.f, 1.f);
	for (size_t i = 0; i < out.size(); i++) {
		out[i] += s.noise * gauss(rng) + s.drone * std::sin(2 * M_PI * 110 * i / kRate);
	}
	// Each note, and its repeat a loop earlier and later, so the loop's ends carry tails like a real recording
	for (int32_t repeat = -1; repeat <= 1; repeat++) {
		for (size_t n = 0; n < starts.size(); n++) {
			int64_t at = (int64_t)starts[n] + latency + (int64_t)repeat * loopSamples;
			float freq = 220.f * (1 + n % 5);
			for (int64_t i = 0; i < kRate; i++) {
				int64_t pos = at + i;
				if (pos < 0) {
					continue;
				}
				if (pos >= (int64_t)out.size()) {
					break;
				}
				float t = (float)i / kRate;
				float env =
				    (t < s.attackSeconds) ? t / s.attackSeconds : std::exp(-(t - s.attackSeconds) / s.decaySeconds);
				out[pos] += 0.3f * env * std::sin(2 * M_PI * freq * t);
			}
		}
	}
	std::vector<int32_t> pcm(out.size());
	for (size_t i = 0; i < out.size(); i++) {
		pcm[i] = (int32_t)(std::clamp(out[i], -1.f, 1.f) * 2147483000.f);
	}
	return pcm;
}

LatencyEstimate measure(std::vector<int32_t> const& pcm, std::vector<uint32_t> const& starts) {
	std::vector<uint32_t> levels(pcm.size() / kLatencyBlockSize);
	size_t n = blockLevels(pcm, levels);
	return estimateLatency(std::span<uint32_t const>(levels.data(), n), starts, kMaxLag);
}

/// 16th notes at 120 BPM for one bar, with a few gaps
std::vector<uint32_t> sixteenths(int32_t loopSamples) {
	std::vector<uint32_t> starts;
	for (int32_t i = 0; i < 16; i++) {
		if (i % 5 != 3) {
			starts.push_back(loopSamples * i / 16);
		}
	}
	return starts;
}

} // namespace

TEST_GROUP(FreezeLatencyTests){};

// Percussive notes: the latency is found to within a block, whatever it is
TEST(FreezeLatencyTests, findsLatencyOfSharpAttacks) {
	constexpr int32_t loop = 2 * kRate;
	auto starts = sixteenths(loop);
	for (float attack : {0.001f, 0.003f}) {
		for (int32_t latency : {0, 5, 100, 441, 777, 1500, 1900}) {
			Synth s;
			s.attackSeconds = attack;
			LatencyEstimate e = measure(render(starts, loop, latency, s), starts);
			CHECK(e.confident);
			CHECK(std::abs(e.samples - latency) <= 2 * kLatencyBlockSize);
		}
	}
}

// With noise under it as well
TEST(FreezeLatencyTests, findsLatencyThroughNoise) {
	constexpr int32_t loop = 2 * kRate;
	auto starts = sixteenths(loop);
	Synth s;
	s.noise = 0.01f;
	LatencyEstimate e = measure(render(starts, loop, 600, s), starts);
	CHECK(e.confident);
	CHECK(std::abs(e.samples - 600) <= kLatencyBlockSize);
}

// Slower attacks put the steepest rise later, and their start is hidden under what's still sounding. A 5 ms attack is
// still found within itself; slower ones may not be found confidently, but if they are, it's within the attack too
TEST(FreezeLatencyTests, slowAttacksAreFoundWithinTheAttackOrNotAtAll) {
	constexpr int32_t loop = 2 * kRate;
	auto starts = sixteenths(loop);
	for (float attack : {0.005f, 0.01f, 0.02f}) {
		int32_t attackSamples = attack * kRate;
		for (int32_t latency : {0, 300, 800}) {
			Synth s;
			s.attackSeconds = attack;
			LatencyEstimate e = measure(render(starts, loop, latency, s), starts);
			if (attack <= 0.005f) {
				CHECK(e.confident);
			}
			if (e.confident) {
				CHECK(e.samples >= latency - kLatencyBlockSize);
				CHECK(e.samples <= latency + attackSamples);
			}
		}
	}
}

// A few long notes are enough
TEST(FreezeLatencyTests, worksWithFewNotes) {
	constexpr int32_t loop = 4 * kRate;
	std::vector<uint32_t> starts{0, (uint32_t)loop / 2};
	LatencyEstimate e = measure(render(starts, loop, 900, {}), starts);
	CHECK(e.confident);
	CHECK(std::abs(e.samples - 900) <= kLatencyBlockSize);
}

// A tone whose period divides the notes' spacing ripples in the same place at every note: not an attack
TEST(FreezeLatencyTests, notConfidentFromASteadyTone) {
	constexpr int32_t loop = 2 * kRate;
	std::vector<uint32_t> starts{0, loop / 4, loop / 2, 3 * loop / 4};
	for (float freq : {55.f, 110.f, 440.f}) {
		std::vector<int32_t> pcm(loop + kMaxLag);
		for (size_t i = 0; i < pcm.size(); i++) {
			pcm[i] = (int32_t)(1e9 * std::sin(2 * M_PI * freq * i / kRate));
		}
		CHECK_FALSE(measure(pcm, starts).confident);
	}
}

// A drone with nothing tied to the notes, or silence, gives nothing to trust
TEST(FreezeLatencyTests, notConfidentWithoutAttacks) {
	constexpr int32_t loop = 2 * kRate;
	auto starts = sixteenths(loop);
	Synth s;
	s.noise = 0.05f;
	s.drone = 0.3f;
	std::vector<uint32_t> noNotesSounding;
	// Render with no notes, then measure against the clip's notes
	auto pcm = render(noNotesSounding, loop, 0, s);
	CHECK_FALSE(measure(pcm, starts).confident);

	std::vector<int32_t> silence(loop + kMaxLag, 0);
	CHECK_FALSE(measure(silence, starts).confident);
}

// A drone with the odd click or dropout in it: one jump can land on a note, but the other notes don't agree
TEST(FreezeLatencyTests, notConfidentFromStrayClicks) {
	constexpr int32_t loop = 2 * kRate;
	std::vector<uint32_t> starts{0, loop / 4, loop / 2, 3 * loop / 4};
	Synth s;
	s.drone = 0.3f;
	for (uint32_t seed = 1; seed <= 20; seed++) {
		std::vector<uint32_t> noNotesSounding;
		auto pcm = render(noNotesSounding, loop, 0, s, seed);
		// A few dropouts and clicks, one right where a note would be heard
		std::mt19937 rng(seed);
		for (int32_t k = 0; k < 6; k++) {
			size_t at = (k == 0) ? starts[seed % 4] + 300 : rng() % (pcm.size() - 400);
			for (size_t i = at; i < at + 200; i++) {
				pcm[i] = (k % 2) ? 0 : pcm[i] * 3;
			}
		}
		CHECK_FALSE(measure(pcm, starts).confident);
	}
}

// Notes of different loudness all count
TEST(FreezeLatencyTests, quietNotesStillAgree) {
	constexpr int32_t loop = 2 * kRate;
	auto starts = sixteenths(loop);
	auto loud = render(starts, loop, 500, {});
	LatencyEstimate e = measure(loud, starts);
	CHECK(e.confident);
	CHECK(std::abs(e.samples - 500) <= 2 * kLatencyBlockSize);
}

// No notes, or nothing to search, can't be measured
TEST(FreezeLatencyTests, degenerateInputs) {
	std::vector<uint32_t> levels(100, 5);
	std::vector<uint32_t> none;
	std::vector<uint32_t> one{0};
	CHECK_FALSE(estimateLatency(levels, none, kMaxLag).confident);
	CHECK_FALSE(estimateLatency(levels, one, 0).confident);
	CHECK_FALSE(estimateLatency(std::span<uint32_t const>(), one, kMaxLag).confident);
}
