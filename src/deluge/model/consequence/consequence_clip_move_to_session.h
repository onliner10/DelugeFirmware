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

#include "model/consequence/consequence.h"
#include <cstdint>

class Clip;

/// An arrangement-only (white) clip that was moved into the session, getting a section. Unlike
/// ConsequenceClipExistence, the clip itself stays attached to its output and is never freed here: it only changes
/// list.
class ConsequenceClipMoveToSession final : public Consequence {
public:
	ConsequenceClipMoveToSession(Clip* clip, int32_t arrangement_only_index, int32_t session_index)
	    : clip_(clip), arrangement_only_index_(arrangement_only_index), session_index_(session_index) {}
	Error revert(TimeType time, ModelStack* model_stack) override;

private:
	Clip* clip_;
	int32_t arrangement_only_index_;
	int32_t session_index_;
	uint8_t section_ = 0;
};
