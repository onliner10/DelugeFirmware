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

// Per-voice drive (LOCAL_DRIVE: a polynomial soft clipper), and the "Character FX" community setting's alternative
// bitcrush (a fractional bit-depth quantizer) and sample-rate reduction (a crossfaded dual sample-and-hold).
//
// Each effect is defined on a normalised signal, full scale +-1.0, and a normalised amount p in [0, 1], where p == 0
// is an exact bypass. Here they run in q31 against a fixed full-scale reference per place in the chain (see below).

#include "dsp/stereo_sample.h"
#include "util/fixedpoint.h"
#include "util/functions.h"
#include <algorithm>
#include <array>
#include <cstdint>

namespace deluge::dsp::character_fx {

// Level references -------------------------------------------------------------------------------------------------
//
// The curves are designed so that a full-level sample peaks at about 0.40 of full scale at the quantizer input and
// at about 0.17 of full scale at the drive input. The references below place a full-level Deluge sample there.
//
// A 0 dBFS 16-bit sample at default Deluge settings (osc volume max, LOCAL_VOLUME 0, envelope at sustain, velocity
// 100) peaks at about 2^24 * 1.64 at the per-voice drive / saturation point in Voice::render() and at the Sound's
// processSRRAndBitcrushing(): sample << 16 = 2^31, times source amplitude 2^25 << 3, times overall amplitude
// 2^28 << 1, gives 2^24; velocity 100 through the default velocity->volume cable and the volume parabola adds
// about x1.64. These figures come from reading the code, not from a measurement, so expect a few dB of error.
//
// - Sound-level bitcrush / SRR: 2^24 * 1.64 / 0.40 = 2^26.04, so full scale is 2^26.
// - Bus-level bitcrush / SRR (kit, audio clip, song master): those buffers come after the sounds' post-FX volume,
//   and the existing clip-level tanh saturation already treats them as 4x (2 bits) hotter than a voice (shift 3 +
//   clippingAmount instead of 5 + clippingAmount). Use the same 2 bits: full scale is 2^28.
// - Per-voice drive: 2^24 * 1.64 / 0.17 = 2^27.26. Not a power of two: it is folded into the drive multiplier,
//   which costs nothing.
//
// The quantizer clips its input at +-full scale, so the references leave a few dB of room over a single full-level
// sample. SRR does not depend on level.
constexpr int32_t kSoundFullScaleLog2 = 26;
constexpr int32_t kBusFullScaleLog2 = 28;
constexpr double kVoiceDriveFullScale = 16777216.0 * 1.64 / 0.17;

// Polynomial soft clipper (per-voice drive) ------------------------------------------------------------------------
//
// drive = 0.1534 + 0.8465 * p^2 (<= 1), y = clip(drive * x, +-1), then y = y * (2 - |y|) three times. Each pass has
// slope 2 at 0 and 0 at +-1, so the small-signal gain is 8 * drive and the curve meets the rails smoothly. Not level
// compensated and not antialiased. p is LOCAL_DRIVE's final value (q31, 0 = off: the caller skips it).

/// drive * 2^57 / full scale: smmul by this then << 6 gives drive * x / full scale in q31
constexpr int32_t kDriveGainForFullDrive = (int32_t)(144115188075855872.0 / kVoiceDriveFullScale);
/// 2 * full scale: smmul by this takes q31 back to the reference's scale
constexpr int32_t kDriveOutputGain = (int32_t)(2.0 * kVoiceDriveFullScale);

/// Input gain for drive amount p (q31, > 0). Per block.
[[gnu::always_inline]] inline int32_t driveInputGain(q31_t amount) {
	constexpr q31_t kOffset = (q31_t)(0.1534 * 2147483648.0);
	constexpr q31_t kSlope = (q31_t)(0.8465 * 2147483648.0);
	q31_t squared = multiply_32x32_rshift32(amount, amount) << 1;
	q31_t drive = kOffset + (multiply_32x32_rshift32(squared, kSlope) << 1); // at most 0.9999, so no overflow
	return multiply_32x32_rshift32(drive, kDriveGainForFullDrive) << 1;
}

/// y * (2 - |y|) in q31, written as y + y * (1 - |y|) so that 2.0 never has to be represented.
[[gnu::always_inline]] inline q31_t driveShape(q31_t y) {
	q31_t magnitude = y ^ (y >> 31); // |y|, one LSB short for negatives, so -2^31 can't overflow
	// y * (1 - |y|) is at most 1/4, so the << 1 can't overflow; the saturating add catches the last LSB at the rails
	return add_saturate(y, multiply_32x32_rshift32(y, ONE_Q31 - magnitude) << 1);
}

[[gnu::always_inline]] inline q31_t drive(q31_t x, int32_t inputGain) {
	q31_t y = lshiftAndSaturate<6>(multiply_32x32_rshift32(x, inputGain)); // clip(drive * x, +-1)
	y = driveShape(y);
	y = driveShape(y);
	y = driveShape(y);
	return multiply_32x32_rshift32(y, kDriveOutputGain);
}

// Fractional bit-depth quantizer -----------------------------------------------------------------------------------
//
// bits = kBitsTable[p * 127], linearly interpolated between entries; n = floor(bits), fr = bits - n.
// u = clamp((x + 1) / 2, 0, 1), two rounding quantizers Lhi = round(u * (2^(n+1) - 1)), qhi = Lhi * 2^-(n+1) and
// Llo = round(u * (2^n - 1)), qlo = Llo * 2^-n, out = 2 * (fr * qhi + (1 - fr) * qlo) - 1, so the bit depth sweeps
// continuously. The 2^-n scale (instead of 1 / (2^n - 1)) makes the top level 1 - 2^(1-n), so loud signals get a DC
// offset at low bit depths (silence still maps exactly to 0); a DC blocker follows to remove it.

// Bits per knob position 0..127, in Q16: [1] = 15.69 down to [127] = 1.90. [0] = 31 (transparent) is never reached,
// as the param's "off" threshold sits just below position 1.
inline constexpr std::array<int32_t, 128> kBitsTable = {
    2031616, 1028508, 1008743, 989277, 970109, 951236, 932656, 914366, //
    896363,  878647,  861213,  844060, 827186, 810587, 794262, 778209, //
    762424,  746906,  731652,  716660, 701927, 687451, 673229, 659260, //
    645541,  632069,  618842,  605858, 593114, 580608, 568337, 556300, //
    544494,  532916,  521564,  510436, 499529, 488841, 478369, 468112, //
    458066,  448230,  438601,  429176, 419954, 410931, 402106, 393476, //
    385039,  376792,  368733,  360860, 353170, 345661, 338330, 331175, //
    324194,  317385,  310744,  304270, 297960, 291812, 285823, 279991, //
    274314,  268789,  263414,  258187, 253105, 248165, 243366, 238705, //
    234179,  229787,  225526,  221393, 217386, 213503, 209741, 206099, //
    202572,  199161,  195860,  192670, 189586, 186607, 183731, 180954, //
    178275,  175691,  173200,  170799, 168487, 166260, 164116, 162053, //
    160069,  158161,  156326,  154563, 152869, 151242, 149678, 148177, //
    146735,  145351,  144021,  142743, 141515, 140335, 139200, 138108, //
    137057,  136043,  135065,  134121, 133207, 132322, 131463, 130628, //
    129815,  129020,  128242,  127478, 126727, 125984, 125249, 124519, //
};

// Above ~24 bits the quantizer is transparent at these references anyway, and capping keeps every shift in range.
constexpr int32_t kMaxBits = 24;

/// Knob position p * 127 in Q16, from an unpatched param value (full int32 range = p 0..1).
[[gnu::always_inline]] inline uint32_t knobPositionQ16(int32_t paramValue) {
	uint32_t positive = (uint32_t)paramValue + 2147483648u;
	return (uint32_t)(((uint64_t)positive * 127) >> 16);
}

/// Per-block setup of the quantizer. process() is the scalar reference; the firmware's NEON loop
/// (ModControllableAudio) computes exactly the same thing four lanes at a time.
template <int32_t fullScaleLog2>
struct BitReducer {
	static_assert(kMaxBits + 1 <= fullScaleLog2 && fullScaleLog2 <= 30);

