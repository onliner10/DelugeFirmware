/*
 * Copyright © 2015-2023 Synthstrom Audible Limited
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
#include "dsp/convolution/impulse_response_processor.h"
#include "dsp/delay/delay_buffer.h"
#include "dsp/delay/feedback_safety.h"
#include "dsp/delay/saturating_tap_delay.h"
#include "model/sync.h"

#include <cstdint>
#include <span>

class Delay {
public:
	/// Stored in files as the "analog" attribute, so append new types at the end. Older firmware clamps unknown
	/// values to ANALOG.
	enum class Type : uint8_t {
		DIGITAL = 0,
		ANALOG = 1,
		WARM = 2, ///< whole-sample taps, saturating and filtered feedback loop (SaturatingTapDelay)
	};
	static constexpr int32_t kNumTypes = 3;

	struct State {
		bool doDelay;
		int32_t userDelayRate;
		int32_t delayFeedbackAmount;
		int32_t analog_saturation = 8;
		/// WARM: level of the saturator's rail is 2^this. A full-level sample peaks around 2^24.7 in a Sound's
		/// buffer and the bus buffers (kit, audio clip, song) run 2 bits hotter.
		int32_t saturation_reference_log2 = 27;
		/// Safe Delay Feedback: the amount has been through delay_feedback::shape(), and self-oscillation is held to
		/// the level of what's played in
		bool safeFeedback = false;
	};

	Delay() = default;
	Delay(const Delay& other) = delete;

	// was originally cloneFrom
	Delay& operator=(const Delay& rhs) {
		pingPong = rhs.pingPong;
		type = rhs.type;
		syncLevel = rhs.syncLevel;
		syncType = rhs.syncType;
		return *this;
	}

	[[nodiscard]] constexpr bool isActive() const {
		return (primaryBuffer.isActive() || secondaryBuffer.isActive() || saturatingTapDelay.isActive());
	}

	void informWhetherActive(bool newActive, int32_t userDelayRate = 0);
	void copySecondaryToPrimary();
	void copyPrimaryToSecondary();
	void initializeSecondaryBuffer(int32_t newNativeRate, bool makeNativeRatePreciseRelativeToOtherBuffer);
	void setupWorkingState(State& workingState, uint32_t timePerInternalTickInverse, bool anySoundComingIn = true);
	void discardBuffers();
	void setTimeToAbandon(const State& workingState);
	void hasWrapped();

	DelayBuffer primaryBuffer;
	DelayBuffer secondaryBuffer;
	ImpulseResponseProcessor ir_processor;
	SaturatingTapDelay saturatingTapDelay;

	uint32_t countCyclesWithoutChange;
	int32_t userRateLastTime;
	bool pingPong = true;
	Type type = Type::DIGITAL;

	SyncType syncType = SYNC_TYPE_EVEN;

	// Basically, 0 is off, max value is 9. Higher numbers are shorter intervals (higher speed).
	SyncLevel syncLevel = SYNC_LEVEL_16TH;

	int32_t sizeLeftUntilBufferSwap;

	int32_t postLPFL;
	int32_t postLPFR;

	int32_t prevFeedback = 0;

	uint8_t repeatsUntilAbandon = 0; // 0 means never abandon

	void process(std::span<StereoSample> buffer, const State& delayWorkingState);

private:
	deluge::dsp::delay_feedback::Leveler leveler_;

	void prepareToBeginWriting();
	[[nodiscard]] constexpr int32_t getAmountToWriteBeforeReadingBegins() const { return secondaryBuffer.size(); }
};
