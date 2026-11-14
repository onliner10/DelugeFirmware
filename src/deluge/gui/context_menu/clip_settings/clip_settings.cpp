/*
 * Copyright (c) 2024 Sean Ditny
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

#include "gui/context_menu/clip_settings/clip_settings.h"
#include "definitions_cxx.hpp"
#include "gui/context_menu/clip_settings/freeze_source.h"
#include "gui/context_menu/clip_settings/launch_style.h"
#include "gui/l10n/l10n.h"
#include "gui/ui/rename/rename_clip_ui.h"
#include "gui/ui/root_ui.h"
#include "gui/views/session_view.h"
#include "hid/display/display.h"
#include "model/clip/audio_clip.h"
#include "model/clip/clip.h"
#include "model/clip/clip_freezer.h"
#include "model/clip/instrument_clip.h"
#include "model/output.h"
#include "model/settings/runtime_feature_settings.h"
#include <cstddef>

namespace deluge::gui::context_menu::clip_settings {

PLACE_SDRAM_BSS ClipSettingsMenu clipSettings{};

char const* ClipSettingsMenu::getTitle() {
	static char const* title = "Clip Settings";
	return title;
}

std::span<char const*> ClipSettingsMenu::getOptions() {
	using enum l10n::String;
	numOptions = 0;
	auto add = [this](Option option, l10n::String string) {
		optionIDs[numOptions] = option;
		optionNames[numOptions] = l10n::get(string);
		numOptions++;
	};

	bool freezeOn = runtimeFeatureSettings.isOn(RuntimeFeatureSettingType::ClipFreeze);
	if (clipFreezer.involves(clip)) {
		add(Option::CANCEL_FREEZE, STRING_FOR_CANCEL_FREEZE);
	}
	else if (clip->type == ClipType::INSTRUMENT) {
		add(Option::CONVERT_TO_AUDIO, STRING_FOR_CONVERT_TO_AUDIO);
		if (freezeOn && clip->output->type == OutputType::MIDI_OUT) {
			add(Option::FREEZE, STRING_FOR_FREEZE);
		}
	}
	else if (static_cast<AudioClip*>(clip)->hasFrozenSource()) {
		// Offered even with the feature turned off, so nobody's stuck with a frozen clip
		add(Option::UNFREEZE, STRING_FOR_UNFREEZE);
	}
	add(Option::CLIP_MODE, STRING_FOR_CLIP_MODE);
	add(Option::CLIP_NAME, STRING_FOR_CLIP_NAME);
	return {optionNames.data(), numOptions};
}

bool ClipSettingsMenu::setupAndCheckAvailability() {
	this->currentOption = scrollPos = 0; // start at the top
	return true;
}

void ClipSettingsMenu::selectEncoderAction(int8_t offset) {
	ContextMenu::selectEncoderAction(offset);
}

bool ClipSettingsMenu::acceptCurrentOption() {
	getOptions(); // In case anything changed while the menu was open
	if (this->currentOption < 0 || static_cast<size_t>(this->currentOption) >= numOptions) {
		return false;
	}

	switch (optionIDs[this->currentOption]) {
	case Option::CONVERT_TO_AUDIO:
		sessionView.replaceInstrumentClipWithAudioClip(clip);
		return false; // exit UI

	case Option::FREEZE:
		if (!ClipFreezer::canFreeze(clip, true)) {
			return false;
		}
		freezeSource.clip = static_cast<InstrumentClip*>(clip);
		freezeSource.setupAndCheckAvailability();
		openUI(&freezeSource);
		return true;

	case Option::UNFREEZE:
		ClipFreezer::unfreeze(static_cast<AudioClip*>(clip));
		return false;

	case Option::CANCEL_FREEZE:
		clipFreezer.cancel();
		return false;

	case Option::CLIP_MODE:
		launchStyle.clip = clip;
		launchStyle.setupAndCheckAvailability();
		openUI(&launchStyle);
		return true;

	case Option::CLIP_NAME:
		currentUIMode = UI_MODE_NONE;
		renameClipUI.clip = clip;
		openUI(&renameClipUI);
		return true;
	}
	return false;
}

ActionResult ClipSettingsMenu::padAction(int32_t x, int32_t y, int32_t on) {
	if (on) {
		return ContextMenu::padAction(x, y, on);
	}
	else {                                      // this would happen if you release pad after entering the menu
		return sessionView.padAction(x, y, on); // let the grid handle this
	}
}

} // namespace deluge::gui::context_menu::clip_settings
