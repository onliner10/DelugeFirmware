Shared filter (synths only, when polyphonic).

Off (the default): every voice has its own filter, as it always has.

On: the voices are mixed first and the mix goes through one filter, so the filter costs the same however many voices
are sounding - with a 6-note pad that's a sixth of the filter work. It sounds a little different, like a string machine
or a paraphonic synth rather than a fully polyphonic one:
  - each voice's amplitude envelope (and drive) comes before the filter rather than after it, so the filter smooths a
    fast release slightly, and resonance can ring on briefly after a note ends;
  - the filter's cutoff and resonance follow the newest note: a filter envelope, velocity or anything else patched to
    the filter per voice comes from the last note played, not each note's own;
  - drive into the filter (24dB Drive, or lots of resonance) works on the whole chord at once.

Best for pads and chords with no per-note filter modulation, where it sounds the same in practice.
