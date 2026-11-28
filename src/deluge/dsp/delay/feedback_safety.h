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

#include <algorithm>
#include <cstdint>

/// The Safe Delay Feedback community feature: delay feedback that can still self-oscillate, but can't blow up.
///
/// The feedback amount is a gain on each repeat, with 1 << 30 as unity. The knob goes linearly from 0 to 2.0, so half
/// its range is past unity, where each repeat is louder than the last - at full, 6 dB louder, up to where the loop
/// saturates, well above anything played into it.
namespace deluge::dsp::delay_feedback {

constexpr int32_t kUnity = 1 << 30;

// Up to here (22 of 50 on the knob) the knob does what it always did
constexpr int64_t kKeepUpTo = static_cast<int64_t>(kUnity) * 22 / 25;
// From there, the rest of the knob up to 45 gives finer control over long tails, reaching unity at 45
constexpr int64_t kUnityAt = static_cast<int64_t>(kUnity) * 45 / 25;
// and the last few steps self-oscillate, each repeat at most 2.3 dB louder than the last - enough for a dub throw to
// swell, with the Leveler below keeping it to the level of what was played
constexpr int64_t kKnobMax = 2147483647;
constexpr int64_t kMax = static_cast<int64_t>(kUnity) * 13 / 10;

/// Reshapes the feedback amount from the knob (and anything patched to it) as above
constexpr int32_t shape(int32_t feedback) {
	int64_t const f = feedback;
	if (f <= kKeepUpTo) {
		return feedback;
	}
	if (f <= kUnityAt) {
		return static_cast<int32_t>(kKeepUpTo + (f - kKeepUpTo) * (kUnity - kKeepUpTo) / (kUnityAt - kKeepUpTo));
	}
	return static_cast<int32_t>(kUnity + (f - kUnityAt) * (kMax - kUnity) / (kKnobMax - kUnityAt));
}

/// Lets a delay self-oscillate, but not louder than what's been played into it: once its repeats get more than 3 dB
/// above the loudest recent input, the feedback comes down to hold them there. Called once per render, however long.
class Leveler {
public:
	/// Returns the feedback amount to use for a render of `frames`, given the one asked for, the loudest sample going
	/// into the delay and the loudest coming out of it (which only needs measuring once the feedback is past
	/// kKeepUpTo; 0 below that)
	int32_t process(int32_t feedback, int32_t inputPeak, int32_t wetPeak, int32_t frames) {
		if (feedback <= kUnity) {
			// Repeats die away by themselves. Keep track of what's being played, forgetting it at about 6 dB a second,
			// so that turning the feedback up just after playing something still knows how loud that was.
			reference_ = std::max(inputPeak, forget(reference_, frames));
			// And of how loud the repeats have been all the way round the loop, refreshed each time round, so that
			// turning up past unity knows where the loudest part of a long tail is, wherever in the loop it's got to
			recentWet_ = std::max(wetPeak, forget(recentWet_, frames));
			allowed_ = feedback;
			selfOscillating_ = false;
			return feedback;
		}

		// Self-oscillating: hold the reference while it lasts, so that the loop can carry on at the level it was fed
		reference_ = std::max(reference_, inputPeak);
		if (!selfOscillating_) {
			// Just turned up past unity. Whatever's going round now is fine - a long tail, or repeats piled up from
			// playing into it - so turning the knob up never makes the echo quieter.
			selfOscillating_ = true;
			reference_ = std::max({reference_, wetPeak, recentWet_});
		}
		int64_t const ceiling = static_cast<int64_t>(reference_) + (reference_ >> 2) + (reference_ >> 3); // +2.8 dB

		// No more than takes what's coming round up to the ceiling, so nothing going back in gets past it - and should
		// it be over, only a little under unity, so it eases back down rather than cutting the repeats off
		int32_t target = feedback;
		if (wetPeak > 0) {
			int64_t const reachesCeiling = static_cast<int64_t>(kUnity) * ceiling / wetPeak;
			target = static_cast<int32_t>(std::clamp<int64_t>(reachesCeiling, kFloor, feedback));
		}
		// Down straight away, but back up gently, over about 50 ms, so the level doesn't flutter
		target = std::min(target, feedback);
		allowed_ = (target < allowed_)
		               ? target
		               : allowed_ + static_cast<int32_t>((static_cast<int64_t>(target - allowed_) * frames) >> 11);
		return allowed_;
	}

private:
	static constexpr int32_t kFloor = kUnity - (kUnity >> 4); // each repeat 0.6 dB quieter

	// About 6 dB a second
	static int32_t forget(int32_t level, int32_t frames) {
		return level - static_cast<int32_t>((static_cast<int64_t>(level) * frames) >> 16);
	}

	int32_t reference_ = 0;
	int32_t recentWet_ = 0;
	int32_t allowed_ = kUnity;
	bool selfOscillating_ = false;
};

} // namespace deluge::dsp::delay_feedback
