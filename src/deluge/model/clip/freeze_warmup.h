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

#include <cstdint>

namespace deluge::freeze {

/// Freezing records one loop of a clip. For that loop to repeat seamlessly, whatever the external gear does over the
/// loop boundary - release tails, delay repeats, reverb - must already be in its steady state when recording starts,
/// so the clip plays for a while first. This long at least, covering a slow reverb or a long delay feedback chain
constexpr uint64_t kMinWarmupSamples = 5 * 44100;

/// But never more passes than this, however short the loop
constexpr int32_t kMaxWarmupPasses = 8;

/// How many loop ends to let go by before recording starts at the next one: enough whole passes to cover
/// kMinWarmupSamples, and at least one. The first loop end may only end a partial pass, if the clip was already playing
/// when freezing started - but then it's been sounding for longer than that anyway.
constexpr int32_t warmupPasses(uint64_t loopLengthInSamples) {
	if (loopLengthInSamples == 0) {
		return kMaxWarmupPasses;
	}
	uint64_t passes = (kMinWarmupSamples + loopLengthInSamples - 1) / loopLengthInSamples;
	if (passes < 1) {
		passes = 1;
	}
	if (passes > static_cast<uint64_t>(kMaxWarmupPasses)) {
		passes = kMaxWarmupPasses;
	}
	return static_cast<int32_t>(passes);
}

} // namespace deluge::freeze
