// Fuzzes the SD sector cache (src/RZA1/sd_sector_cache.c) against a model card. Whatever the sequence of reads,
// writes (some failing), read errors and card swaps (some in the middle of filling a block), every read must return
// exactly what's on the card. Built with AddressSanitizer and UndefinedBehaviorSanitizer.

#include "RZA1/sd_sector_cache.h"
#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

// Not a multiple of the block size, so the last block runs past the end of the card
#define NUM_SECTORS 200
#define SECTOR_SIZE SD_SECTOR_CACHE_SECTOR_SIZE

static uint8_t card[NUM_SECTORS][SECTOR_SIZE];
static bool failNextRead;
static bool swapDuringNextRead;
static bool swappedDuringRead;
static uint8_t swapSeed;

// Every sector of every card is different: its number, then a fill byte depending on the card
static void insertCard(uint8_t seed) {
	for (int s = 0; s < NUM_SECTORS; s++) {
		memset(card[s], (uint8_t)(s * 7 + seed * 13 + 1), SECTOR_SIZE);
		card[s][0] = (uint8_t)s;
		card[s][1] = seed;
	}
}

static int readCard(uint8_t* buff, uint32_t sector, uint32_t count, void* context) {
	(void)context;
	if (failNextRead) {
		failNextRead = false;
		return 1;
	}
	if (sector >= NUM_SECTORS || count > NUM_SECTORS - sector) {
		return 1; // Like the driver's out-of-range check
	}
	memcpy(buff, card[sector], (size_t)count * SECTOR_SIZE);
	if (swapDuringNextRead) {
		// Another card goes in while this one's being read
		swapDuringNextRead = false;
		swappedDuringRead = true;
		insertCard(swapSeed);
		sd_sector_cache_invalidate_all();
	}
	return 0;
}

typedef struct {
	const uint8_t* data;
	size_t size;
} Input;

static uint8_t next(Input* in) {
	if (!in->size) {
		return 0;
	}
	in->size--;
	return *in->data++;
}

static uint32_t nextSector(Input* in) {
	return ((uint32_t)next(in) << 8 | next(in)) % NUM_SECTORS;
}

int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
	Input in = {data, size};
	insertCard(0);
	sd_sector_cache_invalidate_all();
	failNextRead = false;
	swapDuringNextRead = false;

	while (in.size) {
		switch (next(&in) % 6) {
		case 0:
		case 1: { // Read one sector: it must match the card
			uint32_t sector = nextSector(&in);
			uint8_t buff[SECTOR_SIZE];
			swappedDuringRead = false;
			int error = sd_sector_cache_read(buff, sector, readCard, NULL);
			// If the card was swapped during this very read, the data is from the old card, just as with an uncached
			// read that finished as the card came out. Any later read must see the new card.
			if (!error && !swappedDuringRead && memcmp(buff, card[sector], SECTOR_SIZE) != 0) {
				abort();
			}
			break;
		}
		case 2: { // Write, which sometimes fails
			uint32_t sector = nextSector(&in);
			uint32_t count = 1 + next(&in) % 40;
			if (count > NUM_SECTORS - sector) {
				count = NUM_SECTORS - sector;
			}
			uint8_t value = next(&in);
			bool success = (next(&in) % 8) != 0;
			static uint8_t buff[40 * SECTOR_SIZE];
			memset(buff, value, (size_t)count * SECTOR_SIZE);
			if (success) {
				memcpy(card[sector], buff, (size_t)count * SECTOR_SIZE);
			}
			sd_sector_cache_wrote(buff, sector, count, success);
			break;
		}
		case 3: // Swap the card
			insertCard(next(&in));
			sd_sector_cache_invalidate_all();
			break;
		case 4: // Swap the card in the middle of the next read from it
			swapDuringNextRead = true;
			swapSeed = next(&in);
			break;
		case 5: // Next read from the card fails
			failNextRead = true;
			break;
		}
	}
	return 0;
}
