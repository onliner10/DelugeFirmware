#include "filter_config.h"
#include "mem_functions.h"
#include "util/container/enum_to_string_map.hpp"

// converts lpf/hpf mode to string for saving
namespace deluge::dsp::filter {

EnumStringMap<FilterMode, kNumFilterModes> filterMap({{{FilterMode::TRANSISTOR_12DB, "12dB"},
                                                       {FilterMode::TRANSISTOR_24DB, "24dB"},
                                                       {FilterMode::TRANSISTOR_24DB_DRIVE, "24dBDrive"},
                                                       {FilterMode::SVF_BAND, "SVF_Band"},
                                                       {FilterMode::SVF_NOTCH, "SVF_Notch"},
                                                       {FilterMode::HPLADDER, "HPLadder"},
                                                       {FilterMode::OFF, "Off"}}});

EnumStringMap<FilterRoute, kNumFilterRoutes> routeMap({{
    {FilterRoute::LOW_TO_HIGH, "L2H"},
    {FilterRoute::PARALLEL, "PARA"},
    {FilterRoute::HIGH_TO_LOW, "H2L"},
}});

} // namespace deluge::dsp::filter

char const* filterRouteToString(FilterRoute route) {
	return deluge::dsp::filter::routeMap(route);
}

FilterRoute stringToFilterRoute(char const* string) {
	return deluge::dsp::filter::routeMap(string);
}

FilterMode stringToLPFType(char const* string) {
	FilterMode mode = deluge::dsp::filter::filterMap(string);
	// The drive is the sound's saturation now (before or after the filter), not a filter mode
	return (mode == FilterMode::TRANSISTOR_24DB_DRIVE) ? FilterMode::TRANSISTOR_24DB : mode;
}
char const* lpfTypeToString(FilterMode mode) {
	return deluge::dsp::filter::filterMap(mode);
}
