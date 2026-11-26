# Changelog

## 0.1.7 – 2026-11-26

A safety pass before the first install on an FM-1.

- **More room in the audio interrupt.** The orbit's shape is now a table (built once when ALGORITHM changes it,
  within 5/32767 of the exact shape): a sample no longer calls a function that computed up to three sines. The
  landscape glides only for 0.14 s after the ants change it, not on every block. The per-sample loop is now ~110
  instructions a voice (the chip's own code), ~900 with all eight voices: inside the 1000-1500 that Dinghy's sloopDX
  engines take and fit.
- **Two edge cases the host's sanitizers found**, harmless on the chip but fixed: a shift of a negative number, and the
  DIAMOND orbit reaching -32769 (its product could overflow and shift the orbit for a sample).
- Checked: the update path (loader, updater, USB, rescue, flash driver, package tool) is byte for byte PLUMB's; the
  presets need 2084 of the 4084 bytes of their sector; ten minutes of random buttons, knobs, keys, MIDI and presets on
  the host with AddressSanitizer and UndefinedBehaviorSanitizer: no error.

## 0.1.6 – 2026-11-25

- **LATCH keeps the whole chord.** Letting go of the keys one after the other dropped each note let go while others
  were still held, so only the last one stayed latched. Now the chord is every note pressed since the keys (and MIDI)
  were last all up; the next key starts a new chord. Turning LATCH off leaves the keys held now as the chord.

## 0.1.5 – 2026-11-25

- **Only the ants sing.** The keys no longer play the engine. On every page they set the root (C4 = C, "ROOT D" on
  the screen); on the ARP page they hold the chord the ants sing. MIDI notes in do the same: the chord with the ARP
  on, the root otherwise. The root's key is lit, with the ants' notes flashing on the keys.

## 0.1.4 – 2026-11-25

- **A random sound: hold EDIT for 300 ms on the EDIT page.** The orbit, SIZE, GROUND, SMOOTH, DRIFT, the ENV and the LFO
  get new values, within musical ranges (mostly short attacks, decays of 0.2 to 10 s, a gentle LFO). The colony, the
  map, the scale and FX stay. Once a hold; only from the EDIT page itself. The EDIT page's hint says so.

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
