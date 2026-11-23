// DIAGNOSTIC ONLY - see preview_diag.h
#include "io/debug/preview_diag.h"
#include "gui/ui/load/load_instrument_preset_ui.h"
#include "gui/ui/ui.h"
#include "hid/display/display.h"
#include "hid/encoders.h"
#include "io/debug/log.h"
#include "processing/engines/audio_engine.h"
#include <cstdio>

namespace preview_diag {
namespace {
uint32_t lastShownDrops = 0;
}

void routine() {
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
