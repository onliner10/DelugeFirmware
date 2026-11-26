// Heap-backed stand-ins for the firmware's allocator entry points, for DSP code that allocates its own buffers
// (the FDN reverb's delay lines). Not a *_spec.cpp, so the spec glob leaves it alone.

#include "memory/memory_allocator_interface.h"
#include <cstdlib>

void* allocMaxSpeed(uint32_t requiredSize, void* /*thingNotToStealFrom*/) {
	return std::malloc(requiredSize);
}

void* allocLowSpeed(uint32_t requiredSize, void* /*thingNotToStealFrom*/) {
	return std::malloc(requiredSize);
}

extern "C" void delugeDealloc(void* address) {
	std::free(address);
}
