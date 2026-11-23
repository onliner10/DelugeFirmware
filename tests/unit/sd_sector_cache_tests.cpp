#include "CppUTest/TestHarness.h"
#include "RZA1/sd_sector_cache.h"
#include <cstdint>
#include <cstring>
#include <functional>
#include <random>
#include <vector>

namespace {

constexpr uint32_t kSectorSize = SD_SECTOR_CACHE_SECTOR_SIZE;
constexpr uint32_t kBlockSectors = SD_SECTOR_CACHE_BLOCK_SECTORS;

/// A card: an array of sectors. Reads past the end fail, like the real driver.
struct FakeCard {
	std::vector<uint8_t> data;
	int reads = 0;
	int sectorsRead = 0;
	bool failNextRead = false;
	/// Runs in the middle of a read, after the data's been copied - e.g. to pull the card out
	std::function<void()> duringRead;

	explicit FakeCard(uint32_t numSectors, uint8_t seed = 0) : data(numSectors * kSectorSize) { fill(seed); }

	void fill(uint8_t seed) {
		for (size_t i = 0; i < data.size(); i++) {
			data[i] = static_cast<uint8_t>(i * 31 + i / kSectorSize * 7 + seed);
		}
	}
	uint32_t numSectors() const { return data.size() / kSectorSize; }

	static int read(uint8_t* buff, uint32_t sector, uint32_t count, void* context) {
		auto* card = static_cast<FakeCard*>(context);
		card->reads++;
		if (card->failNextRead) {
			card->failNextRead = false;
			return 1;
		}
		if (sector + count > card->numSectors()) {
			return 1;
		}
		card->sectorsRead += count;
		memcpy(buff, &card->data[sector * kSectorSize], count * kSectorSize);
		if (card->duringRead) {
			auto f = card->duringRead;
			card->duringRead = nullptr;
			f();
		}
		return 0;
	}

	/// Writes to the card and tells the cache, as disk_write() does
	void write(uint32_t sector, const std::vector<uint8_t>& buff, bool success = true) {
		if (success) {
			memcpy(&data[sector * kSectorSize], buff.data(), buff.size());
		}
		sd_sector_cache_wrote(buff.data(), sector, buff.size() / kSectorSize, success);
	}

	/// Reads a sector through the cache, and checks it matches the card
	void readAndCheck(uint32_t sector) {
		uint8_t buff[kSectorSize];
		CHECK_EQUAL(0, sd_sector_cache_read(buff, sector, read, this));
		MEMCMP_EQUAL(&data[sector * kSectorSize], buff, kSectorSize);
	}
};

std::vector<uint8_t> pattern(uint32_t numSectors, uint8_t value) {
	return std::vector<uint8_t>(numSectors * kSectorSize, value);
}

} // namespace

TEST_GROUP(SdSectorCache){void setup() override{sd_sector_cache_invalidate_all();
}
}
;

TEST(SdSectorCache, nearbySectorsComeFromOneRead) {
	FakeCard card(1024);
	for (uint32_t s = 32; s < 32 + kBlockSectors; s++) {
		card.readAndCheck(s);
	}
	CHECK_EQUAL(1, card.reads);
	CHECK_EQUAL(kBlockSectors, card.sectorsRead);
}

TEST(SdSectorCache, writeUpdatesCachedCopy) {
	FakeCard card(1024);
	card.readAndCheck(40);
	card.write(38, pattern(4, 0xAB)); // Overlaps the cached block
	int readsBefore = card.reads;
	for (uint32_t s = 32; s < 48; s++) {
		card.readAndCheck(s);
	}
	CHECK_EQUAL(readsBefore, card.reads);
}

TEST(SdSectorCache, writeStraddlingBlocksUpdatesBoth) {
	FakeCard card(1024);
	card.readAndCheck(15);
	card.readAndCheck(16);
	card.write(14, pattern(4, 0x5A));
	for (uint32_t s = 0; s < 32; s++) {
		card.readAndCheck(s);
	}
	CHECK_EQUAL(2, card.reads);
}

