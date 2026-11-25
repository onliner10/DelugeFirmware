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

#pragma once

#include <stdint.h>

// For building a test image for an emulator that only looks at the DMA's links when the channel starts, so that it has
// the bigger TX ring (and the engine then uses it, whatever the setting). Never for a real build: the engine starts up
// going by a ring of SSI_TX_BUFFER_NUM_SAMPLES
#ifndef SSI_TX_SECOND_HALF_LINKED_FROM_START
#define SSI_TX_SECOND_HALF_LINKED_FROM_START 0
#endif

extern uint32_t ssiDmaTxLinkDescriptor[];
extern const uint32_t ssiDmaRxLinkDescriptor[];

void ssiInit(uint8_t ssiChannel, uint8_t dmaChannel);

void* getTxBufferCurrentPlace();
void* getRxBufferCurrentPlace();
void clearTxBuffer();

int32_t* getTxBufferStart();
int32_t* getTxBufferEnd();

/// How many samples the SSI TX ring holds: what the engine can have queued in front of the codec, and so its output
/// latency. SSI_TX_BUFFER_NUM_SAMPLES, or SSI_TX_BUFFER_MAX_NUM_SAMPLES once the bigger output queue is on (see
/// AudioEngine::applyOutputQueueSetting()). Always a power of 2. Only setTxRingNumSamples() changes it
extern uint32_t ssiTxRingNumSamples;

/// Has the DMA carry on into the second half of the TX buffer when it gets to the end of the first, instead of going
/// back to the start. It only finds out when it next gets to the end of the first half, so the ring is twice the size
/// from some point in the next couple of laps: once it's been seen past getTxBufferSecondHalfStart()
void linkTxBufferSecondHalf(void);

/// Has the DMA go back to the start of the TX buffer from the end of its first half again
void unlinkTxBufferSecondHalf(void);

/// Where the second half of the TX buffer starts, as getTxBufferCurrentPlace() would give it. The DMA only gets past
/// here once linkTxBufferSecondHalf() has taken effect - but its source address is here for a moment each time it
/// finishes the first half, whichever it then goes on to
int32_t* getTxBufferSecondHalfStart(void);

/// Tells everything that goes by ssiTxRingNumSamples (and getTxBufferEnd()) what the DMA is now doing. Only for once
/// the DMA has been seen to be doing it
void setTxRingNumSamples(uint32_t numSamples);
int32_t* getRxBufferStart();
int32_t* getRxBufferEnd();
