"""Smoke tests: the firmware boots in the emulator and responds to input.

These double as worked examples for writing new emulator tests.
"""

import pytest


def lit(emu):
    return {k for k, on in emu.dz.read_leds().items() if on}


@pytest.mark.sd("none")
def test_boots_without_card(emu):
    emu.wait_for_log(r"going into main loop")
    assert "No SD card present" in emu.log()
    assert any("#" in row for row in emu.dz.oled_text().splitlines()), "OLED is blank"


def test_boots_with_card(emu):
    emu.wait_for_log(r"going into main loop")
    assert "No SD card present" not in emu.log()


def test_play_toggles_transport(emu):
    assert "PLAY" not in lit(emu)
    emu.dz.press("PLAY")
    emu.wait(0.5)
    assert "PLAY" in lit(emu)
    emu.dz.press("PLAY")
    emu.wait(0.5)
    assert "PLAY" not in lit(emu)


@pytest.mark.sd("basic")
def test_load_song(emu):
    # The default new song starts on a synth clip; the fixture song's first clip is a kit.
    assert "SYNTH" in lit(emu)
    emu.dz.load_song("/SONGS/MULTISAMPLE.XML")
    emu.wait(1)
    assert "KIT" in lit(emu)
    emu.dz.press("PLAY")
    emu.wait(2)
    assert "PLAY" in lit(emu)
