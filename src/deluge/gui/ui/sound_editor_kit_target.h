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

#include "definitions_cxx.hpp"
#include <optional>

namespace deluge::gui {

/// What the sound editor edits when it is set up for a kit clip.
///
/// SoundEditor::setup() picks both the ParamManager and the root menu from this one value, so the menu tree always
/// matches the parameters being edited. (A drum's sound menus read patched params, which the kit's own ParamManager
/// does not have.)
enum class KitEditTarget {
	KIT_GLOBAL_FX, ///< The whole kit (affect entire): the clip's ParamManager, kit FX menu
	SOUND_DRUM,    ///< The selected sound drum: its note row's ParamManager, drum sound menu
	MIDI_DRUM,     ///< The selected MIDI drum
	GATE_DRUM,     ///< The selected gate drum
	NONE,          ///< No drum selected and not editing the whole kit: nothing to edit
};

/// @param editKitGlobalFX whether this editor session edits the whole kit (SoundEditor::setupKitGlobalFXMenu)
/// @param selectedDrumType the type of the kit's selected drum, or empty if no drum is selected
constexpr KitEditTarget kitEditTarget(bool editKitGlobalFX, std::optional<DrumType> selectedDrumType) {
	if (editKitGlobalFX) {
		return KitEditTarget::KIT_GLOBAL_FX;
	}
	if (!selectedDrumType.has_value()) {
		return KitEditTarget::NONE;
	}
	switch (*selectedDrumType) {
	case DrumType::SOUND:
		return KitEditTarget::SOUND_DRUM;
	case DrumType::MIDI:
		return KitEditTarget::MIDI_DRUM;
	case DrumType::GATE:
		return KitEditTarget::GATE_DRUM;
	}
	return KitEditTarget::NONE;
}

} // namespace deluge::gui
