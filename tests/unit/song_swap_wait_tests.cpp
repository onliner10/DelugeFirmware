#include "CppUTest/TestHarness.h"
#include "gui/ui/load/song_swap_wait.h"

using deluge::gui::SongSwapWait;
using deluge::gui::songSwapWait;

TEST_GROUP(SongSwapWait){};

TEST(SongSwapWait, keepsWaitingWhileTheSwapIsArmed) {
	CHECK(songSwapWait(false, true) == SongSwapWait::KEEP_WAITING);
}

TEST(SongSwapWait, doneOnceTheSwapHappened) {
	// doSongSwap() also leaves the waiting UI modes, but the swap is what counts
	CHECK(songSwapWait(true, false) == SongSwapWait::SWAPPED);
	CHECK(songSwapWait(true, true) == SongSwapWait::SWAPPED);
}

// If the UI mode left the waiting modes without a swap, performLoad() used to carry on and delete the old song while
// it was still the current one, which the audio engine was still rendering (E410).
TEST(SongSwapWait, swapsNowIfTheWaitEndedWithoutASwap) {
	CHECK(songSwapWait(false, false) == SongSwapWait::SWAP_NOW);
}

static_assert(songSwapWait(false, false) == SongSwapWait::SWAP_NOW);
