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
#include "gui/colour/rgb.h"
#include "model/sync.h"
#include "modulation/arpeggiator.h"
#include "modulation/params/param.h"
#include "modulation/patch/patch_cable.h"
#include <cstdint>

class InstrumentClip;

/// Preset lock: modules of a synth clip that survive changing its preset.
///
/// Each module groups the settings that belong to the clip's context in the song rather than to the sound itself
/// (arpeggiator, level/pan, sidechain, reverb send, delay). The locked modules are stored per clip as a bitmask; when
/// the clip's preset changes, the locked modules are restored from a snapshot taken before the change.
namespace deluge::preset_lock {

namespace params = deluge::modulation::params;

enum Module : uint8_t {
	ARP = 1 << 0,
	MIX = 1 << 1,
	SIDECHAIN = 1 << 2,
	REVERB = 1 << 3,
	DELAY = 1 << 4,
};

constexpr uint8_t kAllModules = ARP | MIX | SIDECHAIN | REVERB | DELAY;

constexpr int32_t kNumUnpatchedArpParams = params::UNPATCHED_LAST_ARP_PARAM - params::UNPATCHED_FIRST_ARP_PARAM;

/// The values of every lockable module of one synth clip.
struct Snapshot {
	bool valid = false;

	// Arp
	ArpeggiatorSettings arpSettings;
	int32_t arpRate = 0;
	int32_t arpUnpatched[kNumUnpatchedArpParams] = {};

	// Mix
	int32_t volume = 0;
	int32_t pan = 0;

	// Sidechain
	int32_t sidechainSendLevel = 0;
	int32_t sidechainAttack = 0;
	int32_t sidechainRelease = 0;
	SyncType sidechainSyncType = SYNC_TYPE_EVEN;
	SyncLevel sidechainSyncLevel = SYNC_LEVEL_NONE;
	int32_t sidechainShape = 0;
	bool duckingCableExists = false;
	int32_t duckingCableValue = 0;
	Polarity duckingCablePolarity = Polarity::BIPOLAR;

	// Reverb
	int32_t reverbAmount = 0;

	// Delay
	int32_t delayRate = 0;
	int32_t delayFeedback = 0;
	bool delayPingPong = true;
	bool delayAnalog = false;
	SyncType delaySyncType = SYNC_TYPE_EVEN;
	SyncLevel delaySyncLevel = SYNC_LEVEL_16TH;
};

/// Whether the clip's modules can be locked. Only synth clips are supported for now.
bool canLock(InstrumentClip* clip);

/// Records the current values of every lockable module of the clip. Leaves the snapshot invalid if the clip can't be
/// locked.
void capture(Snapshot& snapshot, InstrumentClip* clip);

/// Writes the given modules from the snapshot back into the clip. Does nothing if the snapshot is invalid or the clip
/// can't be locked.
void apply(Snapshot const& snapshot, uint8_t modules, InstrumentClip* clip);

/// The module whose shortcut pad sits at (x, y) on the synth shortcut grid, or 0 if none.
uint8_t moduleForPad(int32_t x, int32_t y);

/// The pad colour for a module, full brightness when locked and dimmed otherwise.
RGB padColour(uint8_t module, bool locked);

/// A short name for the module, for popups.
char const* moduleName(uint8_t module);

} // namespace deluge::preset_lock
