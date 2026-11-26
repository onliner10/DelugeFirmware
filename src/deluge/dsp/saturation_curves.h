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

namespace deluge::dsp {

/// The shaping curve of the saturation stage. CLASSIC is the original tanh and is what everything loads as unless a
/// file says otherwise.
enum class SaturationType : uint8_t {
	CLASSIC, ///< tanh
	SOFT,    ///< sine up to its peak: very gentle, rounds off early
	KNEE,    ///< linear up to 70%, then a short rounded knee into a clip
	ROUND,   ///< soft knee, between CLASSIC and HARD
	HARD,    ///< a much sharper knee, still smooth
	CLIP,    ///< straight clip at full scale
	BASS,    ///< x / sqrt(1 + x^2): reaches full scale very slowly, so heavy drive stays smooth on low notes
	FOLD,    ///< a sine all the way: past its peak the signal folds back down
	CRUNCH,  ///< three passes of y (2 - |y|): a long, uneven knee with plenty of upper harmonics
};
constexpr int32_t kNumSaturationTypes = 9;

/// Rows and columns of an antialiased table: tanH2d's layout. Row: the previous sample's working value >> 26 (plus one
/// for interpolation), column: the current one's >> 25 (plus one).
constexpr int32_t kSaturationTableRows = 65;
constexpr int32_t kSaturationTableColumns = 129;

/// The curve on the tables' own scale: input and output -1 .. 1, slope about 4 at 0 (like tanh(4u) / tanh(4)), so every
/// type has the same small-signal gain and ceiling and switching type doesn't change the level.
[[nodiscard]] float saturationCurve(SaturationType type, float u);

/// The antialiased (first-order antiderivative) table for a type, laid out like tanH2d. CLASSIC is tanH2d itself, so it
/// stays bit for bit what it was. The others are built by generateSaturationTables().
[[nodiscard]] int16_t const* saturationTable(SaturationType type);

/// Builds every table except CLASSIC's. Called once at startup; cheap (about 70k entries in all).
void generateSaturationTables();

[[nodiscard]] char const* saturationTypeToString(SaturationType type);
[[nodiscard]] SaturationType stringToSaturationType(char const* string);

} // namespace deluge::dsp