	explicit BitReducer(int32_t paramValue) {
		uint32_t position = knobPositionQ16(paramValue);
		int32_t index = position >> 16;
		int32_t bits = kBitsTable[index];
		if (index < 127) {
			int32_t fraction = position & 65535;
			bits += (int32_t)(((int64_t)(kBitsTable[index + 1] - bits) * fraction) >> 16);
		}
		bits = std::min(bits, kMaxBits << 16);
		int32_t n = bits >> 16;
		fraction = (bits & 65535) << 15; // q31

		// round(u * (2^n - 1)) with u in Q(fullScaleLog2 + 1) is a rounding doubling multiply-high (vqrdmulh) by
		// (2^n - 1) << (30 - fullScaleLog2)
		loMultiplier = ((1 << n) - 1) << (30 - fullScaleLog2);
		hiMultiplier = ((1 << (n + 1)) - 1) << (30 - fullScaleLog2);
		// L * 2^-n * 2 * fullScale = L << (fullScaleLog2 + 1 - n)
		loShift = fullScaleLog2 + 1 - n;
		hiShift = fullScaleLog2 - n;
	}

	[[gnu::always_inline]] q31_t process(q31_t x) const {
		// u * 2^(fullScaleLog2 + 1), clamped to [0, 1)
		int32_t u = signed_saturate<fullScaleLog2 + 1>(x) + (1 << fullScaleLog2);
		int32_t lo = (int32_t)(((int64_t)u * loMultiplier * 2 + 0x80000000) >> 32) << loShift;
		int32_t hi = (int32_t)(((int64_t)u * hiMultiplier * 2 + 0x80000000) >> 32) << hiShift;
		return lo + (int32_t)(((int64_t)(hi - lo) * fraction * 2) >> 32) - (1 << fullScaleLog2);
	}

