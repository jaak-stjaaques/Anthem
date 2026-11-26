# Changelog

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
