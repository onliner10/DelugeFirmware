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

#include <bitset>
#include <cstddef>
#include <cstdint>
#include <span>

/// Which of the first N zones of a multisample play any of these notes, given zoneOf(note), the zone that plays a
/// note. Zones from N on are left out. Working this out once is quicker than asking, for each zone, whether any of the
/// notes falls in it.
template <size_t N, typename ZoneOf>
std::bitset<N> zonesPlayingNotes(std::span<const int16_t> notes, ZoneOf zoneOf) {
	std::bitset<N> zones;
	for (int16_t note : notes) {
		int32_t zone = zoneOf(note);
		if (zone >= 0 && static_cast<size_t>(zone) < N) {
			zones.set(zone);
		}
	}
	return zones;
}
