/*
 * Copyright © 2026 Synthstrom Audible Limited
 *
 * This file is part of The Synthstrom Audible Deluge Firmware.
 *
 * The Synthstrom Audible Deluge Firmware is free software: you can redistribute it and/or modify it under the
 * terms of the GNU General Public License as published by the Free Software Foundation,
 * either version 3 of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 * without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.
 * See the GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program.
 * If not, see <https://www.gnu.org/licenses/>.
 */

#include "sd_sector_cache.h"
#include <string.h>

#define BLOCK_BYTES (SD_SECTOR_CACHE_BLOCK_SECTORS * SD_SECTOR_CACHE_SECTOR_SIZE)

// Block data lives in SDRAM: it's only touched on a FatFs metadata read, and internal RAM is precious. It's DMA'd
// into, so it's aligned to and sized in whole cache lines (the SD driver does the cache maintenance).
#ifdef IN_UNIT_TESTS
#define CACHE_DATA_PLACEMENT
#else
#define CACHE_DATA_PLACEMENT __attribute__((__section__(".sdram_bss")))
#endif
static uint8_t blockData[SD_SECTOR_CACHE_NUM_BLOCKS][BLOCK_BYTES] CACHE_DATA_PLACEMENT __attribute__((aligned(32)));

typedef struct
{
    uint32_t firstSector;
    uint32_t epoch; ///< Valid only while this equals currentEpoch.
    uint32_t lastUsed;
} Block;

static Block blocks[SD_SECTOR_CACHE_NUM_BLOCKS];

// Bumped (never reset) to invalidate everything, possibly from the card-detect interrupt. Starts at 1 so the
// zero-initialised blocks are invalid. The main thread may also bump it; if the interrupt lands in the middle of
// that, one increment can be lost, but the value still changes, which is all that matters.
static volatile uint32_t currentEpoch = 1;
static uint32_t useCounter            = 0;

static bool blockValid(const Block* b)
{
    return b->epoch == currentEpoch;
}

void sd_sector_cache_invalidate_all(void)
{
    currentEpoch++;
}

int sd_sector_cache_read(uint8_t* buff, uint32_t sector, SdSectorCacheReadFn read, void* context)
{
    uint32_t firstSector = sector - (sector % SD_SECTOR_CACHE_BLOCK_SECTORS);
    uint32_t offset      = (sector - firstSector) * SD_SECTOR_CACHE_SECTOR_SIZE;

    // Hit?
    int victim = 0;
    for (int i = 0; i < SD_SECTOR_CACHE_NUM_BLOCKS; i++)
    {
        Block* b = &blocks[i];
        if (blockValid(b) && b->firstSector == firstSector)
        {
            b->lastUsed = ++useCounter;
            memcpy(buff, blockData[i] + offset, SD_SECTOR_CACHE_SECTOR_SIZE);
            return 0;
        }
        // Prefer an invalid block, else the least recently used one
        Block* v = &blocks[victim];
        if (blockValid(v) && (!blockValid(b) || b->lastUsed < v->lastUsed))
        {
            victim = i;
        }
    }

    // Miss: fill the victim block with one read
    Block* b             = &blocks[victim];
    b->epoch             = 0; // Invalid while being filled
    uint32_t epochBefore = currentEpoch;
    int error            = read(blockData[victim], firstSector, SD_SECTOR_CACHE_BLOCK_SECTORS, context);
    if (error)
    {
        // Perhaps the block runs past the end of the card, or the card's having trouble. Forget everything, to be
        // safe, and just read what was asked for.
        sd_sector_cache_invalidate_all();
        return read(buff, sector, 1, context);
    }
    memcpy(buff, blockData[victim] + offset, SD_SECTOR_CACHE_SECTOR_SIZE);

    // Tagged with the epoch from before the read, so if anything happened to the card while we were reading, the
    // block is already invalid
    b->firstSector = firstSector;
    b->lastUsed    = ++useCounter;
    b->epoch       = epochBefore;
    return 0;
}

void sd_sector_cache_wrote(const uint8_t* buff, uint32_t sector, uint32_t count, bool success)
{
    if (!success)
    {
        // Don't know what's on the card now
        sd_sector_cache_invalidate_all();
        return;
    }

    // Keep any cached copies of the written sectors identical to the card
    for (int i = 0; i < SD_SECTOR_CACHE_NUM_BLOCKS; i++)
    {
        Block* b = &blocks[i];
        if (!blockValid(b))
        {
            continue;
        }
        uint32_t blockEnd = b->firstSector + SD_SECTOR_CACHE_BLOCK_SECTORS;
        uint32_t writeEnd = sector + count;
        uint32_t from     = (sector > b->firstSector) ? sector : b->firstSector;
        uint32_t to       = (writeEnd < blockEnd) ? writeEnd : blockEnd;
        if (from < to)
        {
            memcpy(blockData[i] + (from - b->firstSector) * SD_SECTOR_CACHE_SECTOR_SIZE,
                buff + (from - sector) * SD_SECTOR_CACHE_SECTOR_SIZE, (to - from) * SD_SECTOR_CACHE_SECTOR_SIZE);
        }
    }
}
