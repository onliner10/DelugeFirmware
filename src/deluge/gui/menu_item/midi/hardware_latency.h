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
#include "gui/menu_item/integer.h"
#include "model/instrument/midi_instrument.h"
#include "model/song/song.h"

extern Output* getCurrentOutput();

namespace deluge::gui::menu_item::midi {
/// The MIDI track's round-trip latency, out to its gear and back in, for freezing its clips to allow for
class HardwareLatency final : public Integer {
public:
	using Integer::Integer;
	// Only shown for MIDI tracks
	void readCurrentValue() override { this->setValue(((MIDIInstrument*)getCurrentOutput())->hardwareLatencyMs); }
	void writeCurrentValue() override { ((MIDIInstrument*)getCurrentOutput())->hardwareLatencyMs = this->getValue(); }
	[[nodiscard]] int32_t getMaxValue() const override { return MIDIInstrument::kMaxHardwareLatencyMs; }
	bool isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const override {
		return getCurrentOutputType() == OutputType::MIDI_OUT;
	}

protected:
	const char* getUnit() override { return "ms"; }
};
} // namespace deluge::gui::menu_item::midi
