"""Unit tests for the harness's own logic (no emulator needed)."""

from deluge_emu.watcher import summarize

FAULT = """DELUGE_EMU_CRASH kind=cpu_fault
exception_type=0 (0=undefined 1=prefetch_abort 2=data_abort 3=reserved 15=svc)
faulting_pc=0x20065896
deluge_main + 878 in section .text
Line 914 of "src/deluge/deluge.cpp" starts at address 0x20065878
"""

FREEZE = """DELUGE_EMU_CRASH kind=freeze
error = 0xc0910f4 "EMU1"

--- backtrace ---
#0  freezeWithError (error=0xc0910f4 "EMU1") at src/deluge/hid/display/display.cpp:151
"""

OLED_FREEZE = """DELUGE_EMU_CRASH kind=freeze
error="B001"
"""


def test_summarize_cpu_fault():
    assert (
        summarize(FAULT)
        == "undefined instruction at 0x20065896 (deluge_main + 878 in section .text)"
    )


def test_summarize_freeze():
    assert summarize(FREEZE) == "freezeWithError(EMU1)"
    assert summarize(OLED_FREEZE) == "freezeWithError(B001)"
