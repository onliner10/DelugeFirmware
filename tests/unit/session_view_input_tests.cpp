#include "CppUTest/TestHarness.h"
#include "gui/views/session_view_input.h"

using deluge::gui::views::saveDeletesHeldClip;

TEST_GROUP(SessionViewSaveDeletesHeldClip){};

TEST(SessionViewSaveDeletesHeldClip, deletesWhileRowsClipPressed) {
	CHECK_TRUE(saveDeletesHeldClip(true, false, false));
}

TEST(SessionViewSaveDeletesHeldClip, deletesWhileGridPadHeld) {
	CHECK_TRUE(saveDeletesHeldClip(false, true, false));
}

TEST(SessionViewSaveDeletesHeldClip, doesNothingWithoutAHeldClip) {
	CHECK_FALSE(saveDeletesHeldClip(false, false, false));
}

// A short tap on a Grid clip pad starts the zoom into that clip. SAVE pressed during that animation must not delete
// the clip, or the finished transition opens a deleted clip (froze with E369 / E410).
TEST(SessionViewSaveDeletesHeldClip, neverDeletesDuringClipTransition) {
	CHECK_FALSE(saveDeletesHeldClip(false, true, true));
	CHECK_FALSE(saveDeletesHeldClip(true, false, true));
	CHECK_FALSE(saveDeletesHeldClip(true, true, true));
	CHECK_FALSE(saveDeletesHeldClip(false, false, true));
}

// It's a constexpr predicate: make sure it stays usable at compile time.
static_assert(saveDeletesHeldClip(true, false, false));
static_assert(!saveDeletesHeldClip(false, true, true));