TEST(SdSectorCache, failedWriteForgetsEverything) {
	FakeCard card(1024);
	card.readAndCheck(40);
	card.write(40, pattern(1, 0x11), false);
	card.readAndCheck(40);
	CHECK_EQUAL(2, card.reads);
}

TEST(SdSectorCache, cardSwapSeesNewCard) {
	FakeCard card(1024);
	card.readAndCheck(100);
	// Different card, same sector numbers: eject + insert
	card.fill(99);
	sd_sector_cache_invalidate_all();
	card.readAndCheck(100);
	card.readAndCheck(101);
	// And again, several times
	for (uint8_t seed = 1; seed < 5; seed++) {
		card.fill(seed);
		sd_sector_cache_invalidate_all();
		card.readAndCheck(100);
	}
}

TEST(SdSectorCache, cardChangedDuringFillIsNotKept) {
	FakeCard card(1024);
	// The card's pulled and another put in while the block is being read
	card.duringRead = [&] {
		card.fill(42);
		sd_sector_cache_invalidate_all();
	};
	uint8_t buff[kSectorSize];
	CHECK_EQUAL(0, sd_sector_cache_read(buff, 50, FakeCard::read, &card));
	// Next read must go to the (new) card, not use what came off the old one
	card.readAndCheck(50);
	card.readAndCheck(51);
	CHECK_EQUAL(2, card.reads);
}

TEST(SdSectorCache, blockPastEndOfCardFallsBackToOneSector) {
	FakeCard card(100); // Last block runs past the end
	card.readAndCheck(99);
	CHECK_EQUAL(2, card.reads); // Failed block fill, then just the sector
	card.readAndCheck(98);
}

TEST(SdSectorCache, failedFillRetriesJustTheSector) {
	FakeCard card(1024);
	card.failNextRead = true;
	card.readAndCheck(5);
	CHECK_EQUAL(2, card.reads);
}

TEST(SdSectorCache, readErrorIsReturned) {
	FakeCard card(1024);
	uint8_t buff[kSectorSize];
	CHECK(sd_sector_cache_read(buff, 5000, FakeCard::read, &card) != 0);
}

TEST(SdSectorCache, leastRecentlyUsedBlockIsEvicted) {
	FakeCard card(4096);
	for (uint32_t b = 0; b < SD_SECTOR_CACHE_NUM_BLOCKS; b++) {
		card.readAndCheck(b * kBlockSectors);
	}
	card.readAndCheck(0);                                          // Block 0 now most recent; block 1 is the oldest
	card.readAndCheck(SD_SECTOR_CACHE_NUM_BLOCKS * kBlockSectors); // Evicts block 1
	int reads = card.reads;
	card.readAndCheck(0);
	CHECK_EQUAL(reads, card.reads);
	card.readAndCheck(1 * kBlockSectors);
	CHECK_EQUAL(reads + 1, card.reads);
}

// Random reads, writes (some failing), card swaps (some mid-read): every read must match the card.
TEST(SdSectorCache, randomisedAgainstCard) {
	std::mt19937 rng(1234);
	FakeCard card(600);
	auto sector = [&] { return static_cast<uint32_t>(rng() % card.numSectors()); };
	for (int i = 0; i < 200000; i++) {
		int op = rng() % 100;
		if (op < 70) {
			// Bias towards a few hot areas, like directories and the FAT
			uint32_t s = (rng() % 4) ? (rng() % 64) : sector();
			card.readAndCheck(s);
		}
		else if (op < 90) {
			uint32_t n = 1 + rng() % 40;
			uint32_t s = sector() % (card.numSectors() - n);
			card.write(s, pattern(n, static_cast<uint8_t>(rng())), rng() % 20 != 0);
		}
		else if (op < 95) {
			card.fill(static_cast<uint8_t>(rng()));
			sd_sector_cache_invalidate_all();
		}
		else {
			uint8_t seed = static_cast<uint8_t>(rng());
			card.duringRead = [&card, seed] {
				card.fill(seed);
				sd_sector_cache_invalidate_all();
			};
			uint8_t buff[kSectorSize];
			sd_sector_cache_read(buff, rng() % 64, FakeCard::read, &card);
			card.duringRead = nullptr;
		}
	}
}
