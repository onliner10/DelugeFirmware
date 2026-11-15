#include "CppUTest/TestHarness.h"
#include "model/clip/freeze_warmup.h"
#include <cstdint>

using deluge::freeze::kMaxWarmupPasses;
using deluge::freeze::kMinWarmupSamples;
using deluge::freeze::warmupPasses;

TEST_GROUP(FreezeWarmupTests){};

// A long loop already covers the warm-up time on its own: one pass, so tails from the previous pass are at the start
TEST(FreezeWarmupTests, longLoopsWarmUpForOnePass) {
	CHECK_EQUAL(1, warmupPasses(kMinWarmupSamples));
	CHECK_EQUAL(1, warmupPasses(kMinWarmupSamples * 10));
	CHECK_EQUAL(1, warmupPasses(UINT64_MAX / 2));
}

// Shorter loops play as many whole passes as it takes to cover the warm-up time
TEST(FreezeWarmupTests, shortLoopsWarmUpForEnoughPasses) {
	constexpr uint64_t oneBarAt120 = 2 * 44100; // 2 seconds
	CHECK_EQUAL(3, warmupPasses(oneBarAt120));  // 6 s >= 5 s
	CHECK_EQUAL(2, warmupPasses(kMinWarmupSamples - 1));
	CHECK_EQUAL(2, warmupPasses(kMinWarmupSamples / 2));
	CHECK_EQUAL(3, warmupPasses(kMinWarmupSamples / 2 - 1));
}

// However short the loop, freezing doesn't sit there for ever
TEST(FreezeWarmupTests, veryShortLoopsAreCapped) {
	CHECK_EQUAL(kMaxWarmupPasses, warmupPasses(1));
	CHECK_EQUAL(kMaxWarmupPasses, warmupPasses(441));
	CHECK_EQUAL(kMaxWarmupPasses, warmupPasses(0));
}

// More loop never means more passes
TEST(FreezeWarmupTests, passesNeverIncreaseWithLength) {
	int32_t previous = warmupPasses(1);
	for (uint64_t length = 2; length < 3 * kMinWarmupSamples; length += 97) {
		int32_t passes = warmupPasses(length);
		CHECK(passes <= previous);
		CHECK(passes >= 1);
		CHECK(static_cast<uint64_t>(passes) * length >= kMinWarmupSamples || passes == kMaxWarmupPasses);
		previous = passes;
	}
}
