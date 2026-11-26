# Changelog

## 0.1.3 – 2026-11-25

- **The seed sings an octave up.** On the cells of the SEED's pattern (ONE DOT, DASHES, CROSS, RING, DUST, MOSS,
  STRIPES) the ants sing an octave higher, for the colony's whole life, painted over or not: you hear what the seed
  does. On HOME a seed's cell walked bare keeps a faint amber dot; on SEQ the seed's cells show in chalk. A
  USER preset keeps where its seed lay.

## 0.1.2 – 2026-11-25

- **Twice as fast with both OCT buttons.** OCT− or OCT+ alone: the ants walk backwards or forwards at the same speed
  (holding one no longer speeds it up). Hold OCT− and press OCT+: backwards, twice as fast, as long as both are held;
  hold OCT+ and press OCT−: forwards, twice as fast. Exactly twice, at every STEP (1/32 too), and the notes keep their
  full loudness. The time line shows two white arrows meanwhile.

## 0.1.1 – 2026-11-25

- **No more distortion with long decays.** DECAY now ends at 10 s (it went to 20 s). Long footsteps pile up: eight
  voices ringing for 10 s were up to four times full scale and were squashed by the output's ceiling. Now the voices
  have a limiter of their own, and the whole sound one more after the echo and the reverb: the level comes down
  smoothly before it would distort, and comes back within half a second. On the host: four ants and a held chord at
  10 s, every preset, even with REVERB and ECHO at 100 %, stay under the ceiling; at normal settings nothing changes.

## 0.1 – 2026-11-25

The first ANTHEM: a colony of ants writes the music, and time runs both ways.

- Up to four ants on a ground of 56 x 15 cells (a torus). Nine rules (LANGTON, SQUARE, MIRROR, CHAOS, CLOUD, KNOT,
  WEAVE, CORAL, SPIRAL), eight seeds, six step lengths. REC hatches a new colony; SEL freezes the paint.
- Time backwards and forwards (OCT- / OCT+, held: four times as fast); SELECT walks it a step at a time. The rule
  runs backwards exactly: the colony un-writes itself and sings its song in reverse.
- SEQ: the map from a cell to its note (ACROSS, UP, RANGE: a Tonnetz by default) and which cells sing.
- The sound: wave terrain synthesis over the ground the ants paint. Five orbits (CIRCLE, EIGHT, ROSE, DIAMOND,
  LINE) on ALGORITHM; SIZE, GROUND, SMOOTH, DRIFT (each ant sounds like the ground under its feet).
- ENV (ATTACK, DECAY, ACCENT, BLOOM), LFO (SIZE, SHIFT, PITCH, LEVEL), the ARP as a chord the ants sing, FX
  (reverb, echo, chorus in parallel), 48 scales.
- 8 presets and 8 USER slots; a USER slot keeps the colony too.
- On Dinghy 0.6: MIDI clock sync, MIDI in and out, USB audio, FM-1 Mirror, the backup page.
- Tested on the host only (screens, sound, the clock sync test); not yet on an FM-1.
