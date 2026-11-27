#include "CppUTest/TestHarness.h"
#include "storage/multi_range/zones_playing_notes.h"
#include <algorithm>
#include <random>
#include <vector>

// Source's lazy loading asks, for each zone of a multisample, whether it plays any of some notes. It used to look
// through the notes for each zone, and now works out the zones once. Both must give the same answer for every zone.

namespace {
// As Source::getRangeIndex(): the first zone whose top note is at or above the note, or else the last zone
int32_t zoneOf(std::vector<int32_t> const& topNotes, int32_t note) {
	if (topNotes.size() <= 1) {
		return 0;
	}
	auto e = static_cast<int32_t>(std::lower_bound(topNotes.begin(), topNotes.end(), note) - topNotes.begin());
	return std::min(e, static_cast<int32_t>(topNotes.size()) - 1);
}

// The old Source::rangePlaysAnyOf()
bool zonePlaysAnyOf(std::vector<int32_t> const& topNotes, int32_t e, std::span<const int16_t> notes) {
	for (int16_t note : notes) {
		if (zoneOf(topNotes, note) == e) {
			return true;
		}
	}
	return false;
}
} // namespace

TEST_GROUP(ZonesPlayingNotesTests){};

TEST(ZonesPlayingNotesTests, sameZonesAsAskingEachZone) {
	std::mt19937 rnd(42);
	for (int trial = 0; trial < 3000; trial++) {
		// Up to 300 zones, more than the 256 looked at, and top notes that may repeat
		std::vector<int32_t> topNotes(1 + rnd() % 300);
		for (int32_t& top : topNotes) {
			top = static_cast<int32_t>(rnd() % 160) - 20;
		}
		std::sort(topNotes.begin(), topNotes.end());
		topNotes.back() = (rnd() & 1) ? 32767 : topNotes.back(); // Usually the last zone reaches the top

		std::vector<int16_t> notes(rnd() % 40); // Including none, repeats, negative and above every zone
		for (int16_t& note : notes) {
			note = static_cast<int16_t>(static_cast<int32_t>(rnd() % 220) - 40);
		}

		auto zones = zonesPlayingNotes<256>(notes, [&](int32_t note) { return zoneOf(topNotes, note); });
		for (int32_t e = 0; e < 256; e++) {
			bool expected = e < static_cast<int32_t>(topNotes.size()) && zonePlaysAnyOf(topNotes, e, notes);
			CHECK_EQUAL(expected, static_cast<bool>(zones[e]));
		}
	}
}

TEST(ZonesPlayingNotesTests, zonesPastTheEndAreLeftOut) {
	int16_t notes[] = {0, 5, 300};
	auto zones = zonesPlayingNotes<8>(notes, [](int32_t note) { return note; });
	CHECK_EQUAL(2u, zones.count());
	CHECK(zones[0]);
	CHECK(zones[5]);
}
