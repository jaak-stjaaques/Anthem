/* SPDX-License-Identifier: GPL-3.0-only */
/* ANTHEM: an FM-1 firmware on Dinghy, where a colony of ants writes the music, and time runs both ways.
 *
 * Up to four ants walk a ground of 56 x 15 cells. On each step an ant turns as its RULE says for the colour under it,
 * paints the cell the next colour and walks on (a turmite: Langton's ant and its family). Each step it sings the note
 * of its cell. From a bare ground comes a scribble, then order, then chaos; the ground they paint is also the sound:
 * wave terrain synthesis, an orbit drawn over the landscape of their paint. And the rule runs backwards exactly:
 * OCT- turns time back, and the colony un-writes itself, singing its song in reverse, back to the moment it hatched.
 *
 *   HOME   COLONY    ANTS, RULE, STEP, SEED; REC hatches a new colony, SEL freezes the paint (the walk loops)
 *   SEQ    HARMONY   the map from a cell to its note: ACROSS, UP, RANGE, and which cells SING
 *   EDIT   TERRAIN   the orbit (ALGORITHM: its shape), SIZE, GROUND, SMOOTH, DRIFT
 *   ENV    FOOTSTEP  ATTACK, DECAY, ACCENT (the colour's weight), BLOOM (the orbit opens at each step)
 *   LFO    WIND      SIZE, SHIFT, PITCH or LEVEL
 *   ARP    CHORD     hold keys: the ants sing only your chord
 *   FX, GLO          as ever: reverb, echo, chorus; the scale, the tempo, the sync, the MIDI channel
 *   OCT- / OCT+      time backwards / forwards; hold one and press the other: twice as fast; SELECT: a step at a time
 *
 * The files: anthem.h (the colony, the terrain engine, the ENV, the LFO; the audio interrupt), scale.h (48 scales),
 * effects.h (echo, reverb, chorus in parallel), this file (the keys, the presets, the pages). platform.c and the
 * kit_*.h are Dinghy's. */
#include <stdint.h>
#define APP_NAME "ANTHEM"
static void app_idle(void);
static void clock_rt(uint32_t st);
#define LCD_IDLE app_idle                       /* while the screen's DMA runs: MIDI in */
#define MIDI_RT_HOOK(st) clock_rt(st)           /* clock and transport as they arrive: kit_clock.h */
#define LCD_MIRROR 1                            /* FM-1 Mirror: kit_mirror.h */
#define CV_MAX (240u * 150u)
#define BACKUP_TAG0 'A'
#define BACKUP_TAG1 'N'
#include "platform.c"
#if BOARD_SMK37
#include "smk_wheel.h"
#endif

static void app_tick(uint32_t tick, uint32_t ofs);
#define CLOCK_TICK(tick, ofs) app_tick(tick, ofs)
#include "kit_clock.h"
#include "scale.h"
#include "anthem.h"
#include "effects.h"

static void live_note(uint32_t on, uint32_t note, uint32_t vel);
static void live_panic(void);
static void midi_program(uint32_t p);
#define MIDI_NOTE(on, note, vel) live_note(on, note, vel)
#define MIDI_PANIC() live_panic()
#define MIDI_PROGRAM(p) midi_program(p)
#include "kit_midi.h"

/* ------------------------------------------------------------------------------------------------ the sound */
static int32_t output_level(int32_t sample)
{
    int32_t m = sample < 0 ? -sample : sample;  /* a soft ceiling (as LOOPHEADS) */
    if (m > 20000) {
        int32_t excess = m - 20000;
        m = 20000 + 12000 * excess / (12000 + excess);
    }
    return sample < 0 ? -m : m;
}

/* The last limiter, after the effects: a full echo and a full reverb on top of four ants can still build up. As the
 * voices' one (anthem.h): down within 0.7 ms before the sum reaches 19000, back up slowly. */
static void out_limit(int32_t *out, uint32_t frames)
{
    static int32_t gain = Q15;
    int32_t peak = 0;
    for (uint32_t i = 0; i < 2u * frames; i++) { int32_t m = out[i] < 0 ? -out[i] : out[i]; if (m > peak) peak = m; }
    int32_t target = peak > 19000 ? (19000 << 15) / peak : Q15;
    int32_t g0 = gain, g1 = target < gain ? target : gain + ((target - gain) >> 6);
    uint32_t ramp = target < gain ? 32u : frames;
    for (uint32_t i = 0; i < frames; i++) {
        int32_t g = i < ramp ? g0 + (g1 - g0) * (int32_t)i / (int32_t)ramp : g1;
        out[2u * i] = (int32_t)(((int64_t)out[2u * i] * g) >> 15);
        out[2u * i + 1u] = (int32_t)(((int64_t)out[2u * i + 1u] * g) >> 15);
    }
    gain = g1;
}

static void app_audio(int32_t *out, uint32_t frames)
{
#ifndef DINGHY_HOST
    uint32_t t0 = fm1_ticks();
#endif
    clock_audio(frames);                        /* the transport first: its ticks move the ants */
    anthem_render(out, frames);
    effects_process(out, frames);
    out_limit(out, frames);
    int32_t vol = midi_vol_q15();
    for (uint32_t i = 0; i < 2u * frames; i++) out[i] = (output_level(out[i]) * vol) >> 15;
#ifndef DINGHY_HOST
    /* A safety net (as BUOY, PLUMB): if a block ever takes more than 3.8 ms of its 5.8, new notes get only five
     * voices until it has been calm for 2 s: a little less sound instead of crackles. */
    static uint16_t calm;
    uint32_t us = (fm1_ticks() - t0) / FM1_TICKS_PER_US;
    if (us > 3800u) { voice_cap = 5; calm = 0; }
    else if (voice_cap < NVOICE && us < 2500u && ++calm > 350u) voice_cap = NVOICE;
#endif
}

#include "kit_mirror.h"
#define BACKUP_STOP() (live_panic(), clock_stop())
#include "kit_backup.h"

/* ---------------------------------------------------------------------------------------------- settings */
#define DECAY_MAX 30u                           /* env_ms[30] = 10 s: the longest DECAY */
#define RANDOM_MS 300u                          /* EDIT held this long on EDIT: a random sound */
#define KEY0 53u                                /* key 0 = F3 */
#define SET_VER 1u
static struct { uint8_t ver, oct, ch, sync_int, tempo, pad[3]; } set = {SET_VER, 0, 0, 0, 96, {0}};

static char *put_num(char *p, int32_t v, int sign)
{
    char tmp[12];
    uint32_t n = 0, u = (uint32_t)(v < 0 ? -v : v);
    if (v < 0) *p++ = '-';
    else if (sign && v > 0) *p++ = '+';
    do { tmp[n++] = (char)('0' + u % 10u); u /= 10u; } while (u);
    while (n) *p++ = tmp[--n];
    *p = 0;
    return p;
}

static char *put_str(char *p, const char *s)
{
    while (*s) *p++ = *s++;
    *p = 0;
    return p;
}

/* ------------------------------------------------------------------------------------------------- the ARP
 * Hold keys and the ants sing only your chord: each cell's note goes to the nearest note of the chord (NEAREST), or
 * the cells take the chord's notes in turn (IN TURN). */
enum { PICK_NEAREST, PICK_TURN, PICKS };
static const char *const pick_names[PICKS] = {"NEAREST", "IN TURN"};
static struct { uint8_t on, latch, spread, pick, chance; } arp = {0, 0, 2, PICK_NEAREST, 100};
static uint8_t held[16], held_n, latched;       /* the keys (and MIDI notes) held, in the order pressed */
static uint8_t arp_hist[16], arp_hist_n;        /* what the ants sang last (0..254 high), for the screen */
static int32_t chord[32];
static uint32_t chord_n;

static void chord_build(void)                   /* the held notes, low to high, SPREAD octaves of them */
{
    int32_t base[16];
    uint32_t n = 0;
    for (uint32_t i = 0; i < held_n; i++) {
        int32_t s = scale_note_step(held[i]), j = (int32_t)n;
        uint8_t dup = 0;
        for (uint32_t q = 0; q < n; q++) dup |= base[q] == s;
        if (dup) continue;
        while (j > 0 && base[j - 1] > s) { base[j] = base[j - 1]; j--; }
        base[j] = s;
        n++;
    }
    chord_n = 0;
    int32_t lo = -(int32_t)(arp.spread / 2u);
    for (int32_t o = lo; o < lo + (int32_t)arp.spread; o++)
        for (uint32_t i = 0; i < n && chord_n < 32u; i++) chord[chord_n++] = scale_step_pitch(base[i] + o * (int32_t)scale_now.size);
    chance_pct = arp.on ? arp.chance : 100u;
}

/* ------------------------------------------------------------------------------------- the map (SEQ page)
 * A cell's note: ACROSS scale steps for each column to the right, UP for each row up, folded into RANGE octaves
 * around the root. ACROSS 4 and UP 2 in a seven-note scale is a Tonnetz: fifths one way, thirds the other. */
static int32_t cell_degree(uint32_t x, uint32_t y)
{
    int32_t n = scale_now.size, span = n * (pat.range ? pat.range : 1);
    int32_t d = (int32_t)x * pat.across + (int32_t)(GH - 1u - y) * pat.up;
    d %= span;
    if (d < 0) d += span;
    return d - n * (pat.range / 2);
}

