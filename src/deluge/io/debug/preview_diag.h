/*
 * DIAGNOSTIC ONLY - not for merging.
 * Makes the two ways a preset preview can fail visible on the device:
 *  - "DROP n": n notes were silently dropped because their sample-start Cluster wasn't loaded yet at note-on.
 *  - "STUCK": a preset load was aborted by the select encoder, but no encoder move is left to load anything else,
 *             so the browser shows the new preset while the clip still plays the old one.
 */
#pragma once
#include <atomic>
#include <cstdint>

namespace preview_diag {
/// Bumped from the audio routine, so atomic.
inline std::atomic<uint32_t> notesDroppedStartNotLoaded{0};
/// Every sample-osc note-on (samplesLate == 0), to confirm notes are actually being played.
inline std::atomic<uint32_t> sampleNoteOns{0};
inline std::atomic<uint32_t> lateNoteOns{0};
/// Samples of audio a late-starting voice spent waiting (silent) for its Clusters to load.
inline std::atomic<uint32_t> lateStartWaitSamples{0};
inline std::atomic<uint32_t> lateStartWaitMaxSamples{0};
inline std::atomic<uint32_t> lateStartFailures{0};
/// Main-thread only.
inline bool loadAborted = false;
inline uint32_t loadAbortedAt = 0;

void routine();
} // namespace preview_diag
