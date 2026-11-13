/*
 * Copyright © 2019-2023 Synthstrom Audible Limited
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

#include "edit_name.h"
#include "gui/ui/rename/rename_clip_ui.h"
#include "gui/ui/rename/rename_drum_ui.h"
#include "gui/ui/rename/rename_output_ui.h"
#include "gui/ui/sound_editor.h"
#include "gui/views/instrument_clip_view.h"
#include "hid/display/display.h"
#include "model/instrument/kit.h"
#include "model/song/song.h"
#include "util/functions.h"

namespace deluge::gui::menu_item {

namespace {
// Also called for the clip/output targets, e.g. from song view where there may be no current clip.
bool kitHasSelectedDrum() {
	Clip* clip = getCurrentClip();
	if (clip == nullptr || clip->output == nullptr || clip->output->type != OutputType::KIT) {
		return false;
	}
	return static_cast<Kit*>(clip->output)->selectedDrum != nullptr;
}
} // namespace

bool EditName::isRelevant(ModControllableAudio* modControllable, int32_t whichThing) const {
	return editNameTargetAvailable(target_, kitHasSelectedDrum());
}

void EditName::beginSession(MenuItem* navigatedBackwardFrom) {
	if (!editNameTargetAvailable(target_, kitHasSelectedDrum())) {
		// Nothing to rename: RenameDrumUI would freeze (RN01) without a selected drum.
		display->displayPopup(l10n::get(l10n::String::STRING_FOR_SELECT_A_ROW_OR_AFFECT_ENTIRE));
		// Don't leave the editor on an item that does nothing (same as when the audio recorder can't open)
		if (getCurrentUI() == &soundEditor) {
			soundEditor.goUpOneLevel();
		}
		return;
	}

	Clip* clip = getCurrentClip();
	RenameUI* ui = nullptr;

	switch (target_) {
	case Target::CLIP:
		renameClipUI.clip = clip;
		ui = &renameClipUI;
		break;

	case Target::DRUM:
		ui = &renameDrumUI;
		break;

	case Target::AUDIO_OUTPUT:
		renameOutputUI.output = getCurrentOutput();
		ui = &renameOutputUI;
		break;
	}

	// Done, go for it.
	soundEditor.shouldGoUpOneLevelOnBegin = true;
	if (clip->type == ClipType::INSTRUMENT) {
		instrumentClipView.cancelAllAuditioning();
	}
	openUI(ui);
}

} // namespace deluge::gui::menu_item