static void map_build(void)                     /* every cell's note -> the ISR (only when something changed) */
{
    static uint32_t last = 0;
    uint32_t h = 2166136261u;
#define MIXH(v) (h = (h ^ (uint32_t)(v)) * 16777619u)
    MIXH(pat.across); MIXH(pat.up); MIXH(pat.range); MIXH(scale_now.scale); MIXH(scale_now.transpose);
    MIXH(scale_now.octave); MIXH(arp.on); MIXH(arp.pick); MIXH(chord_n);
    for (uint32_t i = 0; i < chord_n; i++) MIXH(chord[i]);
#undef MIXH
    if (h == last) return;
    last = h;
    uint8_t use_chord = arp.on && chord_n;
    for (uint32_t y = 0; y < GH; y++)
        for (uint32_t x = 0; x < GW; x++) {
            int32_t d = cell_degree(x, y), p = scale_step_pitch(d + 12 * 0) + 12 * 256 * scale_now.octave;
            if (use_chord) {
                if (arp.pick == PICK_TURN) {
                    int32_t m = (int32_t)chord_n, i = d % m;
                    p = chord[i < 0 ? i + m : i] + 12 * 256 * scale_now.octave;
                } else {
                    int32_t best = chord[0], bd = 1 << 30;
                    for (uint32_t i = 0; i < chord_n; i++) {
                        int32_t q = chord[i] + 12 * 256 * scale_now.octave, dd = q - p;
                        if (dd < 0) dd = -dd;
                        if (dd < bd) { bd = dd; best = q; }
                    }
                    p = best;
                }
            }
            cell_pitch[y][x] = clampi(p, 12 * 256, 120 * 256);
        }
}

/* ------------------------------------------------------------------------------- the terrain (EDIT page)
 * The landscape the orbits read: a plain round hill (a bare ground sounds round and soft), and on it the paint, a
 * height for each colour, smoothed SMOOTH times; GROUND is how high the paint stands. Built here, glided to in the
 * ISR. */
static const int16_t colour_h[4] = {0, 16000, -12000, 9000};
static int16_t tbuf[TH][TW];

static void terrain_build(uint32_t now)
{
    static uint32_t last_ver = ~0u, last_par = ~0u, last_at;
    uint32_t par = (uint32_t)pat.ground | (uint32_t)pat.smooth << 8;
    uint32_t ver = world_ver;
    if (ver == last_ver && par == last_par) return;
    if (par == last_par && now - last_at < 20u) return;
    last_ver = ver; last_par = par; last_at = now;
    for (uint32_t ty = 0; ty < TH; ty++)
        for (uint32_t tx = 0; tx < TW; tx++) tbuf[ty][tx] = colour_h[ground[ty * GH / TH][tx * GW / TW] & 3u];
    for (uint32_t pass = 0; pass <= pat.smooth; pass++) {   /* (RAW too: one pass, or the corners alias) */
        int16_t row[TW], col[TH];
        for (uint32_t ty = 0; ty < TH; ty++) {
            for (uint32_t tx = 0; tx < TW; tx++) row[tx] = tbuf[ty][tx];
            for (uint32_t tx = 0; tx < TW; tx++)
                tbuf[ty][tx] = (int16_t)((row[(tx + TW - 1u) % TW] + 2 * row[tx] + row[(tx + 1u) % TW]) / 4);
        }
        for (uint32_t tx = 0; tx < TW; tx++) {
            for (uint32_t ty = 0; ty < TH; ty++) col[ty] = tbuf[ty][tx];
            for (uint32_t ty = 0; ty < TH; ty++)
                tbuf[ty][tx] = (int16_t)((col[(ty + TH - 1u) % TH] + 2 * col[ty] + col[(ty + 1u) % TH]) / 4);
        }
    }
    int32_t g = pat.ground;
    for (uint32_t ty = 0; ty < TH; ty++)
        for (uint32_t tx = 0; tx < TW; tx++) {
            int32_t hill = mul15(sinq(tx * (0xFFFFFFFFu / TW)), sinq(ty * (0xFFFFFFFFu / TH) + 0x40000000u)) * 12000 / 32767;
            terr_to[ty][tx] = (int16_t)clampi(hill + tbuf[ty][tx] * g / 100, -16000, 16000);
        }
}

/* ---------------------------------------------------------------------------------- the keys and MIDI in */
static const char *message;
static uint32_t message_at;
static void say(const char *text) { message = text; message_at = fm1_ms; }

/* The keys never sound: only the ants sing. On the ARP page they hold the chord the ants sing; everywhere else they
 * set the root (C4 = C). MIDI notes in do the same: the chord with the ARP on, else the root. */
static const char *const root_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};
static void root_from(uint32_t note)
{
    static char b[16];
    scale_now.transpose = (int8_t)clampi((int32_t)note - 60, -31, 43);
    put_str(put_str(b, "ROOT "), root_names[scale_root_pc()]);
    say(b);
}

/* LATCH: the chord is every note pressed since the keys (and MIDI) were last all up; letting go, one finger after the
 * other, keeps it all. The next note pressed after that starts a new chord. */
static uint8_t notes_down;                      /* panel keys and MIDI notes held now */

static void chord_key(uint32_t on, uint32_t note)
{
    if (on) {
        if (notes_down < 255u) notes_down++;
        if (arp.latch && latched) { held_n = 0; latched = 0; }   /* a new chord after a latched one */
        for (uint32_t i = 0; i < held_n; i++) if (held[i] == note) return;
        if (held_n < 16u) held[held_n++] = (uint8_t)note;
    } else {
        if (notes_down) notes_down--;
        if (arp.latch) {                        /* LATCH: nothing goes; once every note is up, the chord is kept */
            if (!notes_down) latched = 1;
            return;
        }
        uint32_t j = 0;
        for (uint32_t i = 0; i < held_n; i++) if (held[i] != note) held[j++] = held[i];
        held_n = (uint8_t)j;
    }
}

static void live_note(uint32_t on, uint32_t note, uint32_t vel)
{
    (void)vel;
    if (arp.on) chord_key(on, note);
    else if (on) root_from(note);
}

static void live_panic(void)
{
    notes_down = 0;
    for (uint32_t n = 0; n < 128u; n++) cmd_put(0, (uint8_t)(n + 1u), 0, 0);
    held_n = 0;
    latched = 0;
}

/* ---------------------------------------------------------------------------------------------- presets
 * 8 to start from, and 8 USER slots that SAVE fills (kept in flash). A preset is the whole instrument; a USER preset
 * also keeps the colony itself (the ground, the ants, the time), so it comes back exactly where it was. */
#define FACTORY 8u
#define USERS 8u
static const char *const factory_names[FACTORY] = {
    "FIRST STEPS", "SQUARE DANCE", "MIRROR HALL", "LEAFCUTTER", "NIGHT MARCH", "SUGAR TRAIL", "ANTHILL", "HIGHWAY"
};
/*  orbit       size ground smooth drift  A   D  acc bloom  lfo target, wave, rate, depth   rev echo time cho
 *  ants rule step seed  across up range sing  scale */
static const patch_t factory[FACTORY] = {
    {ORB_CIRCLE,  35, 60, 2,  80,   2, 16, 40, 40,  LFO_SHIFT, LW_SINE, 10, 20,     45, 20, 4, 0,
     1, 0, 3, SEED_EMPTY,  4, 2, 2, SING_PAINT,  1, {0}},
    {ORB_DIAMOND, 40, 70, 1, 100,   1, 14, 60, 50,  LFO_SIZE, LW_TRIANGLE, 20, 30,  30, 30, 3, 10,
     2, 1, 1, SEED_DOT,    2, 3, 2, SING_PAINT,  3, {0}},
    {ORB_ROSE,    30, 80, 3,  60,   3, 20, 30, 30,  LFO_SHIFT, LW_DRIFT, 8, 40,     65, 15, 5, 30,
     2, 2, 2, SEED_CROSS,  4, 1, 2, SING_PAINT,  8, {0}},
    {ORB_LINE,    25, 90, 1, 100,   0, 12, 70, 60,  LFO_PITCH, LW_SINE, 40, 5,      25, 35, 1, 0,
     3, 3, 3, SEED_DUST,   3, 5, 3, SING_PAINT,  4, {0}},
    {ORB_EIGHT,   45, 65, 2,  70,   1, 18, 50, 20,  LFO_LEVEL, LW_TRIANGLE, 30, 20, 50, 25, 3, 20,
     4, 5, 1, SEED_RING,   4, 2, 2, SING_PAINT, 28, {0}},
    {ORB_CIRCLE,  20, 50, 4,  50,   8, 22, 20, 70,  LFO_SIZE, LW_SINE, 12, 40,      70, 20, 4, 40,
     2, 4, 1, SEED_MOSS,   2, 4, 2, SING_PAINT, 10, {0}},
    {ORB_ROSE,    35, 100, 2, 100,  0, 10, 80, 80,  LFO_SHIFT, LW_RANDOM, 36, 30,   20, 40, 2, 0,
     4, 6, 3, SEED_STRIPES, 5, 2, 3, SING_PAINT, 19, {0}},
    {ORB_DIAMOND, 35, 75, 2,  90,   2, 15, 40, 40,  LFO_SIZE, LW_DRIFT, 6, 50,      55, 30, 3, 15,
     1, 0, 5, SEED_DUST,   4, 3, 2, SING_PAINT, 25, {0}},
};

typedef struct { patch_t p; int32_t t; uint8_t ants[ANTS * 3u]; uint8_t cells[GH * GW / 4u + 1u]; uint8_t has; } user_t;
static struct { uint32_t magic; user_t user[USERS]; } store;
#define PRESET_MAGIC 0x414E5431u                /* "ANT1" */
#define PRESET_BLOB 0u
static uint8_t preset, last_user = FACTORY;

