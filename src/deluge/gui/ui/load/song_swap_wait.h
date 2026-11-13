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

namespace deluge::gui {

/// Where LoadSongUI::performLoad() is while a song loaded during playback waits to be swapped in.
enum class SongSwapWait {
	KEEP_WAITING, ///< The swap is armed (or waits for LOAD to be released) and hasn't happened yet
	SWAPPED,      ///< The new song is the current one: the old one can be deleted
	SWAP_NOW,     ///< Something other than the swap ended the wait: swap now, before deleting the old song
};

/// @param swapDone whether the song swap has happened (preLoadedSong has been swapped in)
/// @param uiWaitingForSwap whether currentUIMode is still one of the "waiting for the song swap" modes
constexpr SongSwapWait songSwapWait(bool swapDone, bool uiWaitingForSwap) {
	if (swapDone) {
		return SongSwapWait::SWAPPED;
	}
	// The UI mode only leaves the waiting modes because of the swap. If input changed it anyway, carrying on
	// would delete the old song while it is still the current one (and still being rendered).
	return uiWaitingForSwap ? SongSwapWait::KEEP_WAITING : SongSwapWait::SWAP_NOW;
}

} // namespace deluge::gui