	int32_t loMultiplier;
	int32_t hiMultiplier;
	int32_t loShift;
	int32_t hiShift;
	q31_t fraction;
};

/// One-pole DC blocker, about 3.4 Hz at 44.1 kHz: out = x - state, then state += floor((x - state) / 2^11), plus 1 if
/// that distance is positive, so any non-zero distance moves the state and it settles exactly on a constant input
/// (silence in gives exactly 0 out). Scalar reference for the NEON loop.
constexpr int32_t kDCBlockerShift = 11;
[[gnu::always_inline]] inline q31_t blockDC(q31_t x, int32_t& state) {
	int32_t distance = x - state; // inputs are within +-2^28, so no overflow
	state += (distance >> kDCBlockerShift) + (distance > 0);
	return distance;
}

// Crossfaded dual sample-and-hold ----------------------------------------------------------------------------------
//
// T = 0.91875 * (1 + 100 * k^2 / 127^2) - 1 (at least 0) for knob position k = round(p * 127): hold periods from 1
// sample (44.1 kHz) to about 93 samples (about 475 Hz). n = floor(T), fr = T - n. Two free-running zero-order holds on
// the same input, with periods n + 1 and n + 2 samples, out = (1 - fr) * A + fr * B. No interpolation, no anti-alias
// filter: the beat between the two unsynchronised holds is part of the sound.

constexpr int32_t makeSRRPeriodQ16(int32_t knob) {
	double t = 0.91875 * (1.0 + 100.0 * knob * knob / (127.0 * 127.0)) - 1.0;
	return (t > 0.0) ? (int32_t)(t * 65536.0 + 0.5) : 0;
}

constexpr std::array<int32_t, 128> makeSRRPeriodTable() {
	std::array<int32_t, 128> table{};
	for (int32_t k = 0; k < 128; k++) {
		table[k] = makeSRRPeriodQ16(k);
	}
	return table;
}

/// T per knob position 0..127, in Q16 samples (hold periods are floor(T) + 1 and + 2)
inline constexpr auto kSRRPeriodTable = makeSRRPeriodTable();

struct SampleRateReducer {
	/// Per-block setup from the SRR param value
	explicit SampleRateReducer(int32_t paramValue) {
		// Nearest knob position rather than truncated, so the param's maximum (2^31 - 1, just under p = 1) reaches 127
		int32_t knob = (knobPositionQ16(paramValue) + 32768) >> 16;
		int32_t period = kSRRPeriodTable[knob];
		periodA = (period >> 16) + 1;
		periodB = periodA + 1;
		fraction = (period & 65535) << 15; // q31
	}

	uint32_t periodA;
	uint32_t periodB;
	q31_t fraction;
};

} // namespace deluge::dsp::character_fx