static void preset_name(uint8_t p, char *b)
{
    if (p < FACTORY) { put_str(b, factory_names[p]); return; }
    put_str(b, "USER ");
    b[5] = (char)('1' + p - FACTORY);
    b[6] = 0;
}

/* a patch read from flash is checked value by value: a damaged slot never points past the end of a table */
static int patch_ok(const patch_t *s)
{
    return s->orbit < ORBITS && s->size <= 100 && s->ground <= 100 && s->smooth <= 4 && s->drift <= 100 &&
           s->attack < ENV_STEPS && s->decay <= DECAY_MAX && s->accent <= 100 && s->bloom <= 100 &&
           s->lfo_target < LFO_TARGETS && s->lfo_wave < LFO_WAVES && s->lfo_rate < LFO_RATES && s->lfo_depth <= 100 &&
           s->reverb <= 100 && s->echo <= 100 && s->echo_time < ECHOS && s->chorus <= 100 &&
           s->ants >= 1 && s->ants <= ANTS && s->rule < RULES && s->step < STEPS && s->seed < SEEDS &&
           s->across <= 12 && s->up <= 12 && s->range >= 1 && s->range <= 4 && s->sing < SINGS && s->scale < SCALE_COUNT;
}

static void preset_load(uint8_t p)
{
    if (p >= FACTORY + USERS) p = 0;
    preset = p;
    patch_t next = p < FACTORY ? factory[p] : store.user[p - FACTORY].p;
    if (!patch_ok(&next)) next = factory[p % FACTORY];
    pat = next;
    scale_select(pat.scale);
    if (p >= FACTORY) {
        last_user = p;
        user_t *u = &store.user[p - FACTORY];
        if (u->has == 1u && !world_load) {      /* its colony, exactly as it was saved */
            for (uint32_t i = 0; i < sizeof world_buf; i++) world_buf[i] = u->cells[i];
            for (uint32_t i = 0; i < sizeof world_ants; i++) world_ants[i] = u->ants[i];
            world_buf_t = u->t;
            world_seed = u->p.pad[0] ? (uint8_t)((u->p.pad[0] - 1u) % SEEDS) : pat.seed;   /* where its seed lay */
            world_load = 1;
            return;
        }
    }
    hatch_w++;                                  /* a factory preset: a new colony on its seed */
}

static void presets_init(void)
{
    if (blob_load(PRESET_BLOB, &store, sizeof store) < 0 || store.magic != PRESET_MAGIC) {
        store.magic = PRESET_MAGIC;
        for (uint32_t i = 0; i < USERS; i++) { store.user[i].p = factory[i]; store.user[i].has = 0; }
    }
    for (uint32_t i = 0; i < USERS; i++)
        if (!patch_ok(&store.user[i].p)) { store.user[i].p = factory[i]; store.user[i].has = 0; }
}

static int preset_save(uint8_t slot)            /* the patch and the colony as it stands */
{
    user_t *u = &store.user[slot - FACTORY];
    pat.scale = scale_now.scale;
    u->p = pat;
    for (uint32_t i = 0; i < sizeof u->cells; i++) u->cells[i] = 0;
    for (uint32_t i = 0; i < GH * GW; i++) u->cells[i >> 2] |= (uint8_t)((ground[i / GW][i % GW] & 3u) << ((i & 3u) * 2u));
    for (uint32_t i = 0; i < ANTS; i++) { u->ants[3u * i] = ant_vx[i]; u->ants[3u * i + 1u] = ant_vy[i]; u->ants[3u * i + 2u] = ant_vd[i]; }
    u->t = world_t;
    u->p.pad[0] = (uint8_t)(hatched_seed + 1u);  /* the seed the colony hatched on (its cells sing an octave up) */
    u->has = 1;
    store.magic = PRESET_MAGIC;
    return blob_save(PRESET_BLOB, &store, sizeof store) >= 0;
}

static void midi_program(uint32_t p) { preset_load((uint8_t)(p % (FACTORY + USERS))); }

/* -------------------------------------------------------------------------------------------- the screen
 * As LOOPHEADS, Endless Memory, BUOY, PLUMB: IBM Plex Mono, the title and the battery on top, then the TIME line (on
 * every page: how long the colony has lived, how much of the ground is painted, which way time runs), a picture, the
 * four knobs numbered underneath, a hint at the bottom. The paint is amber, blue and chalk; the ants are red; time
 * is blue. */
#include "ui_font.h"
#include "anthem_boot_logo.h"

#define C_BG RGB(10, 16, 22)
#define C_AMBER RGB(226, 176, 84)
#define C_AMBER_DIM RGB(110, 86, 44)
#define C_TIME RGB(122, 168, 240)
#define C_TIME_DIM RGB(48, 70, 108)
#define C_KEY RGB(236, 232, 222)
#define C_OFF RGB(140, 152, 160)
#define C_RULE RGB(44, 58, 68)
#define C_GRAPH RGB(22, 33, 42)
#define C_ANT RGB(255, 92, 72)
#define C_REC RGB(255, 96, 96)
#define UI_LEFT 15u
#define UI_WIDTH 210u
#define UI_FOOTER_Y 218u
#define UI_BATTERY_WIDTH 27u
#define LINE_Y 46u                              /* the time line: 210 x 16 */
#define PIC_Y 68u                               /* the picture: 224 x 60 */
#define PIC_H 60u
#define ROW_Y 134u                              /* the four knob rows */
#define ROW_H 19u
#define BOOT_LOGO_MS 1800u
#define BOX_X 8u
#define BOX_W 224u

static const uint16_t paint_c[4] = {C_GRAPH, C_AMBER, C_TIME, C_KEY};

enum { P_HOME, P_ARP, P_SEQ, P_EDIT, P_ENV, P_LFO, P_FX, P_GLO };

static void ui_text(uint32_t x, uint32_t y, uint32_t width, const felucca_font_t *font,
                    const char *text, uint16_t color, int align)
{
    draw_text_box_bg(x, y, width, font, text, color, align, C_BG);
}

static void ui_divider(uint32_t y, uint16_t color) { lcd_fill(UI_LEFT, y, UI_WIDTH, 1, color); }
static void ui_footer(const char *hint, uint16_t color) { ui_text(UI_LEFT, UI_FOOTER_Y, UI_WIDTH, &UI_SMALL, hint, color, 1); }

static void ui_row(uint32_t knob, const char *label, const char *value, uint16_t value_colour)
{
    char n[3] = {(char)('1' + knob), ' ', 0};
    cv_begin(UI_WIDTH, UI_BODY.h, C_BG);
    if (*label) {
        int32_t x = cv_text(0, 0, &UI_BODY, n, C_OFF, C_BG);
        cv_text(x, 0, &UI_BODY, label, C_KEY, C_BG);
        cv_text((int32_t)UI_WIDTH - text_w(&UI_BODY, value), 0, &UI_BODY, value, value_colour, C_BG);
    }
    cv_blit(UI_LEFT, ROW_Y + knob * ROW_H);
}

#if !BOARD_SMK37
static uint8_t read_battery_level(uint32_t now)
{
    static uint32_t last_read;
    static int32_t average;
    static uint8_t level = UINT8_MAX;
    if (now - last_read >= 250u) {
        last_read = now;
        int32_t raw = fm1_adc_read(FM1_ADC_BATT);
        if (raw > 0) {
            average = average ? average + (raw * 16 - average) / 4 : raw * 16;
            level = average >= 591 * 16 ? 3 : average >= 561 * 16 ? 2 : average >= 531 * 16 ? 1 : 0;
        }
    }
    return level;
}

static void draw_battery(uint8_t level)
{
    uint16_t outline = level == UINT8_MAX ? C_RULE : C_OFF;
    cv_begin(UI_BATTERY_WIDTH, 12, C_BG);
    cv_rect(0, 0, 24, 12, outline);
    cv_rect(1, 1, 22, 10, C_BG);
    cv_rect(24, 4, 3, 4, outline);
    if (level != UINT8_MAX)
        for (uint32_t i = 0; i < level; i++) cv_rect(3 + i * 6, 3, 5, 6, C_AMBER);
    cv_blit(UI_LEFT + UI_WIDTH - UI_BATTERY_WIDTH, 17);
}
#endif

static void draw_boot_logo(void)
{
    lcd_fill(0, 0, 240, 240, C_BG);
    uint32_t run = 0, remaining = 0;
    uint16_t color = 0;
    for (uint32_t y = 0; y < BOOT_LOGO_HEIGHT; y += cv_h) {
        cv_begin(BOOT_LOGO_WIDTH, BOOT_LOGO_HEIGHT - y, C_BG);
        uint16_t *pixel = cv_px;
        uint32_t pixels = cv_w * cv_h;
        while (pixels) {
            if (!remaining) {
                remaining = boot_logo_runs[run++];
                color = swap16(boot_logo_runs[run++]);
            }
            uint32_t count = remaining < pixels ? remaining : pixels;
            remaining -= count;
            pixels -= count;
            while (count--) *pixel++ = color;
        }
        cv_blit(BOOT_LOGO_X, BOOT_LOGO_Y + y);
    }
    ui_text(UI_LEFT, 214, UI_WIDTH, &UI_SMALL, "v" APP_VER, C_OFF, 1);
}

static void fmt(char *b, int32_t v, const char *unit) { put_str(put_num(b, v, 0), unit); }

static void fmt_ms(char *b, uint32_t ms)
{
    if (ms < 1000u) { fmt(b, (int32_t)ms, " MS"); return; }
    char *p = put_num(b, (int32_t)(ms / 1000u), 0);
    if (ms < 10000u && ms % 1000u) { *p++ = '.'; *p++ = (char)('0' + ms % 1000u / 100u); }
    put_str(p, " S");
}

