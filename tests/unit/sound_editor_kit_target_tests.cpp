#include "CppUTest/TestHarness.h"
#include "gui/ui/sound_editor_kit_target.h"

using deluge::gui::KitEditTarget;
using deluge::gui::kitEditTarget;

TEST_GROUP(SoundEditorKitEditTarget){};

TEST(SoundEditorKitEditTarget, kitGlobalFXWinsOverTheSelectedDrum) {
	// Editing the whole kit uses the clip's ParamManager, so the kit FX menu has to be used too, whatever drum
	// happens to be selected. Picking a drum menu here froze the Deluge (E411 in a sound menu, RN01 on rename).
	CHECK(kitEditTarget(true, DrumType::SOUND) == KitEditTarget::KIT_GLOBAL_FX);
	CHECK(kitEditTarget(true, DrumType::MIDI) == KitEditTarget::KIT_GLOBAL_FX);
	CHECK(kitEditTarget(true, DrumType::GATE) == KitEditTarget::KIT_GLOBAL_FX);
	CHECK(kitEditTarget(true, std::nullopt) == KitEditTarget::KIT_GLOBAL_FX);
}

TEST(SoundEditorKitEditTarget, selectedDrumWhenNotEditingTheWholeKit) {
	CHECK(kitEditTarget(false, DrumType::SOUND) == KitEditTarget::SOUND_DRUM);
	CHECK(kitEditTarget(false, DrumType::MIDI) == KitEditTarget::MIDI_DRUM);
	CHECK(kitEditTarget(false, DrumType::GATE) == KitEditTarget::GATE_DRUM);
}

TEST(SoundEditorKitEditTarget, nothingToEditWithoutADrumOrAffectEntire) {
	CHECK(kitEditTarget(false, std::nullopt) == KitEditTarget::NONE);
}

static_assert(kitEditTarget(true, DrumType::SOUND) == KitEditTarget::KIT_GLOBAL_FX);
