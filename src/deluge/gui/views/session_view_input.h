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

namespace deluge::gui::views {

/// Whether pressing SAVE in song view should delete the clip under the held pad.
///
/// SAVE deletes a clip while its pad is held: in Rows layout that is the "clip pressed" UI mode, in Grid layout the
/// first grid press is still being tracked. Never while song view is zooming into or out of a clip: that clip is then
/// the one being transitioned into, and deleting it leaves the finished animation opening a deleted clip.
constexpr bool saveDeletesHeldClip(bool clipPressedInSongView, bool gridFirstPadHeld, bool inClipTransitionAnimation) {
	return (clipPressedInSongView || gridFirstPadHeld) && !inClipTransitionAnimation;
}

} // namespace deluge::gui::views