static const char *const note_names[] = {"C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"};

static char *pitch_name(char *b, int32_t pitch)  /* 1/256 semitones -> "G4" */
{
    int32_t n = (pitch + 128) >> 8;
    char *p = put_str(b, note_names[n % 12]);
    return put_num(p, n / 12 - 1, 0);
}

static uint16_t mix565(uint32_t a, uint32_t b, uint32_t k)   /* RGB565 a -> b, k/16 of the way */
{
    uint32_t ar = a >> 11, ag = (a >> 5) & 63u, ab = a & 31u, br = b >> 11, bg = (b >> 5) & 63u, bb = b & 31u;
    uint32_t r = (ar * (16u - k) + br * k) / 16u, g = (ag * (16u - k) + bg * k) / 16u, bl = (ab * (16u - k) + bb * k) / 16u;
    return (uint16_t)(r << 11 | g << 5 | bl);
}

static void cv_disc(int32_t cx, int32_t cy, int32_t r, uint16_t c)
{
    for (int32_t dy = -r; dy <= r; dy++)
        for (int32_t dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r + r / 2) cv_pset(cx + dx, cy + dy, c);
}

static void cv_tri(int32_t x, int32_t y, int32_t dir, uint16_t c)   /* a small arrow: dir 1 right, -1 left */
{
    for (int32_t i = 0; i < 5; i++) cv_rect(dir > 0 ? x + i : x + 4 - i, y + i, 1, 9 - 2 * i, c);
}

/* the ants on the ground: where they sang last, for the screen */
static uint32_t sang_at[ANTS];

/* --- the TIME line, on every page: the colony's age, how much is painted, which way time runs */
static void draw_time_line(uint8_t time_page)
{
    char b[16];
    int32_t t = world_t;
    char *p = put_str(b, "T");
    put_num(p, t, 1);
    if (!t) put_str(b, "T 0");
    uint16_t ct = clk_run ? C_TIME : C_OFF;
    cv_begin(UI_WIDTH, 16, C_BG);
    int32_t wl = cv_text(0, 1, &UI_SMALL, b, ct, C_BG);
    const char *word = !clk_run ? "STOP" : frozen ? "LOOP" : time_dir < 0 ? "BACK" : "FWD";
    int32_t wm = text_w(&UI_SMALL, word);
    int32_t x0 = 70 > wl + 8 ? 70 : wl + 8, x1 = (int32_t)UI_WIDTH - wm - 16;
    cv_rect(x0, 8, x1 - x0, 1, C_RULE);
    for (int32_t i = 0; i <= 4; i++) cv_rect(x0 + i * (x1 - x0) / 4, 6, 1, 5, C_RULE);
    uint32_t pc = painted;
    int32_t w = (int32_t)(pc * (uint32_t)(x1 - x0) / (GW * GH));
    cv_rect(x0, 7, w, 3, time_page ? C_AMBER : C_AMBER_DIM);
    if (clk_run && !frozen) {                   /* the way time runs; two arrows: twice as fast */
        cv_tri((int32_t)UI_WIDTH - wm - 10, 3, time_dir, time_fast ? C_KEY : C_TIME);
        if (time_fast) cv_tri((int32_t)UI_WIDTH - wm - 16, 3, time_dir, C_KEY);
    }
    else if (frozen) { cv_rect((int32_t)UI_WIDTH - wm - 11, 4, 2, 8, C_TIME); cv_rect((int32_t)UI_WIDTH - wm - 7, 4, 2, 8, C_TIME); }
    cv_text((int32_t)UI_WIDTH - wm, 1, &UI_SMALL, word, clk_run ? C_KEY : C_OFF, C_BG);
    cv_blit(UI_LEFT, LINE_Y);
}

/* --- the ground: 56 x 15 cells, 4 px each (HOME paints them in their colours, SEQ by their notes) */
static void draw_ants(uint32_t now)
{
    uint32_t n = pat.ants;
    for (uint32_t i = 0; i < n && i < ANTS; i++) {
        int32_t x = (int32_t)ant_vx[i] * 4, y = (int32_t)ant_vy[i] * 4, d = ant_vd[i];
        uint8_t sung = now - sang_at[i] < 110u;
        if (sung) cv_rect(x - 1, y - 1, 5, 5, C_KEY);
        cv_rect(x, y, 3, 3, C_ANT);
        cv_pset(x + 1 + DX[d] * 2, y + 1 + DY[d] * 2, C_ANT);   /* its head: the way it goes */
    }
}

static void draw_ground_pic(uint32_t now)
{
    cv_begin(BOX_W, PIC_H, C_BG);
    for (uint32_t y = 0; y < GH; y++)
        for (uint32_t x = 0; x < GW; x++) {
            uint8_t c = ground[y][x] & 3u;
            if (c) cv_rect((int32_t)x * 4, (int32_t)y * 4, 3, 3, paint_c[c]);
            else cv_pset((int32_t)x * 4 + 1, (int32_t)y * 4 + 1, seed_at[y][x] ? C_AMBER_DIM : C_RULE);   /* (a seed's
                                                 * cell, walked bare: still an octave up) */
        }
    draw_ants(now);
    cv_blit(BOX_X, PIC_Y);
}

static void draw_home_rows(void)
{
    char v[24];
    fmt(v, pat.ants, "");
    ui_row(0, "ANTS", v, C_ANT);
    put_str(put_str(put_str(v, rules[pat.rule].name), " "), rules[pat.rule].code);
    ui_row(1, "RULE", v, C_AMBER);
    ui_row(2, "STEP", step_names[pat.step], C_TIME);
    ui_row(3, "SEED", seed_names[pat.seed], C_AMBER);
}

/* --- SEQ: every cell coloured by its note (the root in amber); the ants walk on it */
static void draw_map_pic(uint32_t now)
{
    int32_t lo = 1 << 30, hi = -(1 << 30);
    for (uint32_t y = 0; y < GH; y++) for (uint32_t x = 0; x < GW; x++) {
        int32_t p = cell_pitch[y][x];
        if (p < lo) lo = p;
        if (p > hi) hi = p;
    }
    int32_t span = hi - lo > 0 ? hi - lo : 1;
    uint8_t root = scale_root_pc();
    cv_begin(BOX_W, PIC_H, C_BG);
    for (uint32_t y = 0; y < GH; y++)
        for (uint32_t x = 0; x < GW; x++) {
            int32_t p = cell_pitch[y][x], note = (p + 128) >> 8;
            uint32_t k = (uint32_t)(1 + (p - lo) * 9 / span);   /* low notes dark, high ones bright */
            uint16_t c = (uint32_t)(note % 12) == root ? C_AMBER_DIM : mix565(C_BG, C_TIME, k);
            if (seed_at[y][x]) c = mix565(c, C_KEY, 10u);   /* the seed's cells: an octave up, in chalk */
            cv_rect((int32_t)x * 4, (int32_t)y * 4, 3, 3, c);
        }
    draw_ants(now);
    cv_blit(BOX_X, PIC_Y);
}

static void draw_seq_rows(void)
{
    char v[20];
    fmt(v, pat.across, pat.across == 1 ? " STEP" : " STEPS");
    ui_row(0, "ACROSS", v, C_TIME);
    fmt(v, pat.up, pat.up == 1 ? " STEP" : " STEPS");
    ui_row(1, "UP", v, C_TIME);
    fmt(v, pat.range, " OCT");
    ui_row(2, "RANGE", v, C_TIME);
    ui_row(3, "SING ON", sing_names[pat.sing], C_AMBER);
}

