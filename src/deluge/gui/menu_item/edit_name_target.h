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

namespace deluge::gui::menu_item {

/// What an EditName menu item renames.
enum class EditNameTarget {
	CLIP,
	DRUM,
	AUDIO_OUTPUT,
};

/// Whether an EditName item has something to rename. Renaming a drum needs a selected drum: RenameDrumUI freezes
/// (RN01) when the kit has none, so the menu item must not be offered or opened then.
constexpr bool editNameTargetAvailable(EditNameTarget target, bool kitHasSelectedDrum) {
	return target != EditNameTarget::DRUM || kitHasSelectedDrum;
}

} // namespace deluge::gui::menu_item
