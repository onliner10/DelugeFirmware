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

#include "dsp/filter/filter.h"
#include "dsp/filter/matrix_ladder.h"
#include "model/mod_controllable/filters/filter_config.h"

namespace deluge::dsp::filter {

/// The ladder filter modes (12dB, 24dB and the HPF ladder) on MatrixLadder: linear, so cheap whatever the resonance.
/// No drive and no morph: saturation is the sound's own (before or after the filter, see Sound::saturationPreFilter).
///  - 24dB (and the old 24dB Drive): the 4-pole ladder lowpass
///  - 12dB: a 2-pole state-variable lowpass
///  - HPF ladder: a 2-pole state-variable highpass
class MatrixFilter : public Filter<MatrixFilter> {
public:
	MatrixFilter() = default;
	// returns a compensatory gain value
	q31_t setConfig(q31_t frequency, q31_t resonance, FilterMode mode, q31_t morph, q31_t filterGain);
	void doFilter(q31_t* startSample, q31_t* endSample, int32_t sampleIncrement) {
		int32_t const numSamples =
		    static_cast<int32_t>((endSample - startSample + sampleIncrement - 1) / sampleIncrement);
		core_.process(startSample, numSamples, sampleIncrement);
	}
	void doFilterStereo(q31_t* startSample, q31_t* endSample) {
		core_.processStereo(startSample, static_cast<int32_t>((endSample - startSample) >> 1));
	}
	void resetFilter() { core_.reset(); }

private:
	MatrixLadder core_;
};

} // namespace deluge::dsp::filter