/* --- ARP: what the ants sang (height: the note), and the chord held */
static void draw_arp_pic(void)
{
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    for (uint32_t i = 0; i < 16u; i++) {
        int32_t x = (int32_t)UI_WIDTH - 13 - (int32_t)i * 13;
        if (i >= arp_hist_n) { cv_rect(x, 40, 10, 1, C_RULE); continue; }
        int32_t y = 40 - arp_hist[i] * 34 / 255;
        cv_rect(x, y, 10, 3, i == 0 ? C_AMBER : C_AMBER_DIM);
    }
    char b[48], *p = b;
    *p = 0;
    if (!held_n) put_str(b, arp.on ? "HOLD KEYS: A CHORD" : "ARP OFF: PRESS ARP AGAIN");
    else for (uint32_t i = 0; i < held_n && i < 6u; i++) {
        p = pitch_name(p, scale_step_pitch(scale_note_step(held[i])));
        *p++ = ' '; *p = 0;
    }
    cv_text(0, 47, &UI_SMALL, b, held_n ? C_AMBER : C_OFF, C_BG);
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_arp_rows(void)
{
    char v[12];
    ui_row(0, "LATCH", arp.latch ? "ON" : "OFF", C_AMBER);
    fmt(v, arp.spread, " OCT");
    ui_row(1, "SPREAD", v, C_AMBER);
    ui_row(2, "PICK", pick_names[arp.pick], C_AMBER);
    fmt(v, arp.chance, "%");
    ui_row(3, "CHANCE", v, C_AMBER);
}

/* --- EDIT: the landscape (amber high, blue low) and the first ant's orbit over it */
static void draw_terrain_pic(void)
{
    cv_begin(BOX_W, PIC_H, C_BG);
    for (uint32_t ty = 0; ty < TH; ty++)
        for (uint32_t tx = 0; tx < TW; tx++) {
            int32_t h = terr_to[ty][tx];
            uint32_t k = (uint32_t)clampi((h + 16000) * 15 / 32000, 0, 15);   /* low: dark, high: amber */
            uint16_t c = mix565(C_BG, C_AMBER, k * 11u / 15u);
            int32_t x0 = (int32_t)(tx * BOX_W / TW), x1 = (int32_t)((tx + 1u) * BOX_W / TW);
            int32_t y0 = (int32_t)(ty * PIC_H / TH), y1 = (int32_t)((ty + 1u) * PIC_H / TH);
            cv_rect(x0, y0, x1 - x0, y1 - y0, c);
        }
    /* the orbit, as the first ant's next note draws it */
    uint32_t q8 = (uint32_t)pat.drift * 256u / 100u;
    int32_t ddx = (int32_t)(cell_u32(ant_vx[0], GW) - 0x80000000u), ddy = (int32_t)(cell_u32(ant_vy[0], GH) - 0x80000000u);
    uint32_t cx = 0x80000000u + (uint32_t)(int32_t)(((int64_t)ddx * q8) >> 8);
    uint32_t cy = 0x80000000u + (uint32_t)(int32_t)(((int64_t)ddy * q8) >> 8);
    int32_t sz = clampi((int32_t)pat.size * 327, 0, 32767), R = 1300 + mul15(sz, 64200), Ry = R * 15 / 4 > 65535 ? 65535 : R * 15 / 4;
    for (uint32_t i = 0; i < 160u; i++) {
        uint32_t ph = i * (0xFFFFFFFFu / 160u);
        int32_t ox, oy;
        orbit_at(pat.orbit % ORBITS, ph, &ox, &oy);
        uint32_t X = cx + (uint32_t)(ox * R), Y = cy + (uint32_t)(oy * Ry);
        if (pat.orbit == ORB_LINE) X = cx + ph;
        cv_pset((int32_t)(((uint64_t)X * BOX_W) >> 32), (int32_t)(((uint64_t)Y * PIC_H) >> 32), C_KEY);
    }
    cv_rect((int32_t)(((uint64_t)cx * BOX_W) >> 32) - 1, (int32_t)(((uint64_t)cy * PIC_H) >> 32) - 1, 3, 3, C_ANT);
    cv_blit(BOX_X, PIC_Y);
}

static void draw_edit_rows(void)
{
    char v[16];
    fmt(v, pat.size, "%");
    ui_row(0, "SIZE", v, C_AMBER);
    fmt(v, pat.ground, "%");
    ui_row(1, "GROUND", v, C_AMBER);
    ui_row(2, "SMOOTH", smooth_names[pat.smooth % 5u], C_AMBER);
    fmt(v, pat.drift, "%");
    ui_row(3, "DRIFT", v, C_ANT);
}

/* --- ENV: a footstep on each colour (ACCENT: how much louder the later colours are) */
static uint32_t isqrt32(uint32_t x)
{
    uint32_t r = 0, bit = 1u << 30;
    while (bit > x) bit >>= 2;
    while (bit) {
        if (x >= r + bit) { x -= r + bit; r = (r >> 1) + bit; } else r >>= 1;
        bit >>= 2;
    }
    return r;
}

static void draw_env_pic(void)
{
    int32_t a = (int32_t)isqrt32((uint32_t)(env_ms[pat.attack] + 40) * 64u), d = (int32_t)isqrt32((uint32_t)(env_ms[pat.decay] + 40) * 64u);
    int32_t W = (int32_t)UI_WIDTH - 10, xa = 4 + a * W / (a + d + 40), xd = 4 + (a + d) * W / (a + d + 40);
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    cv_rect(0, 56, UI_WIDTH, 1, C_RULE);
    for (int32_t c = 1; c <= 3; c++) {
        int32_t vel = 100 - (int32_t)pat.accent * (3 - c) * 7 / 30;   /* % */
        int32_t top = 56 - vel * 48 / 100;
        cv_line(4, 56, xa, top, paint_c[c]);
        int32_t px = xa, py = top;
        for (int32_t x = xa + 1; x <= xd; x++) {    /* the decay: a curve */
            int32_t f = (x - xa) * 256 / (xd - xa > 0 ? xd - xa : 1), g = (256 - f) * (256 - f) / 256;
            int32_t y = 56 - (56 - top) * g / 256;
            cv_line(px, py, x, y, paint_c[c]);
            px = x; py = y;
        }
    }
    cv_disc(4, 56, 3, C_ANT);                   /* the footstep */
    if (pat.bloom) {                            /* BLOOM: the orbit opens at the step, then closes */
        int32_t r = 3 + (int32_t)pat.bloom * 9 / 100;
        for (uint32_t i = 0; i < 48u; i++) {
            uint32_t ph = i << 26;
            cv_pset((int32_t)UI_WIDTH - 16 + sinq(ph + 0x40000000u) * r / 32768, 14 + sinq(ph) * r / 32768, C_KEY);
        }
        cv_disc((int32_t)UI_WIDTH - 16, 14, 1, C_ANT);
    }
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_env_rows(void)
{
    char v[12];
    fmt_ms(v, env_ms[pat.attack]);
    ui_row(0, "ATTACK", v, C_AMBER);
    fmt_ms(v, env_ms[pat.decay]);
    ui_row(1, "DECAY", v, C_AMBER);
    fmt(v, pat.accent, "%");
    ui_row(2, "ACCENT", v, C_AMBER);
    fmt(v, pat.bloom, "%");
    ui_row(3, "BLOOM", v, C_AMBER);
}

/* --- LFO: the wave, and where it is now */
static void draw_lfo_pic(void)
{
    static const int16_t held_v[] = {18000, -11000, 29000, 7000, -28000, 13000, -3000, 24000};
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    cv_rect(0, 30, UI_WIDTH, 1, C_RULE);
    int32_t depth = pat.lfo_depth, prev = 30;
    for (uint32_t x = 0; x < UI_WIDTH; x++) {
        uint32_t ph = x * (65536u * 2u / UI_WIDTH) % 65536u;
        int32_t s, m = (int32_t)ph - 32768, a = m < 0 ? -m : m;
        if (pat.lfo_wave == LW_SINE) s = m * (32768 - a) >> 13;
        else if (pat.lfo_wave == LW_TRIANGLE) s = 32767 - 2 * a;
        else if (pat.lfo_wave == LW_RANDOM) s = held_v[x * 8u / UI_WIDTH];
        else { int32_t i = (int32_t)(x * 8u / UI_WIDTH), f = (int32_t)(x * 8u % UI_WIDTH) * 256 / (int32_t)UI_WIDTH;
               s = held_v[i] + (held_v[(i + 1) & 7] - held_v[i]) * f / 256; }
        s = clampi(s, -32767, 32767);
        int32_t y = 30 - s * depth * 26 / (32768 * 100);
        if (x) cv_line((int32_t)x - 1, prev, (int32_t)x, y, depth ? C_AMBER : C_AMBER_DIM);
        prev = y;
    }
    int32_t px = (int32_t)((lfo_ph >> 16) * (UI_WIDTH / 2u) >> 16);
    int32_t py = 30 - lfo_now * depth * 26 / (32768 * 100);
    cv_disc(px, py, 3, C_KEY);
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_lfo_rows(void)
{
    char v[12];
    ui_row(0, "TARGET", lfo_target_names[pat.lfo_target % LFO_TARGETS], C_AMBER);
    ui_row(1, "WAVE", lfo_wave_names[pat.lfo_wave], C_AMBER);
    uint16_t h = lfo_hundredths[pat.lfo_rate];
    char *p = put_num(v, h / 100u, 0);
    *p++ = '.';
    *p++ = (char)('0' + h % 100u / 10u);
    *p++ = (char)('0' + h % 10u);
    put_str(p, " HZ");
    ui_row(2, "RATE", v, C_AMBER);
    fmt(v, pat.lfo_depth, "%");
    ui_row(3, "DEPTH", v, C_AMBER);
}

/* --- FX: a step, its echoes, the room after it */
static void draw_fx_pic(void)
{
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    cv_rect(0, 56, UI_WIDTH, 1, C_RULE);
    int32_t tail = 20 + pat.reverb * 19 / 10;
    for (int32_t x = 0; x < tail && x < (int32_t)UI_WIDTH - 6; x++) {
        int32_t h = (int32_t)pat.reverb * 30 / 100 * (tail - x) / tail;
        if (h > 0) cv_rect(6 + x, 56 - h, 1, h, C_GRAPH);
    }
    cv_rect(6, 6, 2, 50, C_KEY);
    int32_t step = 6 + (int32_t)echo_clocks[pat.echo_time] * 2, level = pat.echo * 48 / 100;
    for (int32_t x = 6 + step; x < (int32_t)UI_WIDTH && level > 1; x += step) {
        cv_rect(x, 56 - level, 2, level, C_AMBER);
        level = level * 55 / 100;
    }
    if (pat.chorus)
        for (int32_t x = 0; x < (int32_t)UI_WIDTH; x += 2)
            cv_pset(x, 3 + sinq((uint32_t)x << 26) * (int32_t)pat.chorus / 100 * 2 / 32768, C_TIME_DIM);
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_fx_rows(void)
{
    char v[12];
    fmt(v, pat.reverb, "%");
    ui_row(0, "REVERB", v, C_AMBER);
    fmt(v, pat.echo, "%");
    ui_row(1, "ECHO", v, C_AMBER);
    ui_row(2, "TIME", echo_names[pat.echo_time], C_TIME);
    fmt(v, pat.chorus, "%");
    ui_row(3, "CHORUS", v, C_AMBER);
}

/* --- GLO: the scale, on a piano octave (the root in blue) */
static void draw_scale_pic(void)
{
    static const uint8_t white[] = {0, 2, 4, 5, 7, 9, 11};
    static const uint8_t black[] = {1, 3, 6, 8, 10};
    static const uint8_t black_x[] = {30, 60, 120, 150, 180};
    uint8_t root = scale_root_pc();
    cv_begin(UI_WIDTH, PIC_H, C_BG);
    cv_text(0, 0, &UI_BODY, scales[scale_now.scale].name, C_AMBER, C_BG);
    for (uint32_t i = 0; i < 7; i++) {
        uint16_t c = scale_contains(white[i]) ? (white[i] == root ? C_TIME : C_AMBER) : C_GRAPH;
        cv_rect((int32_t)i * 30, 20, 28, 40, C_RULE);
        cv_rect((int32_t)i * 30 + 1, 21, 26, 38, c);
    }
    for (uint32_t i = 0; i < 5; i++) {
        int32_t x = black_x[i] - 10;
        cv_rect(x, 20, 20, 24, C_RULE);
        cv_rect(x + 1, 20, 18, 23, C_BG);
        uint16_t c = scale_contains(black[i]) ? (black[i] == root ? C_TIME : C_AMBER) : C_BG;
        cv_rect(x + 2, 21, 16, 21, c);
    }
    cv_blit(UI_LEFT, PIC_Y);
}

static void draw_glo_rows(void)
{
    char v[16];
    fmt(v, scale_now.size, scale_now.size == 1 ? " NOTE" : " NOTES");
    ui_row(0, "SCALE", v, C_AMBER);
    uint32_t bpm10 = clock_bpm10();
    fmt(v, (int32_t)((bpm10 + 5u) / 10u), clk_ext() ? " MIDI" : " BPM");
    ui_row(1, "TEMPO", v, C_TIME);
    ui_row(2, "SYNC", set.sync_int ? "INTERNAL" : "AUTO", C_TIME);
    if (set.ch) fmt(v, set.ch, "");
    ui_row(3, "MIDI CH", set.ch ? v : "OMNI", C_KEY);
}

/* ------------------------------------------------------------------------------------- knobs and keys */
static int32_t step_clamp(int32_t v, int32_t d, int32_t lo, int32_t hi)
{
    v += d;
    return v < lo ? lo : v > hi ? hi : v;
}

static uint8_t pct(uint8_t v, int32_t d) { return (uint8_t)step_clamp(v, d * 2, 0, 100); }

static void edit_page(uint8_t page, const int32_t s[4])
{
    switch (page) {
    case P_HOME:
        pat.ants = (uint8_t)step_clamp(pat.ants, s[0], 1, ANTS);
        pat.rule = (uint8_t)step_clamp(pat.rule, s[1], 0, RULES - 1);
        pat.step = (uint8_t)step_clamp(pat.step, s[2], 0, STEPS - 1);
        if (s[3]) { pat.seed = (uint8_t)step_clamp(pat.seed, s[3], 0, SEEDS - 1); say("REC: HATCH ON THIS SEED"); }
        break;
    case P_SEQ:
        pat.across = (uint8_t)step_clamp(pat.across, s[0], 0, 12);
        pat.up = (uint8_t)step_clamp(pat.up, s[1], 0, 12);
        pat.range = (uint8_t)step_clamp(pat.range, s[2], 1, 4);
        pat.sing = (uint8_t)step_clamp(pat.sing, s[3], 0, SINGS - 1);
        break;
    case P_ARP:
        if (s[0] && arp.latch != (s[0] > 0)) {
            arp.latch = s[0] > 0;
            if (!arp.latch) {                   /* LATCH off: the chord is the keys held now */
                held_n = 0; latched = 0;
                for (uint32_t k = 0; k < 27u && held_n < 16u; k++) if (fm1_in.notes >> k & 1u) held[held_n++] = (uint8_t)(KEY0 + k);
            }
        }
        arp.spread = (uint8_t)step_clamp(arp.spread, s[1], 1, 4);
        arp.pick = (uint8_t)step_clamp(arp.pick, s[2], 0, PICKS - 1);
        arp.chance = (uint8_t)step_clamp(arp.chance, s[3] * 5, 0, 100);
        break;
    case P_EDIT:
        pat.size = pct(pat.size, s[0]);
        pat.ground = pct(pat.ground, s[1]);
        pat.smooth = (uint8_t)step_clamp(pat.smooth, s[2], 0, 4);
        pat.drift = pct(pat.drift, s[3]);
        break;
    case P_ENV:
        pat.attack = (uint8_t)step_clamp(pat.attack, s[0], 0, ENV_STEPS - 1);
        pat.decay = (uint8_t)step_clamp(pat.decay, s[1], 0, DECAY_MAX);
        pat.accent = pct(pat.accent, s[2]);
        pat.bloom = pct(pat.bloom, s[3]);
        break;
    case P_LFO:
        pat.lfo_target = (uint8_t)step_clamp(pat.lfo_target, s[0], 0, LFO_TARGETS - 1);
        pat.lfo_wave = (uint8_t)step_clamp(pat.lfo_wave, s[1], 0, LFO_WAVES - 1);
        pat.lfo_rate = (uint8_t)step_clamp(pat.lfo_rate, s[2], 0, LFO_RATES - 1);
        pat.lfo_depth = pct(pat.lfo_depth, s[3]);
        break;
    case P_FX:
        pat.reverb = pct(pat.reverb, s[0]);
        pat.echo = pct(pat.echo, s[1]);
        pat.echo_time = (uint8_t)step_clamp(pat.echo_time, s[2], 0, ECHOS - 1);
        pat.chorus = pct(pat.chorus, s[3]);
        break;
    default:
        if (s[0]) { scale_select((uint8_t)step_clamp(scale_now.scale, s[0], 0, SCALE_COUNT - 1)); pat.scale = scale_now.scale; }
        if (s[1]) { set.tempo = (uint8_t)step_clamp(set.tempo, s[1], 40, 240); clk_tempo = set.tempo; }
        if (s[2]) { set.sync_int = s[2] > 0; clock_set_sync(set.sync_int); }
        if (s[3]) { set.ch = (uint8_t)step_clamp(set.ch, s[3], 0, 16); midi_ch = set.ch; }
        break;
    }
}

static void update_fx(void)
{
    static uint32_t previous = ~0u;
    uint32_t bpm = (clock_bpm10() + 5u) / 10u;
    if (bpm < 30u) bpm = 30u;
    if (bpm > 300u) bpm = 300u;
    uint32_t state = bpm | (uint32_t)pat.echo << 9 | (uint32_t)pat.reverb << 16 | (uint32_t)pat.echo_time << 23 | (uint32_t)pat.chorus << 26;
    chorus_amount = pat.chorus / 2u;
    if (state == previous) return;
    previous = state;
    delay_configure(pat.echo != 0, delay_tempo_samples(bpm, echo_clocks[pat.echo_time]), 55, pat.echo);
    hall_configure(pat.reverb != 0, 84, 40, pat.reverb);
}

/* Hold EDIT on the EDIT page: a random sound. The orbit, EDIT, ENV and LFO, each within a range that still sounds
 * like an instrument (mostly short attacks, decays that ring, a gentle LFO); the colony, the map and FX stay. */
static uint32_t dice = 0x6D2B79F5u;
static uint32_t roll(uint32_t lo, uint32_t hi) { return lo + rnd_next(&dice) % (hi - lo + 1u); }

static void sound_random(void)
{
    dice ^= fm1_ms * 2654435761u ^ (uint32_t)world_t;
    pat.orbit = (uint8_t)roll(0, ORBITS - 1u);
    pat.size = (uint8_t)roll(5, 35) * 2u;       /* 10 .. 70 % */
    pat.ground = (uint8_t)roll(10, 50) * 2u;    /* 20 .. 100 % */
    pat.smooth = (uint8_t)roll(0, 4);
    pat.drift = (uint8_t)roll(0, 50) * 2u;
    pat.attack = (uint8_t)(roll(0, 3) ? roll(0, 8) : roll(9, 18));   /* mostly 0 .. 100 ms, now and then up to 1 s */
    pat.decay = (uint8_t)roll(10, DECAY_MAX);   /* 200 ms .. 10 s */
    pat.accent = (uint8_t)roll(0, 40) * 2u;
    pat.bloom = (uint8_t)roll(0, 40) * 2u;
    pat.lfo_target = (uint8_t)roll(0, LFO_TARGETS - 1u);
    pat.lfo_wave = (uint8_t)roll(0, LFO_WAVES - 1u);
    pat.lfo_rate = (uint8_t)roll(0, 44);        /* 0.01 .. ~2 Hz */
    pat.lfo_depth = (uint8_t)(pat.lfo_target == LFO_PITCH ? roll(0, 10) : roll(0, 30)) * 2u;
}

static uint8_t button_page(uint32_t button)
{
    switch (button) {
    case BTN_HOME: return P_HOME;
    case BTN_ARP: return P_ARP;
    case BTN_SEQ: return P_SEQ;
    case BTN_EDIT: return P_EDIT;
    case BTN_ENV: return P_ENV;
    case BTN_LFO: return P_LFO;
    case BTN_FX: return P_FX;
    case BTN_GLO: return P_GLO;
    default: return 0xFF;
    }
}

static const char *page_title(uint8_t page, char *b)
{
    switch (page) {
    case P_HOME: return "HOME  COLONY";
    case P_ARP: return arp.on ? "ARP  CHORD" : "ARP  OFF";
    case P_SEQ: return "SEQ  HARMONY";
    case P_EDIT: put_str(put_str(b, "EDIT  "), orbit_names[pat.orbit % ORBITS]); return b;
    case P_ENV: return "ENV  STEPS";
    case P_LFO: return "LFO  WIND";
    case P_FX: return "FX";
    default: return "GLO  SCALE";
    }
}

static uint16_t title_colour(uint8_t page)
{
    switch (page) {
    case P_HOME: return C_KEY;
    case P_SEQ: return C_TIME;
    case P_EDIT: return C_AMBER;
    default: return C_AMBER;
    }
}

static void draw_footer(uint8_t page)
{
    static char b[48];
    if (message) { ui_footer(message, C_AMBER); return; }
    switch (page) {
    case P_HOME:
        if (!clk_run) ui_footer("PLAY: LET THEM WALK", C_TIME);
        else if (frozen) ui_footer("SEL: LET THEM PAINT AGAIN", C_TIME);
        else ui_footer("REC: HATCH   SEL: FREEZE", C_OFF);
        break;
    case P_SEQ: ui_footer("A CELL'S NOTE: ACROSS + UP", C_OFF); break;
    case P_ARP: ui_footer(arp.on ? "KEYS: THE CHORD THEY SING" : "ARP: PRESS AGAIN TO TURN ON", C_OFF); break;
    case P_EDIT: ui_footer("ALGO: ORBIT  HOLD EDIT: RANDOM", C_OFF); break;
    case P_ENV: ui_footer("EVERY FOOTSTEP", C_OFF); break;
    case P_LFO: {
        static const char *const what[LFO_TARGETS] = {"THE ORBITS GROW AND SHRINK", "THE ORBITS SLIDE SIDEWAYS",
                                                      "THE PITCH SWAYS", "THE LOUDNESS SWAYS"};
        ui_footer(what[pat.lfo_target % LFO_TARGETS], C_OFF);
        break;
    }
    case P_FX: ui_footer("IN PARALLEL", C_OFF); break;
    default: {
        char *p = put_str(b, "KEYS: ROOT ");
        p = put_str(p, note_names[scale_root_pc()]);
        put_str(p, "   OCT: OCTAVE");
        ui_footer(b, C_OFF);
    }
    }
}

#define MIX(v) (h = (h ^ (uint32_t)(v)) * 16777619u)
static uint32_t page_state(uint8_t page, uint32_t now)
{
    uint32_t h = 2166136261u;
    switch (page) {
    case P_HOME: case P_SEQ:
        MIX(world_ver); MIX(pat.ants); MIX(now / 40u);
        if (page == P_SEQ) { MIX(pat.across); MIX(pat.up); MIX(pat.range); MIX(scale_now.scale); MIX(scale_now.transpose); MIX(chord_n); MIX(arp.on); }
        break;
    case P_EDIT: MIX(pat.orbit); MIX(pat.size); MIX(pat.drift); MIX(pat.ground); MIX(pat.smooth); MIX(world_ver / 4u); break;
    case P_ENV: MIX(pat.attack); MIX(pat.decay); MIX(pat.accent); MIX(pat.bloom); break;
    case P_LFO: MIX(pat.lfo_wave); MIX(pat.lfo_depth); MIX(pat.lfo_target); MIX(now / 40u); break;
    case P_ARP:
        MIX(arp_hist_n); MIX(arp_hist[0]); MIX(arp_hist[1]); MIX(arp.on); MIX(held_n);
        for (uint32_t i = 0; i < held_n; i++) MIX(held[i]);
        MIX(scale_now.octave); MIX(scale_now.scale); MIX(scale_now.transpose);
        break;
    case P_FX: MIX(pat.reverb); MIX(pat.echo); MIX(pat.echo_time); MIX(pat.chorus); break;
    default: MIX(scale_now.scale); MIX(scale_now.transpose); break;
    }
    return h;
}

static uint32_t row_state(uint8_t page)
{
    uint32_t h = 2166136261u;
    switch (page) {
    case P_HOME: MIX(pat.ants); MIX(pat.rule); MIX(pat.step); MIX(pat.seed); break;
    case P_SEQ: MIX(pat.across); MIX(pat.up); MIX(pat.range); MIX(pat.sing); break;
    case P_EDIT: MIX(pat.size); MIX(pat.ground); MIX(pat.smooth); MIX(pat.drift); break;
    case P_ENV: MIX(pat.attack); MIX(pat.decay); MIX(pat.accent); MIX(pat.bloom); break;
    case P_LFO: MIX(pat.lfo_target); MIX(pat.lfo_wave); MIX(pat.lfo_rate); MIX(pat.lfo_depth); break;
    case P_ARP: MIX(arp.latch); MIX(arp.spread); MIX(arp.pick); MIX(arp.chance); break;
    case P_FX: MIX(pat.reverb); MIX(pat.echo); MIX(pat.echo_time); MIX(pat.chorus); break;
    default: MIX(scale_now.scale); MIX(set.tempo); MIX(set.sync_int); MIX(set.ch); MIX(clock_bpm10()); MIX(clk_ext()); break;
    }
    return h;
}
#undef MIX

static void draw_page(uint8_t page, uint8_t whole, uint32_t now)
{
    switch (page) {
    case P_HOME: draw_ground_pic(now); if (whole) draw_home_rows(); break;
    case P_SEQ: draw_map_pic(now); if (whole) draw_seq_rows(); break;
    case P_ARP: draw_arp_pic(); if (whole) draw_arp_rows(); break;
    case P_EDIT: draw_terrain_pic(); if (whole) draw_edit_rows(); break;
    case P_ENV: draw_env_pic(); if (whole) draw_env_rows(); break;
    case P_LFO: draw_lfo_pic(); if (whole) draw_lfo_rows(); break;
    case P_FX: draw_fx_pic(); if (whole) draw_fx_rows(); break;
    default: draw_scale_pic(); if (whole) draw_glo_rows(); break;
    }
}

static uint32_t idle_ok;
static void app_idle(void) { if (idle_ok) midi_poll(1); }

/* ----------------------------------------------------------------------------------------------- the loop */
static void app_main(void)
{
    uint8_t page = P_HOME, drawn_page = 0xFF, key_on[27] = {0};
    uint32_t prev_keys = 0, drawn_state = 0, drawn_rows = 0, drawn_footer = ~0u, title_state = ~0u, drawn_line = ~0u;
    uint32_t flash_until[27] = {0}, off_at[16] = {0}, hatched_at = 0;
    uint8_t off_note[16] = {0}, arp_was_page = 0, edit_held = 0;
    uint32_t edit_down = 0;
    static char msg[40];
#if !BOARD_SMK37
    uint8_t battery_drawn = UINT8_MAX;
#endif

    if (settings_load(&set, sizeof set) || set.ver != SET_VER) { set.ver = SET_VER; set.oct = 0; set.ch = 0; set.sync_int = 0; set.tempo = 96; }
    if ((int8_t)set.oct < -2 || (int8_t)set.oct > 2) set.oct = 0;
    if (set.ch > 16u) set.ch = 0;
    if (set.tempo < 40u || set.tempo > 240u) set.tempo = 96;
    set.sync_int &= 1u;
    scale_now.octave = (int8_t)set.oct;
    midi_ch = set.ch;
    clk_tempo = set.tempo;
    clock_set_sync(set.sync_int);
    presets_init();
    ants_place();
    preset_load(0);
    chord_build();
    map_build();

    draw_boot_logo();
    uint32_t boot_started = fm1_ms;
    /* (nothing plays at power-on: PLAY lets them walk) */
    for (;;) {
        if (mirror_poll()) drawn_page = 0xFF;   /* FM-1 Mirror opened: everything again */
        if (backup_poll()) { platform_poll(); continue; }
        platform_poll();
        clock_poll();
        uint32_t now = fm1_ms, rel;
        uint32_t b = fm1_input_edges(&rel);
        uint32_t held_b = fm1_in.buttons;

        /* ---- buttons */
        for (uint32_t i = 0; i < 14; i++) {
            uint32_t bit = 1u << i;
            if (b & bit) {
                uint8_t to = button_page(i);
                if (to != 0xFF) {
                    if (i == BTN_ARP) arp_was_page = page == P_ARP;
                    if (i == BTN_EDIT) { edit_held = page == P_EDIT; edit_down = now; }   /* (only from EDIT itself) */
                    page = to;
                }
                if (i == BTN_OCT_DOWN || i == BTN_OCT_UP) {
                    int32_t d = i == BTN_OCT_UP ? 1 : -1;
                    uint32_t other = i == BTN_OCT_UP ? BTN_OCT_DOWN : BTN_OCT_UP;
                    if (page == P_GLO) scale_now.transpose = (int8_t)step_clamp(scale_now.transpose, d * 12, -31, 43);
                    else if (!(held_b >> other & 1u)) {   /* alone: the way time runs. (With the other held: twice
                                                           * as fast that way, see below; the way stays) */
                        if (time_dir != d) say(d > 0 ? "TIME RUNS FORWARD" : "TIME RUNS BACKWARD");
                        time_dir = d;
                    } else say(time_dir > 0 ? "FORWARD, TWICE AS FAST" : "BACKWARD, TWICE AS FAST");
                }
                if (i == BTN_SEL) {             /* SEL: freeze the paint (the walk closes into a loop), or thaw it */
                    frozen = !frozen;
                    say(frozen ? "FROZEN: THEIR WALK LOOPS" : "THEY PAINT AGAIN");
                }
                if (i == BTN_REC) {             /* REC: a new colony hatches on the SEED */
                    hatch_w++;
                    hatched_at = now;
                    say("A NEW COLONY HATCHES");
                }
                if (i == BTN_PLAY) clock_toggle();
            }
            if ((rel & bit) && i == BTN_ARP && arp_was_page) {
                arp.on = !arp.on;
                held_n = 0;
                latched = 0;
                live_panic();
                say(arp.on ? "THE ANTS SING YOUR CHORD" : "THE ANTS SING THE MAP");
            }
        }
        /* EDIT held 300 ms on the EDIT page: a random sound (once a hold) */
        if (edit_held && !(held_b >> BTN_EDIT & 1u)) edit_held = 0;
        if (edit_held == 1u && page == P_EDIT && now - edit_down >= RANDOM_MS) {
            edit_held = 2;
            sound_random();
            say("A RANDOM SOUND");
        }
        /* OCT- and OCT+ held together: time runs twice as fast, the way the first one pressed set */
        time_fast = page != P_GLO && (held_b >> BTN_OCT_DOWN & 1u) && (held_b >> BTN_OCT_UP & 1u);
        /* SAVE: the whole instrument, and the colony as it stands, into a USER preset */
        if (b & (1u << BTN_SAVE)) {
            uint8_t slot = preset >= FACTORY ? preset : last_user;
            set.oct = (uint8_t)scale_now.octave;
            int ok = preset_save(slot);
            settings_save(&set, sizeof set);
            preset = last_user = slot;
            char *p = put_str(msg, ok ? "SAVED TO " : "COULD NOT SAVE ");
            preset_name(slot, p);
            say(msg);
        }

        /* ---- encoders: SELECT walks time by hand (GLO: the scale); ALGORITHM the orbit; PRESETS */
        int32_t e = fm1_enc_take(ENC_SELECT);
        if (e) {
            if (page == P_GLO) { scale_select((uint8_t)step_clamp(scale_now.scale, e, 0, SCALE_COUNT - 1)); pat.scale = scale_now.scale; }
            else if (e > 0) scrub_fw += (uint32_t)e;
            else scrub_bw += (uint32_t)-e;
        }
        e = fm1_enc_take(ENC_ALGORITHM);
        if (e) {
            pat.orbit = (uint8_t)step_clamp(pat.orbit, e, 0, ORBITS - 1);
            put_str(put_str(msg, "THE ORBIT: "), orbit_names[pat.orbit]);
            say(msg);
        }
        e = fm1_enc_take(ENC_PRESETS);
        if (e) {
            preset_load((uint8_t)step_clamp(preset, e, 0, FACTORY + USERS - 1));
            char *p = put_str(msg, "PRESET ");
            preset_name(preset, p);
            say(msg);
        }
        int32_t steps[4];
        for (uint32_t k = 0; k < 4; k++) steps[k] = fm1_enc_take(ENC_KNOB1 + k);
        if (steps[0] || steps[1] || steps[2] || steps[3]) edit_page(page, steps);

        /* ---- the keys never sound: on the ARP page they hold the chord the ants sing, everywhere else they set the
         * root */
        uint32_t keys = fm1_in.notes, down = keys & ~prev_keys, up = prev_keys & ~keys;
        prev_keys = keys;
        for (uint32_t k = 0; k < 27u; k++) {
            uint32_t bit = 1u << k;
            uint8_t note = (uint8_t)(KEY0 + k);
            if (down & bit) {
                if (page == P_ARP) { key_on[k] = note; chord_key(1, note); }
                else { root_from(note); key_on[k] = 0; }
            }
            if ((up & bit) && key_on[k]) { chord_key(0, key_on[k]); key_on[k] = 0; }
        }

        midi_poll(0);
        chord_build();
        map_build();
        terrain_build(now);
        update_fx();
        if (message && now - message_at > 1500u) message = 0;

        /* ---- what the ants sang: MIDI out, the key lights, the screen */
        int32_t base = (int32_t)KEY0 + 12 * scale_now.octave;
        while (sung_r != sung_w) {
            sung_t s = sungq[sung_r % 32u];
            sung_r++;
            uint32_t ch = midi_out_ch();
            for (uint32_t i = 0; i < 16u; i++)  /* a note still on goes off first */
                if (off_at[i] && off_note[i] == s.note) { midi_send(0x80u | ch, s.note, 0); off_at[i] = 0; }
            midi_send(0x90u | ch, s.note, s.vel);
            for (uint32_t i = 0; i < 16u; i++) if (!off_at[i]) { off_at[i] = now + 120u; off_note[i] = s.note; break; }
            if (s.who < ANTS) sang_at[s.who] = now;
            int32_t k = (int32_t)s.note - base;
            if (k >= 0 && k < 27) flash_until[k] = now + 90u;
            for (uint32_t i = 15; i > 0; i--) arp_hist[i] = arp_hist[i - 1];
            arp_hist[0] = (uint8_t)clampi(((int32_t)s.note - 45) * 255 / 36, 0, 254);
            if (arp_hist_n < 16) arp_hist_n++;
        }
        for (uint32_t i = 0; i < 16u; i++)
            if (off_at[i] && (int32_t)(now - off_at[i]) >= 0) { midi_send(0x80u | midi_out_ch(), off_note[i], 0); off_at[i] = 0; }

        /* ---- LEDs */
        uint32_t lit = page == P_ARP ? keys : 0u;
        if (page != P_ARP) {                    /* the root, and the notes the ants sing as they sing them */
            int32_t k = scale_now.transpose + 7;
            while (k > 26) k -= 12;
            while (k < 0) k += 12;
            lit = 1u << k;
        }
        {
            for (uint32_t k = 0; k < 27u; k++) if ((int32_t)(flash_until[k] - now) > 0) lit |= 1u << k;
        }
        for (uint32_t k = 0; k < 27u; k++) fm1_led_key(KEY_LED(k), lit >> k & 1u);
        static uint32_t beat_seen, beat_t;
        if (clk_ui_beat != beat_seen) { beat_seen = clk_ui_beat; beat_t = now; }
        uint8_t blink = !time_fast || (now / 80u) % 2u;
        if (page == P_GLO) {
            fm1_led_key(BTN_OCT_DOWN, scale_now.transpose < -6);
            fm1_led_key(BTN_OCT_UP, scale_now.transpose > 5);
        } else {
            fm1_led_key(BTN_OCT_DOWN, time_dir < 0 && blink);
            fm1_led_key(BTN_OCT_UP, time_dir > 0 && blink);
        }
        fm1_led_key(BTN_PLAY, clk_run && now - beat_t > 90u);
        fm1_led_key(BTN_REC, now - hatched_at < 400u && (now / 100u) % 2u == 0);
        fm1_led_key(BTN_ARP, arp.on);
        fm1_led_key(BTN_SEL, frozen);
        fm1_led_key(BTN_HOME, page == P_HOME);
        fm1_led_key(BTN_SEQ, page == P_SEQ);
        fm1_led_key(BTN_EDIT, page == P_EDIT);
        fm1_led_key(BTN_ENV, page == P_ENV);
        fm1_led_key(BTN_LFO, page == P_LFO);
        fm1_led_key(BTN_FX, page == P_FX);
        fm1_led_key(BTN_GLO, page == P_GLO);
        fm1_led_key(BTN_SAVE, message && message == msg && now - message_at < 1500u && preset >= FACTORY);

        /* ---- the screen */
        if (drawn_page == 0xFF && now - boot_started < BOOT_LOGO_MS && !b && !keys) continue;
        idle_ok = 1;
        uint8_t whole = page != drawn_page;
        if (whole) {
            lcd_fill(0, 0, 240, 240, C_BG);
            ui_divider(40, C_RULE);
            ui_divider(211, C_RULE);
            drawn_page = page;
            drawn_footer = drawn_line = title_state = ~0u;
#if !BOARD_SMK37
            battery_drawn = UINT8_MAX - 1u;
#endif
        }
        uint32_t ts = (uint32_t)page | (uint32_t)arp.on << 4 | (uint32_t)pat.orbit << 5;
        if (ts != title_state) {
            char tb[24];
            lcd_fill(UI_LEFT, 10, UI_WIDTH - UI_BATTERY_WIDTH - 4u, 26, C_BG);
            ui_text(UI_LEFT, 13, UI_WIDTH - UI_BATTERY_WIDTH - 8u, &UI_LARGE, page_title(page, tb), title_colour(page), 0);
            title_state = ts;
        }
        uint8_t time_page = page == P_SEQ || page == P_HOME;
        uint32_t ls = (uint32_t)world_t * 31u ^ painted << 12 ^ (uint32_t)(time_dir + 1) << 24 ^ frozen << 26 ^ clk_run << 27 ^
                      time_fast << 28 ^ (uint32_t)time_page << 29;
        if (ls != drawn_line) { draw_time_line(time_page); drawn_line = ls; }
        uint32_t ps = page_state(page, now), rs = row_state(page);
        if (whole || ps != drawn_state || rs != drawn_rows) {
            draw_page(page, whole || rs != drawn_rows, now);
            drawn_state = ps;
            drawn_rows = rs;
        }
        uint32_t fs = (uint32_t)(uintptr_t)message ^ message_at * 31u ^ (uint32_t)scale_root_pc() << 10 ^ (uint32_t)arp.on << 14 ^
                      clk_run << 15 ^ frozen << 16 ^ (uint32_t)pat.lfo_target << 24;
        if (fs != drawn_footer) { draw_footer(page); drawn_footer = fs; }
#if !BOARD_SMK37
        uint8_t battery_level = read_battery_level(now);
        if (battery_drawn != battery_level) { draw_battery(battery_level); battery_drawn = battery_level; }
#endif
        idle_ok = 0;
    }
}
