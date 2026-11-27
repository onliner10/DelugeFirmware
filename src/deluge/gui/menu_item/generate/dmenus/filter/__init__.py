from dmui.dsl import Menu, Submenu

from . import hpf, lpf

routing = Menu(
    "FilterRouting",
    "filterRoutingMenu",
    ["{title}"],
    "filter/routing.md",
    title="STRING_FOR_FILTER_ROUTE",
)

shared = Menu(
    "filter::SharedFilter",
    "sharedFilterMenu",
    ["{name}", "{title}"],
    "filter/shared.md",
    name="STRING_FOR_SHARED_FILTER",
    title="STRING_FOR_SHARED_FILTER",
)

sound_filters = Submenu(
    "Submenu",
    "soundFiltersMenu",
    ["{title}", "%%CHILDREN%%"],
    "filter/sound_filters.md",
    [lpf.menu, hpf.menu, routing, shared],
    name="STRING_FOR_HPF",
    title="STRING_FOR_FILTERS",
)
