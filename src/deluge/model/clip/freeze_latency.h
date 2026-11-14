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

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <span>

/// Measuring the round-trip latency of a freeze: from the Deluge sending a MIDI note, through the gear, to its sound
/// arriving back at the input. A frozen loop recorded without allowing for it plays late by that much.
///
/// It's measured on the recording itself, by lining the clip's note starts up with where the recording gets suddenly
/// louder. The recording is reduced to one level per block of samples (blockLevels()); the onset strength of a block is
/// how much louder it is than the one before. For each lag being tried, the onset strengths at every note start plus
/// that lag are added up. The lag with the highest total is where the notes' attacks land. Attacks that rise over a few
/// blocks put the steepest rise a little after the real start, so the estimate is the earliest lag, working back from
/// the best one, that still scores at least a quarter as well. Sharp attacks come out within a block or so; slow ones
/// (tens of ms) a little late, as their start is hidden under whatever's still sounding, or not confidently at all.
///
/// Only lags from 0 up are tried. To allow for audio arriving early too, start the levels that much before the clip's
/// position 0, add that to the note starts, and take it off the result.
namespace deluge::freeze {

/// Block length for the levels the estimate works on: 32 samples is 0.73 ms at 44.1 kHz
constexpr int32_t kLatencyBlockSize = 32;

struct LatencyEstimate {
	int32_t samples = 0;    ///< The latency, a multiple of kLatencyBlockSize
	bool confident = false; ///< Whether the notes' attacks stood out clearly enough to trust it
};

/// How fast the level falls between peaks: by 1/2^this of itself per sample, a time constant of 46 ms. Slow enough
/// that the cycles of even a low note barely ripple it, fast enough to let the next note stand out
constexpr int32_t kLevelDecayShift = 11;

/// Level of mono audio at the end of each kLatencyBlockSize-sample block: an envelope that jumps straight up to each
/// new peak and falls away slowly. levels must have room for audio.size() / kLatencyBlockSize entries; returns how many
/// it wrote. Audio can be fed in pieces of whole blocks, passing the same envelope each time.
inline size_t blockLevels(std::span<int32_t const> audio, std::span<uint32_t> levels, uint32_t& envelope) {
	size_t numBlocks = std::min<size_t>(audio.size() / kLatencyBlockSize, levels.size());
	for (size_t b = 0; b < numBlocks; b++) {
		for (int32_t i = 0; i < kLatencyBlockSize; i++) {
			int32_t value = audio[b * kLatencyBlockSize + i];
			uint32_t magnitude = (value < 0) ? 0u - static_cast<uint32_t>(value) : static_cast<uint32_t>(value);
			envelope -= envelope >> kLevelDecayShift;
			envelope = std::max(envelope, magnitude);
		}
		levels[b] = envelope;
	}
	return numBlocks;
}

inline size_t blockLevels(std::span<int32_t const> audio, std::span<uint32_t> levels) {
	uint32_t envelope = 0;
	return blockLevels(audio, levels, envelope);
}

/// levels: block levels of the recording, starting at the point which should line up with position 0 of the clip, and
/// extending at least maxLagSamples past the loop's end. noteStarts: sample positions of the clip's notes, from 0.
inline LatencyEstimate estimateLatency(std::span<uint32_t const> levels, std::span<uint32_t const> noteStarts,
                                       int32_t maxLagSamples) {
	LatencyEstimate result;
	int32_t maxLagBlocks = maxLagSamples / kLatencyBlockSize;
	if (levels.size() < 2 || noteStarts.empty() || maxLagBlocks < 1) {
		return result;
	}

	auto onset = [&](size_t block) -> uint64_t {
		if (block == 0 || block >= levels.size()) {
			return 0;
		}
		return (levels[block] > levels[block - 1]) ? levels[block] - levels[block - 1] : 0;
	};

	auto score = [&](int32_t lagBlocks) -> uint64_t {
		uint64_t total = 0;
		for (uint32_t start : noteStarts) {
			total += onset(start / kLatencyBlockSize + lagBlocks);
		}
		return total;
	};

	int32_t bestLag = 0;
	uint64_t bestScore = 0;
	uint64_t sumScores = 0;
	for (int32_t lag = 0; lag <= maxLagBlocks; lag++) {
		uint64_t thisScore = score(lag);
		sumScores += thisScore;
		if (thisScore > bestScore) {
			bestScore = thisScore;
			bestLag = lag;
		}
	}
	if (bestScore == 0) {
		return result;
	}

	// Back to where the attacks begin
	int32_t lag = bestLag;
	while (lag > 0 && score(lag - 1) * 4 >= bestScore) {
		lag--;
	}
	result.samples = lag * kLatencyBlockSize;

	// Trusted if most notes have an attack there: their level rises by at least 30% over the attack (from a little
	// before where it was found to begin, to its steepest point), and rises over the attack at least 3 times as fast as
	// it does on average across all the lags tried. Pads, drones, noise and the odd click or dropout don't do that at
	// most notes, even when one big jump lands on a note and wins the total. Nor does a steady tone whose period
	// divides the notes' spacing, though it ripples in the same place at each note
	auto level = [&](size_t block) -> uint64_t { return (block < levels.size()) ? levels[block] : 0; };
	size_t agreeing = 0;
	for (uint32_t start : noteStarts) {
		size_t block = start / kLatencyBlockSize;
		uint64_t sum = 0;
		for (int32_t l = 0; l <= maxLagBlocks; l++) {
			sum += onset(block + l);
		}
		// Over the attack, from where it was found to begin to its steepest point
		uint64_t attackSum = 0;
		for (int32_t l = lag; l <= bestLag; l++) {
			attackSum += onset(block + l);
		}
		uint64_t attackBlocks = bestLag - lag + 1;
		size_t attackStart = block + lag;
		uint64_t before = level(attackStart >= 2 ? attackStart - 2 : 0);
		uint64_t peak = level(block + bestLag);
		if (attackSum * (maxLagBlocks + 1) >= 3 * sum * attackBlocks && peak * 10 >= before * 13) {
			agreeing++;
		}
	}
	uint64_t meanScore = sumScores / (maxLagBlocks + 1);
	result.confident = bestScore >= 4 * meanScore && agreeing * 2 >= noteStarts.size();
	return result;
}

} // namespace deluge::freeze
