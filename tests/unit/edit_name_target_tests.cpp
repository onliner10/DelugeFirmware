#include "CppUTest/TestHarness.h"
#include "gui/menu_item/edit_name_target.h"

using deluge::gui::menu_item::EditNameTarget;
using deluge::gui::menu_item::editNameTargetAvailable;

TEST_GROUP(EditNameTarget){};

// RenameDrumUI freezes (RN01) when the kit has no selected drum, so "Rename drum" must not be available then.
TEST(EditNameTarget, renameDrumNeedsASelectedDrum) {
	CHECK_FALSE(editNameTargetAvailable(EditNameTarget::DRUM, false));
	CHECK_TRUE(editNameTargetAvailable(EditNameTarget::DRUM, true));
}

TEST(EditNameTarget, clipAndOutputNamesDoNotDependOnTheSelectedDrum) {
	CHECK_TRUE(editNameTargetAvailable(EditNameTarget::CLIP, false));
	CHECK_TRUE(editNameTargetAvailable(EditNameTarget::CLIP, true));
	CHECK_TRUE(editNameTargetAvailable(EditNameTarget::AUDIO_OUTPUT, false));
	CHECK_TRUE(editNameTargetAvailable(EditNameTarget::AUDIO_OUTPUT, true));
}

static_assert(!editNameTargetAvailable(EditNameTarget::DRUM, false));
