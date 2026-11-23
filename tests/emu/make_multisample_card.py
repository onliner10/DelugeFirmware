# Builds an SD card folder with several multisampled synth presets (each with its own WAVs).
import math
import os
import struct
import sys
import wave

out = sys.argv[1]
presets = (
    sys.argv[2].split(",") if len(sys.argv) > 2 else ["MS_A", "MS_B", "MS_C", "MS_D"]
)
zones = int(sys.argv[3]) if len(sys.argv) > 3 else 61
secs = float(sys.argv[4]) if len(sys.argv) > 4 else 1.5
sr = 44100
os.makedirs(f"{out}/SYNTHS", exist_ok=True)
for pi, p in enumerate(presets):
    d = f"{out}/SAMPLES/{p}"
    os.makedirs(d, exist_ok=True)
    ranges = []
    for z in range(zones):
        note = 24 + z  # C1..
        freq = 440 * 2 ** ((note - 69) / 12)
        fn = f"SAMPLES/{p}/N{note:03d}.WAV"
        n = int(sr * secs)
        one = [
            struct.pack(
                "<h", int(12000 * math.sin(2 * math.pi * freq * i / sr) * (1 - i / n))
            )
            for i in range(n)
        ]
        frames = b"".join(x + x for x in one)
        with wave.open(f"{out}/{fn}", "wb") as w:
            w.setnchannels(2)
            w.setsampwidth(2)
            w.setframerate(sr)
            w.writeframes(frames)
        top = note if z < zones - 1 else 127
        ranges.append(
            f'\t\t\t<sampleRange rangeTopNote="{top}" fileName="{fn}">\n'
            f'\t\t\t\t<zone startSamplePos="0" endSamplePos="{n}" />\n\t\t\t</sampleRange>'
        )
    xml = f"""<?xml version="1.0" encoding="UTF-8"?>
<sound
	firmwareVersion="4.1.0"
	earliestCompatibleFirmware="4.1.0"
	polyphonic="poly"
	voicePriority="1"
	mode="subtractive">
	<osc1
		type="sample"
		transpose="0"
		cents="0"
		loopMode="0"
		reversed="0"
		timeStretchEnable="0"
		timeStretchAmount="0">
		<sampleRanges>
{chr(10).join(ranges)}
		</sampleRanges>
	</osc1>
	<osc2
		type="square"
		transpose="0"
		cents="0" />
	<defaultParams
		oscAVolume="0x7FFFFFFF"
		oscBVolume="0x80000000"
		volume="0x4CCCCCA8" />
</sound>
"""
    with open(f"{out}/SYNTHS/{p}.XML", "w") as f:
        f.write(xml)
print("ok", out)
