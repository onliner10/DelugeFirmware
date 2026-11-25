/*
 * Copyright © 2015-2023 Synthstrom Audible Limited
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

#include "drivers/ssi/ssi.h"
#include "RZA1/system/iodefines/dmac_iodefine.h"
#include "RZA1/system/iodefines/ssif_iodefine.h"
#include "definitions.h"

// Room for the biggest ring the DMA can be given - see ssiTxRingNumSamples
int32_t ssiTxBuffer[SSI_TX_BUFFER_MAX_NUM_SAMPLES * NUM_MONO_OUTPUT_CHANNELS] __attribute__((aligned(CACHE_LINE_SIZE)));

PLACE_SDRAM_DATA int32_t ssiRxBuffer[SSI_RX_BUFFER_NUM_SAMPLES * NUM_MONO_INPUT_CHANNELS]
    __attribute__((aligned(CACHE_LINE_SIZE)));

uint32_t ssiTxRingNumSamples = SSI_TX_BUFFER_NUM_SAMPLES;

// One half of the TX buffer: the whole ring normally, or half of it once the second half is linked in
#define SSI_TX_HALF_BUFFER_BYTES (SSI_TX_BUFFER_NUM_SAMPLES * NUM_MONO_OUTPUT_CHANNELS * sizeof(int32_t))

extern const uint32_t ssiDmaTxSecondHalfLinkDescriptor[];

// The DMA reads this each time it gets to the start of the TX buffer. Its last word says where it goes after the first
// half: back to this one normally, which makes that the whole ring. Not const, because linkTxBufferSecondHalf() changes
// that word - through the uncached mirror, as the DMA reads memory, not the cache
uint32_t ssiDmaTxLinkDescriptor[] __attribute__((aligned(CACHE_LINE_SIZE))) = {
    0b1101,                                                                          // Header
    (uint32_t)ssiTxBuffer,                                                           // Source address
    (uint32_t)&SSIF(SSI_CHANNEL).SSIFTDR.LONG,                                       // Destination address
    SSI_TX_HALF_BUFFER_BYTES,                                                        // Transaction size
    0b10000001001000100010001000101000 | DMA_LVL_FOR_SSI | (SSI_TX_DMA_CHANNEL & 7), // Config
    0,                                                                               // Interval
    0,                                                                               // Extension
#if SSI_TX_SECOND_HALF_LINKED_FROM_START
    (uint32_t)ssiDmaTxSecondHalfLinkDescriptor // Next link address
#else
    (uint32_t)ssiDmaTxLinkDescriptor // Next link address (this one again)
#endif
};

// The second half of the TX buffer, which always goes back to the first. The DMA only gets here once the descriptor
// above links to it
const uint32_t ssiDmaTxSecondHalfLinkDescriptor[] __attribute__((aligned(CACHE_LINE_SIZE))) = {
    0b1101,                                                                          // Header
    (uint32_t)&ssiTxBuffer[SSI_TX_BUFFER_NUM_SAMPLES * NUM_MONO_OUTPUT_CHANNELS],    // Source address
    (uint32_t)&SSIF(SSI_CHANNEL).SSIFTDR.LONG,                                       // Destination address
    SSI_TX_HALF_BUFFER_BYTES,                                                        // Transaction size
    0b10000001001000100010001000101000 | DMA_LVL_FOR_SSI | (SSI_TX_DMA_CHANNEL & 7), // Config
    0,                                                                               // Interval
    0,                                                                               // Extension
    (uint32_t)ssiDmaTxLinkDescriptor // Next link address (the first half)
};

const uint32_t ssiDmaRxLinkDescriptor[] __attribute__((aligned(CACHE_LINE_SIZE))) = {
    0b1101,                                                                          // Header
    (uint32_t)&SSIF(SSI_CHANNEL).SSIFRDR.LONG,                                       // Source address
    (uint32_t)ssiRxBuffer,                                                           // Destination address
    sizeof(ssiRxBuffer),                                                             // Transaction size
    0b10000001000100100010001000100000 | DMA_LVL_FOR_SSI | (SSI_RX_DMA_CHANNEL & 7), // Config
    0,                                                                               // Interval
    0,                                                                               // Extension
    (uint32_t)ssiDmaRxLinkDescriptor // Next link address (this one again)
};

void* getTxBufferCurrentPlace() {
	return (void*)((DMACn(SSI_TX_DMA_CHANNEL).CRSA_n & ~((NUM_MONO_OUTPUT_CHANNELS * 4) - 1)) + UNCACHED_MIRROR_OFFSET);
}

void* getRxBufferCurrentPlace() {
	return (void*)((DMACn(SSI_RX_DMA_CHANNEL).CRDA_n & ~((NUM_MONO_INPUT_CHANNELS * 4) - 1)) + UNCACHED_MIRROR_OFFSET);
}

int32_t* getTxBufferStart() {
	return (int32_t*)((uint32_t)&ssiTxBuffer[0] + UNCACHED_MIRROR_OFFSET);
}

int32_t* getTxBufferEnd() {
	return (int32_t*)((uint32_t)&ssiTxBuffer[ssiTxRingNumSamples * NUM_MONO_OUTPUT_CHANNELS] + UNCACHED_MIRROR_OFFSET);
}

static void setTxFirstHalfNextLink(const uint32_t* descriptor) {
	// NOLINTNEXTLINE(performance-no-int-to-ptr)
	volatile uint32_t* next_link = (volatile uint32_t*)((uint32_t)&ssiDmaTxLinkDescriptor[7] + UNCACHED_MIRROR_OFFSET);
	*next_link = (uint32_t)descriptor;
}

void linkTxBufferSecondHalf(void) {
	setTxFirstHalfNextLink(ssiDmaTxSecondHalfLinkDescriptor);
}

void unlinkTxBufferSecondHalf(void) {
	setTxFirstHalfNextLink(ssiDmaTxLinkDescriptor);
}

int32_t* getTxBufferSecondHalfStart(void) {
	return getTxBufferStart() + (SSI_TX_BUFFER_NUM_SAMPLES * NUM_MONO_OUTPUT_CHANNELS);
}

void setTxRingNumSamples(uint32_t numSamples) {
	ssiTxRingNumSamples = numSamples;
}
inline void clearTxBuffer() {
	int output = 0;
	for (int32_t* address = getTxBufferStart(); address < getTxBufferEnd(); address++) {
		*address = output;
		output = !output;
	}
}
int32_t* getRxBufferStart() {
	return (int32_t*)((uint32_t)&ssiRxBuffer[0] + UNCACHED_MIRROR_OFFSET);
}

int32_t* getRxBufferEnd() {
	return (int32_t*)((uint32_t)&ssiRxBuffer[SSI_RX_BUFFER_NUM_SAMPLES * NUM_MONO_INPUT_CHANNELS]
	                  + UNCACHED_MIRROR_OFFSET);
}
