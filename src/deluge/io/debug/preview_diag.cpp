// DIAGNOSTIC ONLY - see preview_diag.h
#include "io/debug/preview_diag.h"
#include "gui/ui/load/load_instrument_preset_ui.h"
#include "gui/ui/ui.h"
#include "hid/display/display.h"
#include "hid/encoders.h"
#include "io/debug/log.h"
#include "processing/engines/audio_engine.h"
#include "storage/audio/audio_file_manager.h"
#include <cstdio>

namespace preview_diag {
namespace {
uint32_t lastShownDrops = 0;
uint32_t lastNoteOns = 0;
bool startsPending = false;
} // namespace

void routine() {
	bool pending = audioFileManager.loadingQueueHasAnySampleStarts();
	if (pending != startsPending) {
		startsPending = pending;
		D_PRINTLN("PREVIEW_DIAG sample-start clusters pending: %d", (int)pending);
	}
	uint32_t ons = sampleNoteOns.load(std::memory_order_relaxed);
	uint32_t fails = lateStartFailures.load(std::memory_order_relaxed);
	uint32_t waited = lateStartWaitSamples.exchange(0, std::memory_order_relaxed);
	uint32_t waitMax = lateStartWaitMaxSamples.exchange(0, std::memory_order_relaxed);
	if (ons != lastNoteOns || waited) {
		lastNoteOns = ons;
		D_PRINTLN("PREVIEW_DIAG note-ons=%d (late=%d) lateStartFailures=%d waitedSilentMs=%d longestWaitMs=%d",
		          (int)ons, (int)lateNoteOns.load(std::memory_order_relaxed), (int)fails, (int)(waited / 44),
		          (int)(waitMax / 44));
	}

	uint32_t drops = notesDroppedStartNotLoaded.load(std::memory_order_relaxed);
	if (drops != lastShownDrops) {
		lastShownDrops = drops;
		char buf[24];
		snprintf(buf, sizeof(buf), "DROP %lu", (unsigned long)drops);
		display->displayPopup(buf, 3);
		D_PRINTLN("PREVIEW_DIAG dropped notes (sample start not loaded): %d", (int)drops);
	}

	if (loadAborted && getCurrentUI() == &loadInstrumentPresetUI && currentUIMode == UI_MODE_NONE
	    && !deluge::hid::encoders::select.pending()
	    && (int32_t)(AudioEngine::audioSampleTimer - loadAbortedAt) > 44100 / 4) {
		loadAborted = false;
		display->displayPopup("STUCK", 3);
		D_PRINTLN("PREVIEW_DIAG STUCK: load aborted, no encoder move left, old preset still playing");
	}
}
} // namespace preview_diag
