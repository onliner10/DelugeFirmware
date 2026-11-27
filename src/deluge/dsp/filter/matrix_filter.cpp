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
#include "dsp/filter/matrix_filter.h"
#include <algorithm>

namespace deluge::dsp::filter {

q31_t MatrixFilter::setConfig(q31_t frequency, q31_t resonance, FilterMode mode, [[maybe_unused]] q31_t morph,
                              q31_t filterGain) {
	// The same cutoff curve as the old ladders: fc = G = g / (1 + g), g = tan(pi f / fs)
	curveFrequency(frequency);
	float const G = std::clamp(static_cast<float>(fc) * (1.f / 2147483648.f), 0.002f, 0.995f);
	float const g = G / (1.f - G);

	// Resonance comes in as 0 to 2^29
	float const r = std::clamp(static_cast<float>(resonance) * (1.f / 536870912.f), 0.f, 1.f);

	MatrixLadder::Topology topology = MatrixLadder::Topology::LP4;
	float k = 0.f;
	switch (mode) {
	case FilterMode::TRANSISTOR_12DB:
		topology = MatrixLadder::Topology::SVF_LP;
		k = 0.7071f * (1.f - 0.97f * r); // damping, Q from 0.707 to ~24
		break;
	case FilterMode::HPLADDER:
		topology = MatrixLadder::Topology::SVF_HP;
		k = 0.7071f * (1.f - 0.97f * r);
		break;
	default: // TRANSISTOR_24DB, TRANSISTOR_24DB_DRIVE
		topology = MatrixLadder::Topology::LP4;
		k = 3.9f * r; // feedback, oscillation at 4
	}
	core_.configure(topology, g, k);

	return filterGain;
}

} // namespace deluge::dsp::filter
