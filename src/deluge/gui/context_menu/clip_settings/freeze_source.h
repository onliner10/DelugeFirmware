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
#include "gui/context_menu/context_menu.h"
#include <array>

class InstrumentClip;

namespace deluge::gui::context_menu::clip_settings {

/// Picks the audio input to record a MIDI clip's gear from, then starts freezing it
class FreezeSourceMenu final : public ContextMenu {
public:
	FreezeSourceMenu() = default;
	bool setupAndCheckAvailability() override;
	bool canSeeViewUnderneath() override { return true; }
	bool acceptCurrentOption() override;

	InstrumentClip* clip = nullptr;

	char const* getTitle() override;
	std::span<const char*> getOptions() override;

	/// The inputs offered, in order
	static constexpr std::array<AudioInputChannel, 5> kInputs{
	    AudioInputChannel::STEREO,   AudioInputChannel::LEFT, AudioInputChannel::RIGHT,
	    AudioInputChannel::BALANCED, AudioInputChannel::USB,
	};
};

extern FreezeSourceMenu freezeSource;
} // namespace deluge::gui::context_menu::clip_settings
