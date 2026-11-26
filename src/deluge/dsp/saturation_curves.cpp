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

#include "dsp/saturation_curves.h"
#include "definitions_cxx.hpp"
#include "util/lookuptables/lookuptables.h"
#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>

namespace deluge::dsp {

namespace {

// Each curve as c(v) with slope 1 at 0 and a ceiling of 1, for v >= 0 (all are odd). The tables use c(4u) / tanh(4),
// tanh's own scaling, so every type matches CLASSIC's small-signal gain and ceiling.
double curvePositive(SaturationType type, double v) {
	switch (type) {
	case SaturationType::CLASSIC:
		return std::tanh(v);
	case SaturationType::SOFT:
		return (v < M_PI / 2) ? std::sin(v) : 1.0;
	case SaturationType::KNEE: {
		constexpr double kKnee = 0.7;
		return (v <= kKnee) ? v : kKnee + (1 - kKnee) * std::tanh((v - kKnee) / (1 - kKnee));
	}
	case SaturationType::ROUND:
		return v / std::pow(1 + std::pow(v, 2.5), 1 / 2.5);
	case SaturationType::HARD:
		return v / std::pow(1 + std::pow(v, 8.0), 1 / 8.0);
	case SaturationType::CLIP:
		return std::min(v, 1.0);
	case SaturationType::BASS:
		return v / std::sqrt(1 + v * v);
	case SaturationType::FOLD:
		return std::sin(v);
	case SaturationType::CRUNCH: {
		double y = std::min(v / 8, 1.0);
		for (int32_t i = 0; i < 3; i++) {
			y = y * (2 - y);
		}
		return y;
	}
	}
	return std::tanh(v);
}

double curve(SaturationType type, double u) {
	static const double kNormalise = 1 / std::tanh(4.0);
	double v = 4 * std::abs(u);
	double y = std::clamp(curvePositive(type, v) * kNormalise, -1.0, 1.0);
	return (u < 0) ? -y : y;
}

// Fine grid for the antiderivative. 2048 steps over -1 .. 1 puts every table column (128 steps) and row (64 steps)
// exactly on a grid point.
constexpr int32_t kGridSteps = 2048;
constexpr int32_t kColumnStride = kGridSteps / (kSaturationTableColumns - 1);
constexpr int32_t kRowStride = kGridSteps / (kSaturationTableRows - 1);

using Table = std::array<std::array<int16_t, kSaturationTableColumns>, kSaturationTableRows>;
PLACE_SDRAM_BSS Table generatedTables[kNumSaturationTypes - 1];
PLACE_SDRAM_BSS double antiderivative[kGridSteps + 1];
bool tablesGenerated = false;

void generate(SaturationType type, Table& table) {
	auto u = [](int32_t i) { return -1.0 + 2.0 * i / kGridSteps; };
	constexpr double kStep = 2.0 / kGridSteps;
	antiderivative[0] = 0;
	double previous = curve(type, u(0));
	for (int32_t i = 1; i <= kGridSteps; i++) {
		double value = curve(type, u(i));
		antiderivative[i] = antiderivative[i - 1] + 0.5 * (previous + value) * kStep;
		previous = value;
	}
	for (int32_t row = 0; row < kSaturationTableRows; row++) {
		int32_t yi = row * kRowStride;
		for (int32_t column = 0; column < kSaturationTableColumns; column++) {
			int32_t xi = column * kColumnStride;
			// The curve averaged between the previous sample and this one; where they are the same, the curve itself
			double average =
			    (xi == yi) ? curve(type, u(xi)) : (antiderivative[xi] - antiderivative[yi]) / (u(xi) - u(yi));
			table[row][column] = static_cast<int16_t>(std::lround(std::clamp(average, -1.0, 1.0) * 32767));
		}
	}
}

constexpr std::array<char const*, kNumSaturationTypes> kNames{"classic", "soft", "knee", "round", "hard",
                                                              "clip",    "bass", "fold", "crunch"};

} // namespace

float saturationCurve(SaturationType type, float u) {
	return static_cast<float>(curve(type, u));
}

void generateSaturationTables() {
	if (tablesGenerated) {
		return;
	}
	for (int32_t t = 1; t < kNumSaturationTypes; t++) {
		generate(static_cast<SaturationType>(t), generatedTables[t - 1]);
	}
	tablesGenerated = true;
}

int16_t const* saturationTable(SaturationType type) {
	auto index = static_cast<int32_t>(type);
	if (index <= 0 || index >= kNumSaturationTypes) {
		return &tanH2d[0][0];
	}
	if (!tablesGenerated) [[unlikely]] {
		generateSaturationTables();
	}
	return generatedTables[index - 1][0].data();
}

char const* saturationTypeToString(SaturationType type) {
	auto index = static_cast<size_t>(type);
	return index < kNames.size() ? kNames[index] : kNames[0];
}

SaturationType stringToSaturationType(char const* string) {
	for (size_t i = 0; i < kNames.size(); i++) {
		if (!strcmp(string, kNames[i])) {
			return static_cast<SaturationType>(i);
		}
	}
	return SaturationType::CLASSIC;
}

} // namespace deluge::dsp
