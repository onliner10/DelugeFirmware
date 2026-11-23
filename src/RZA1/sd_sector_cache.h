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

#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/// Read cache for single-sector SD reads, sitting under FatFs.
///
/// FatFs reads directories and the FAT one 512-byte sector at a time, and finds every file by walking its path from
/// the root, so loading a preset with many samples issues thousands of tiny reads, each paying the card's access
/// latency. This keeps a few aligned blocks of sectors in RAM, each filled with one multi-sector read.
///
/// The one rule: what's cached always equals what the card would return now. It's a read cache only (writes go
/// straight to the card and update any cached copy), so there is never anything to lose. Anything that might break
/// the rule - the card being removed or inserted, (re)initialisation, or any failed read or write - throws the whole
/// cache away by bumping an epoch. A block whose fill straddled such an event is never kept.

#define SD_SECTOR_CACHE_SECTOR_SIZE   512
#define SD_SECTOR_CACHE_BLOCK_SECTORS 16 ///< Sectors per block, filled by one read. Blocks are aligned to this.
#define SD_SECTOR_CACHE_NUM_BLOCKS    16

/// Reads `count` sectors starting at `sector` from the card into `buff`. Returns 0 on success.
typedef int (*SdSectorCacheReadFn)(uint8_t* buff, uint32_t sector, uint32_t count, void* context);

/// Forget everything. Safe to call from an interrupt.
void sd_sector_cache_invalidate_all(void);

/// Reads one sector, from the cache or else by filling a block through `read`. Returns 0 on success, or whatever
/// `read` returned. If filling a block fails (e.g. near the end of the card), falls back to reading just this sector.
int sd_sector_cache_read(uint8_t* buff, uint32_t sector, SdSectorCacheReadFn read, void* context);

/// Must be called after every write to the card, with whether it succeeded.
void sd_sector_cache_wrote(const uint8_t* buff, uint32_t sector, uint32_t count, bool success);

#ifdef __cplusplus
}
#endif
