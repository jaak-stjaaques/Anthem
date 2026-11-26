/* SPDX-License-Identifier: GPL-3.0-only */
/* ANTHEM's colony and its sound. Everything here runs in the audio interrupt (or is set up for it).
 *
 *   THE COLONY   up to four ants walk a ground of 56 x 15 cells (a torus: off one edge, on at the other). Each cell is
 *                bare or painted in one of three colours. On every step an ant looks at its cell, turns as the RULE
 *                says for that colour (R, L, N: none, U: about), paints the cell the next colour and walks on: a
 *                turmite, Langton's ant and its family. From almost nothing comes a scribble, then order (a square, a
 *                mirror, a highway), then chaos again. Each step an ant sings the note of its cell (SEQ: the map).
 *   TIME         the rule can be run backwards exactly: the ant steps back, un-paints, un-turns. So time has a
 *                direction (OCT- back, OCT+ forward): backwards the colony un-writes itself and sings the same melody
 *                in reverse, all the way back to the moment it hatched (and further, into a past nobody wrote).
 *   FREEZE       the ants stop painting: on a ground that no longer changes their walk soon closes into a loop.
 *   THE SOUND    wave terrain synthesis. The colony's ground, smoothed, is a landscape of heights; each note draws an
 *                ORBIT (circle, eight, rose, diamond, line) over it at the note's frequency and the heights it passes
 *                are the wave. The orbit is centred where the ant stands (DRIFT), so every ant sounds like the ground
 *                under its feet, and as they paint, the landscape and the sound change with the music.
 *
 * Every number is an integer (no FPU). */
#include "anthem_tables.h"

#define NVOICE 8u
#define ANTS 4u
#define GW 56u                                  /* the ground: GW x GH cells */
#define GH 15u
#define TW 64u                                  /* the terrain the orbits read: TW x TH heights (a power of 2 each) */
#define TH 16u
#define Q15 32768
#define SUB 16u                                 /* the control rate: 16 frames (as kit_clock.h's CLK_CTL) */

enum { ORB_CIRCLE, ORB_EIGHT, ORB_ROSE, ORB_DIAMOND, ORB_LINE, ORBITS };
enum { SING_PAINT, SING_BARE, SING_ALL, SINGS };
enum { LFO_SIZE, LFO_SHIFT, LFO_PITCH, LFO_LEVEL, LFO_TARGETS };
enum { LW_SINE, LW_TRIANGLE, LW_RANDOM, LW_DRIFT, LFO_WAVES };
enum { SEED_EMPTY, SEED_DOT, SEED_DASH, SEED_CROSS, SEED_RING, SEED_DUST, SEED_MOSS, SEED_STRIPES, SEEDS };

static const char *const orbit_names[ORBITS] = {"CIRCLE", "EIGHT", "ROSE", "DIAMOND", "LINE"};
static const char *const sing_names[SINGS] = {"PAINT", "BARE", "EVERY STEP"};
static const char *const lfo_target_names[LFO_TARGETS] = {"SIZE", "SHIFT", "PITCH", "LEVEL"};
static const char *const lfo_wave_names[LFO_WAVES] = {"SINE", "TRIANGLE", "RANDOM", "DRIFT"};
static const char *const seed_names[SEEDS] = {"BARE", "ONE DOT", "DASHES", "CROSS", "RING", "DUST", "MOSS", "STRIPES"};
static const char *const smooth_names[5] = {"RAW", "GRAIN", "SOFT", "SMOOTH", "MIST"};

/* the rules: what an ant does on each colour (R right, L left, N straight on, U about) */
typedef struct { const char *name, *code; uint8_t n; int8_t turn[4]; } rule_t;
static const rule_t rules[] = {
    {"LANGTON", "RL",   2, {1, 3, 0, 0}},
    {"SQUARE",  "LLRR", 4, {3, 3, 1, 1}},
    {"MIRROR",  "RLLR", 4, {1, 3, 3, 1}},
    {"CHAOS",   "RLR",  3, {1, 3, 1, 0}},
    {"CLOUD",   "RNNL", 4, {1, 0, 0, 3}},
    {"KNOT",    "LRRR", 4, {3, 1, 1, 1}},
    {"WEAVE",   "LLRL", 4, {3, 3, 1, 3}},
    {"CORAL",   "LRL",  3, {3, 1, 3, 0}},
    {"SPIRAL",  "RRLU", 4, {1, 1, 3, 2}},
};
#define RULES (sizeof rules / sizeof rules[0])
static const uint8_t step_ticks[] = {24, 12, 8, 6, 4, 3};    /* 24 a quarter */
static const char *const step_names[] = {"1/4", "1/8", "1/8 T", "1/16", "1/16 T", "1/32"};
#define STEPS (sizeof step_ticks)
static const uint8_t echo_clocks[] = {3, 6, 9, 12, 18, 24, 36};  /* 24 a quarter */
static const char *const echo_names[] = {"1/32", "1/16", "1/16 D", "1/8", "1/8 D", "1/4", "1/4 D"};
#define ECHOS (sizeof echo_clocks)

/* a patch: everything PRESETS loads and SAVE keeps (the colony itself is kept beside it) */
typedef struct {
    uint8_t orbit, size, ground, smooth, drift;  /* EDIT: the terrain and the orbit */
    uint8_t attack, decay, accent, bloom;       /* ENV: every footstep */
    uint8_t lfo_target, lfo_wave, lfo_rate, lfo_depth;
    uint8_t reverb, echo, echo_time, chorus;    /* FX */
    uint8_t ants, rule, step, seed;             /* HOME: the colony */
    uint8_t across, up, range, sing;            /* SEQ: the map from a cell to its note */
    uint8_t scale, pad[3];
} patch_t;

static patch_t pat;

/* ------------------------------------------------------------------------------------------------ helpers */
static inline int32_t sinq(uint32_t ph)         /* 2^32 a cycle -> -32767..32767 */
{
    uint32_t i = ph >> 22, f = (ph >> 6) & 0xFFFFu;
    int32_t a = sine1k[i], b = sine1k[i + 1u];
    return a + (((b - a) * (int32_t)f) >> 16);
}

static uint32_t inc_of(int32_t p)               /* 1/256 semitones -> phase step a frame */
{
    if (p < 12 * 256) p = 12 * 256;
    if (p > 119 * 256) p = 119 * 256;
    uint32_t n = (uint32_t)p >> 8, f = (uint32_t)p & 255u;
    uint32_t a = inc_c9[n % 12u] >> (10u - n / 12u);
    uint32_t m = n + 1u, b = inc_c9[m % 12u] >> (10u - m / 12u);
    return a + (uint32_t)(((uint64_t)(b - a) * f) >> 8);
}

static inline int32_t mul15(int32_t a, int32_t b) { return (int32_t)(((int64_t)a * b) >> 15); }
static inline int32_t clampi(int32_t v, int32_t lo, int32_t hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint32_t env_step(uint8_t index)         /* the change a frame (Q24) for a full sweep in env_ms[index] */
{
    if (index >= ENV_STEPS) index = ENV_STEPS - 1u;
    uint32_t frames = (uint32_t)env_ms[index] * (FS / 100u) / 10u;
    if (frames < 64u) frames = 64u;             /* at least 1.5 ms: a jump from silence is a click */
    return (1u << 24) / frames + 1u;
}

static uint32_t rnd_next(uint32_t *s) { *s = *s * 1664525u + 1013904223u; return *s >> 16; }

/* ---------------------------------------------------------------------------------------------- the ground */
static volatile uint8_t ground[GH][GW];         /* 0 bare, 1..3 painted (written by the ISR only) */
static struct { uint8_t x, y, d; } ant[ANTS];   /* d: 0 up, 1 right, 2 down, 3 left */
static volatile int32_t world_t;                /* steps since the colony hatched (negative: before it) */
static volatile uint32_t world_ver;             /* +1 on every change of the ground or the ants (the screen) */
static volatile uint8_t ant_vx[ANTS], ant_vy[ANTS], ant_vd[ANTS];   /* the ants, for the screen */
static volatile uint32_t painted;               /* how many cells are painted */
static const int8_t DX[4] = {0, 1, 0, -1}, DY[4] = {-1, 0, 1, 0};

static void ants_place(void)                    /* where a new colony's ants start: spread along the middle */
{
    for (uint32_t i = 0; i < ANTS; i++) {
        ant[i].x = (uint8_t)((GW * (2u * i + 1u)) / (2u * ANTS));
        ant[i].y = GH / 2u;
        ant[i].d = (uint8_t)(i & 3u);
    }
}

/* where the SEED's pattern lies: an ant sings an octave higher on these cells, painted over or not, for the colony's
 * whole life (so you hear what the seed does) */
static volatile uint8_t seed_at[GH][GW];
static volatile uint8_t hatched_seed;           /* the SEED this colony hatched on */

static void ground_seed(uint8_t seed, uint8_t paint)   /* the pattern of a seed (always the same); paint: onto the
                                                         * ground too (a hatch), or only where it lies (a USER preset) */
{
    hatched_seed = seed;
    uint32_t r = 0x2545F491u;
    for (uint32_t y = 0; y < GH; y++)
        for (uint32_t x = 0; x < GW; x++) {
            uint8_t c = 0;
            switch (seed) {
            case SEED_DOT: c = x == GW / 2u - 3u && y == GH / 2u - 2u; break;
            case SEED_DASH: c = (y == 3u || y == 11u) && x % 8u < 3u ? (uint8_t)(1u + (x / 8u) % 3u) : 0u; break;
            case SEED_CROSS: c = (x == GW / 2u && y > 2u && y < GH - 3u) || (y == GH / 2u && x > 16u && x < GW - 16u); break;
            case SEED_RING: c = ((x == 12u || x == GW - 13u) && y >= 2u && y <= GH - 3u) ||
                                ((y == 2u || y == GH - 3u) && x >= 12u && x <= GW - 13u) ? 2u : 0u; break;
            case SEED_DUST: c = rnd_next(&r) % 100u < 6u; break;
            case SEED_MOSS: { uint32_t v = rnd_next(&r) % 100u; c = v < 30u ? (uint8_t)(1u + v % 3u) : 0u; break; }
            case SEED_STRIPES: c = x % 7u == 0u ? (uint8_t)(1u + (x / 7u) % 3u) : 0u; break;
            default: break;
            }
            seed_at[y][x] = c != 0u;
            if (paint) ground[y][x] = c;
        }
}

static uint32_t count_painted(void)
{
    uint32_t n = 0;
    for (uint32_t y = 0; y < GH; y++) for (uint32_t x = 0; x < GW; x++) n += ground[y][x] != 0u;
    return n;
}

/* the main loop's requests (one writer each side) */
static volatile uint32_t hatch_w, hatch_r;      /* REC: a new colony */
static volatile uint32_t scrub_fw, scrub_bw;    /* SELECT: single steps forward / back (counted by the main loop) */
static uint32_t scrub_fr, scrub_br;             /* ... and done by the ISR */
static volatile int32_t time_dir = 1;           /* OCT+ forward (1), OCT- backward (-1) */
static volatile uint32_t time_fast;             /* both OCT buttons held: two steps for one */
static volatile uint32_t frozen;                /* SEL: the ants walk but no longer paint */
static volatile uint32_t world_load;            /* a saved colony to put down (world_buf), set by the main loop */
static uint8_t world_buf[GH * GW / 4u + 1u];
static uint8_t world_ants[ANTS * 3u];
static int32_t world_buf_t;
static uint8_t world_seed;

/* -------------------------------------------------------------------------------------------- the voices */
enum { ST_IDLE, ST_ATTACK, ST_DECAY, ST_HOLD, ST_RELEASE };
typedef struct {
    int32_t pitch;                              /* 1/256 semitones */
    int32_t env;                                /* 0 .. 1 << 24 */
    int32_t vel;                                /* Q15 */
    int32_t gl, gr;                             /* the pan, Q15 */
    uint32_t cx, cy;                            /* the orbit's centre on the terrain (2^32 = all the way round) */
    int32_t dc;                                 /* the wave's own mean, taken away */
    uint32_t fade;                              /* a stolen voice: frames of its fade-out left before the next note */
    int32_t nx_pitch, nx_vel, nx_gl, nx_gr;     /* ... and that note */
    uint32_t nx_cx, nx_cy;
    uint8_t nx_tag;
    uint32_t ph, wait, age;
    uint8_t stage, tag, who;                    /* tag: the key holding it (0: none); who: the ant + 1 */
} voice_t;
static voice_t vo[NVOICE];
static uint32_t vo_age;
#define STEAL_FADE 64u                          /* 1.5 ms: a stolen voice fades out, then plays anew (no click) */

static void voice_begin(voice_t *x)
{
    x->pitch = x->nx_pitch;
    x->vel = x->nx_vel;
    x->gl = x->nx_gl;
    x->gr = x->nx_gr;
    x->cx = x->nx_cx;
    x->cy = x->nx_cy;
    x->tag = x->nx_tag;
    x->stage = ST_ATTACK;
    x->env = 0;
    x->ph = 0;
    x->fade = 0;
}

static void voice_start(uint32_t v, int32_t pitch, int32_t vel, int32_t pan, uint32_t cx, uint32_t cy,
                        uint32_t wait, uint8_t tag, uint8_t who)
{
    voice_t *x = &vo[v];
    uint32_t a = (uint32_t)(pan + 32768) << 14;  /* an equal-power pan from the sine table */
    x->nx_pitch = pitch;
    x->nx_vel = vel;
    x->nx_gl = sinq(a + 0x40000000u);
    x->nx_gr = sinq(a);
    x->nx_cx = cx;
    x->nx_cy = cy;
    x->nx_tag = tag;
    x->who = who;
    x->age = ++vo_age;
    if (x->stage == ST_IDLE) { x->wait = wait; voice_begin(x); x->dc = 0; }
    else { x->wait = 0; x->fade = STEAL_FADE; x->tag = 0; }
}

static uint32_t voice_cap = NVOICE;             /* fewer when the sound runs short of time (app_audio) */
static uint32_t voice_pick(void)                /* a silent voice, else the quietest of the oldest */
{
    uint32_t best = 0, score_best = 0xFFFFFFFFu;
    for (uint32_t v = 0; v < voice_cap; v++) {
        uint32_t score;
        if (vo[v].stage == ST_IDLE) return v;
        score = (vo[v].stage == ST_RELEASE || vo[v].stage == ST_DECAY ? 0u : 0x40000000u) + (uint32_t)(vo[v].env >> 10) +
                (vo_age - vo[v].age > 1000u ? 0u : 0x10000000u);
        if (vo[v].tag) score += 0x80000000u;    /* a held key is the last to go */
        if (score < score_best) { score_best = score; best = v; }
    }
    return best;
}

/* the keys (from the main loop): a small queue */
typedef struct { uint8_t on, tag; int16_t vel; int32_t pitch; } cmd_t;
static cmd_t cmdq[16];
static volatile uint32_t cmd_w, cmd_r;
static void cmd_put(uint8_t on, uint8_t tag, int32_t pitch, int32_t vel)
{
    if (cmd_w - cmd_r >= 16u) return;
    cmd_t *c = &cmdq[cmd_w % 16u];
    c->on = on; c->tag = tag; c->pitch = pitch; c->vel = (int16_t)vel;
    cmd_w++;
}

/* --------------------------------------------------------------------------------------------------- LFO */
static uint32_t lfo_ph, lfo_rnd[2] = {0x68e31da4u, 0x1b873593u};
static volatile int32_t lfo_now;
static int32_t lfo_value(void)
{
    int32_t x = (int32_t)(lfo_ph >> 16) - 32768, m = x < 0 ? -x : x;
    switch (pat.lfo_wave) {
    case LW_SINE: return clampi((int32_t)((int64_t)x * (32768 - m) >> 13), -32767, 32767);
    case LW_TRIANGLE: return clampi(32767 - 2 * m, -32767, 32767);
    case LW_RANDOM: return (int32_t)(lfo_rnd[1] >> 16) - 32768;
    default: {
        int32_t a = (int32_t)(lfo_rnd[0] >> 16) - 32768, b = (int32_t)(lfo_rnd[1] >> 16) - 32768;
        uint32_t t = lfo_ph >> 17, s = (t * t >> 15) * (3u * 32768u - 2u * t) >> 15;
        return a + (int32_t)(((int64_t)(b - a) * (int32_t)s) >> 15);
    }
    }
}

/* ------------------------------------------------------------------------------------ the map and the song */
static volatile int32_t cell_pitch[GH][GW];     /* each cell's note (the map through the scale, the ARP's chord) */
static volatile uint32_t chance_pct = 100;      /* the ARP's CHANCE (100 when it is off) */

/* what an ant sang, for the main loop (MIDI out, the key lights, the screen) */
typedef struct { uint8_t who, x, y, note, vel; } sung_t;
static sung_t sungq[32];
static volatile uint32_t sung_w, sung_r;

static uint32_t tk_n, tk_tick[8], tk_ofs[8];   /* the clock's ticks in this block (kit_clock.h -> app_tick) */
static void app_tick(uint32_t tick, uint32_t ofs)
{
    if (tk_n < 8u) { tk_tick[tk_n] = tick; tk_ofs[tk_n] = ofs; tk_n++; }
}

static inline uint32_t cell_u32(uint32_t i, uint32_t n) { return i * (0xFFFFFFFFu / n) + 0x7FFFFFFFu / n; }   /* a cell's middle */

static void ant_sing(uint32_t i, uint8_t colour, uint32_t wait, int32_t loud)
{
    uint8_t x = ant[i].x, y = ant[i].y;
    if (pat.sing == SING_PAINT && !colour) return;
    if (pat.sing == SING_BARE && colour) return;
    if (chance_pct < 100u) {                    /* CHANCE: the same for the same step, forwards and backwards */
        uint32_t h = ((uint32_t)world_t * 2654435761u) ^ (i * 0x9E3779B9u) ^ ((uint32_t)x << 8 | y);
        h ^= h >> 15; h *= 0x2C1B3C6Du; h ^= h >> 12;
        if (h % 100u >= chance_pct) return;
    }
    /* ACCENT: bare and the first colour softer, the last the loudest */
    uint32_t c = colour ? colour : 1u;
    int32_t vel = 32767 - (int32_t)pat.accent * (3 - (int32_t)c) * 32767 * 7 / 3000;
    vel = mul15(vel, loud);
    int32_t pan = ((int32_t)x * 2 - (int32_t)(GW - 1u)) * 32767 / (int32_t)(GW - 1u) * 7 / 10;
    /* DRIFT: the orbit's centre, from the middle of the terrain towards where the ant stands */
    uint32_t q8 = (uint32_t)pat.drift * 256u / 100u;
    int32_t ddx = (int32_t)(cell_u32(x, GW) - 0x80000000u), ddy = (int32_t)(cell_u32(y, GH) - 0x80000000u);
    uint32_t cx = 0x80000000u + (uint32_t)(int32_t)(((int64_t)ddx * q8) >> 8);
    uint32_t cy = 0x80000000u + (uint32_t)(int32_t)(((int64_t)ddy * q8) >> 8);
    int32_t pitch = cell_pitch[y][x];
    if (seed_at[y][x]) pitch = pitch + 12 * 256 > 120 * 256 ? pitch : pitch + 12 * 256;   /* the seed: an octave up */
    voice_start(voice_pick(), pitch, vel, pan, cx, cy, wait, 0, (uint8_t)(i + 1u));
    if (sung_w - sung_r < 32u) {
        sung_t *e = &sungq[sung_w % 32u];
        e->who = (uint8_t)i; e->x = x; e->y = y;
        e->note = (uint8_t)clampi((pitch + 128) >> 8, 0, 127);
        e->vel = (uint8_t)clampi(vel >> 8, 1, 127);
        sung_w++;
    }
}

/* one step of the whole colony, forwards (dir 1) or backwards (-1); the ants sing (loud: Q15, 0 = silent) */
static void colony_step(int32_t dir, uint32_t wait, int32_t loud)
{
    const rule_t *r = &rules[pat.rule % RULES];
    uint32_t n = pat.ants < 1u ? 1u : pat.ants > ANTS ? ANTS : pat.ants;
    uint8_t paint = !frozen;
    if (dir > 0) {
        for (uint32_t i = 0; i < n; i++) {
            uint8_t x = ant[i].x, y = ant[i].y, c = (uint8_t)(ground[y][x] % r->n);
            if (loud) ant_sing(i, c, wait, loud);
            ant[i].d = (uint8_t)((ant[i].d + r->turn[c]) & 3u);
            if (paint) {
                uint8_t nc = (uint8_t)((c + 1u) % r->n);
                painted += (nc != 0u) - (c != 0u);
                ground[y][x] = nc;
            }
            ant[i].x = (uint8_t)((x + GW + DX[ant[i].d]) % GW);
            ant[i].y = (uint8_t)((y + GH + DY[ant[i].d]) % GH);
        }
        world_t++;
    } else {
        for (uint32_t k = n; k-- > 0;) {        /* backwards: the last ant first, every move undone */
            uint8_t x = (uint8_t)((ant[k].x + GW - DX[ant[k].d]) % GW), y = (uint8_t)((ant[k].y + GH - DY[ant[k].d]) % GH);
            uint8_t c = (uint8_t)(ground[y][x] % r->n);
            if (paint) {
                uint8_t pc = (uint8_t)((c + r->n - 1u) % r->n);
                painted += (pc != 0u) - (c != 0u);
                ground[y][x] = pc;
                c = pc;
            }
            ant[k].d = (uint8_t)((ant[k].d - r->turn[c]) & 3u);
            ant[k].x = x; ant[k].y = y;
        }
        world_t--;
        if (loud) for (uint32_t k = 0; k < n; k++)   /* ... and each sings what it sang there going forwards */
            ant_sing(k, (uint8_t)(ground[ant[k].y][ant[k].x] % r->n), wait, loud);
    }
    world_ver++;
}

static void colony_publish(void)
{
    for (uint32_t i = 0; i < ANTS; i++) { ant_vx[i] = ant[i].x; ant_vy[i] = ant[i].y; ant_vd[i] = ant[i].d; }
}

static struct { uint8_t running; uint32_t acc; } cl;
static volatile uint32_t cl_running;

static void colony_block(void)
{
    if (hatch_r != hatch_w) {                   /* REC: a new colony from the SEED */
        hatch_r = hatch_w;
        ground_seed(pat.seed, 1);
        ants_place();
        world_t = 0;
        painted = count_painted();
        world_ver++;
    }
    if (world_load) {                           /* a USER preset's colony */
        for (uint32_t i = 0; i < GH * GW; i++) ground[i / GW][i % GW] = (uint8_t)(world_buf[i >> 2] >> ((i & 3u) * 2u) & 3u);
        for (uint32_t i = 0; i < ANTS; i++) {
            ant[i].x = (uint8_t)(world_ants[3u * i] % GW);
            ant[i].y = (uint8_t)(world_ants[3u * i + 1u] % GH);
            ant[i].d = (uint8_t)(world_ants[3u * i + 2u] & 3u);
        }
        world_t = world_buf_t;
        painted = count_painted();
        ground_seed(world_seed, 0);
        world_load = 0;
        world_ver++;
    }
    /* SELECT: a step at a time, by hand (one a block, so each is heard) */
    if (scrub_fr != scrub_fw) { scrub_fr++; colony_step(1, 0, Q15 - 1); }
    else if (scrub_br != scrub_bw) { scrub_br++; colony_step(-1, 0, Q15 - 1); }

    if (!clk_run) { cl.running = 0; cl_running = 0; tk_n = 0; colony_publish(); return; }
    uint32_t every = step_ticks[pat.step % STEPS];
    for (uint32_t t = 0; t < tk_n; t++) {
        uint32_t tick = tk_tick[t];
        if (tick == 0u || !cl.running) cl.running = 1;
        if (time_fast) {                        /* both OCT buttons: twice as fast (two half-ticks a tick: exact
                                                 * for every STEP, 1/32 too) */
            cl.acc += 2u;
            if (cl.acc >= every) { cl.acc -= every; colony_step(time_dir, tk_ofs[t], Q15 - 1); }
        } else {
            cl.acc = 0;                         /* (back on the beat's grid) */
            if (tick % every == 0u) colony_step(time_dir, tk_ofs[t], Q15 - 1);
        }
    }
    tk_n = 0;
    cl_running = cl.running;
    colony_publish();
}

/* ------------------------------------------------------------------------------------------- the terrain */
static int16_t terr[TH][TW];                    /* what the orbits read (the ISR glides it to terr_to) */
static volatile int16_t terr_to[TH][TW];        /* the landscape from the ground, built by the main loop */

static void terrain_glide(void)                 /* once a block: no step in the landscape is ever heard as a click */
{
    int16_t *a = &terr[0][0];
    volatile int16_t *b = &terr_to[0][0];
    for (uint32_t i = 0; i < TH * TW; i++) a[i] = (int16_t)(a[i] + ((b[i] - a[i]) >> 2));
}

static inline int32_t terrain_at(uint32_t X, uint32_t Y)   /* the height under a point, bilinear, Q14 */
{
    uint32_t xi = X >> 26, yi = Y >> 28, x1 = (xi + 1u) & (TW - 1u), y1 = (yi + 1u) & (TH - 1u);
    int32_t fx = (int32_t)((X >> 11) & 0x7FFFu), fy = (int32_t)((Y >> 13) & 0x7FFFu);
    int32_t a = terr[yi][xi], b = terr[yi][x1], c = terr[y1][xi], d = terr[y1][x1];
    int32_t top = a + (((b - a) * fx) >> 15), bot = c + (((d - c) * fx) >> 15);
    return top + (((bot - top) * fy) >> 15);
}

/* where an orbit is at phase ph: an offset from its centre, Q15 each way */
static inline void orbit_at(uint8_t orbit, uint32_t ph, int32_t *ox, int32_t *oy)
{
    switch (orbit) {
    case ORB_EIGHT: *ox = sinq(ph); *oy = sinq(ph << 1) >> 1; break;
    case ORB_ROSE: {                            /* three petals */
        int32_t r = sinq(ph * 3u + 0x40000000u);
        *ox = mul15(r, sinq(ph + 0x40000000u)); *oy = mul15(r, sinq(ph));
        break;
    }
    case ORB_DIAMOND: {                         /* two triangles a quarter apart */
        int32_t u = (int32_t)(ph >> 16) - 32768, v = (int32_t)((ph + 0x40000000u) >> 16) - 32768;
        *ox = 32767 - 2 * (u < 0 ? -u : u); *oy = 32767 - 2 * (v < 0 ? -v : v);
        break;
    }
    case ORB_LINE: *ox = 0; *oy = sinq(ph); break;   /* (and all the way across: see voice_render) */
    default: *ox = sinq(ph + 0x40000000u); *oy = sinq(ph); break;
    }
}

/* ------------------------------------------------------------------------------------------------ one voice */
static int32_t vbuf_l[HALF_FRAMES], vbuf_r[HALF_FRAMES];

static void voice_render(voice_t *x, uint32_t frames, int32_t pitch_mod, int32_t size_mod, uint32_t shift,
                         int32_t level0, int32_t level)
{
    uint32_t up = env_step(pat.attack), down = env_step(pat.decay);
    uint32_t inc = inc_of(x->pitch + pitch_mod);
    uint8_t orbit = pat.orbit % ORBITS;
    /* the orbit's size: SIZE, smaller for high notes (their waves alias sooner), opened by BLOOM at each footstep */
    int32_t base = clampi((int32_t)pat.size * 327 + size_mod, 0, 32767);
    int32_t over = x->pitch - 60 * 256, kt = over > 0 ? 32767 * (36 * 256) / (36 * 256 + over) : 32767;
    int32_t bloom = (int32_t)pat.bloom * 327;
    uint32_t cx = x->cx + shift, cy = x->cy;
    for (uint32_t f0 = 0; f0 < frames; f0 += SUB) {
        uint32_t n = frames - f0 < SUB ? frames - f0 : SUB;
        uint32_t start = 0;
        if (x->wait) {
            if (x->wait >= n) { x->wait -= n; continue; }
            start = x->wait;
            x->wait = 0;
        }
        int32_t e15 = x->env >> 9;
        int32_t sz = mul15(base, 32767 - bloom + mul15(bloom, e15));
        int32_t R = 1300 + mul15(sz, 64200);    /* the radius: 2 % .. 50 % of the terrain (2^17 = all of it) */
        R = mul15(R, kt);
        int32_t Ry = R * 15 / 4 > 65535 ? 65535 : R * 15 / 4;   /* as round in cells as across (the ground is 56 x 15) */
        int32_t lv = level0 + (level - level0) * (int32_t)f0 / (int32_t)frames;
        int32_t vl = ((x->vel * lv) >> 15) >> 1;
        for (uint32_t i = start; i < n; i++) {
            uint32_t fade = 0;
            if (x->fade) {
                fade = --x->fade;
                if (!fade || x->stage == ST_IDLE) {
                    voice_begin(x);
                    inc = inc_of(x->pitch + pitch_mod);
                    over = x->pitch - 60 * 256;
                    kt = over > 0 ? 32767 * (36 * 256) / (36 * 256 + over) : 32767;
                    cx = x->cx + shift; cy = x->cy;
                    vl = ((x->vel * lv) >> 15) >> 1;
                    fade = 0;
                }
            }
            switch (x->stage) {                 /* attack, then a decay to silence; a held key holds, then lets go */
            case ST_ATTACK:
                x->env += (int32_t)up;
                if (x->env >= 1 << 24) { x->env = 1 << 24; x->stage = x->tag ? ST_HOLD : ST_DECAY; }
                break;
            case ST_DECAY: case ST_RELEASE:
                x->env -= (int32_t)down;
                if (x->env <= 0) { x->env = 0; x->stage = ST_IDLE; }
                break;
            default: break;
            }
            uint32_t ph = x->ph;
            x->ph = ph + inc;
            int32_t ox, oy;
            orbit_at(orbit, ph, &ox, &oy);
            uint32_t X = cx + (uint32_t)(ox * R), Y = cy + (uint32_t)(oy * Ry);
            if (orbit == ORB_LINE) X = cx + ph;
            int32_t s = terrain_at(X, Y) << 1;  /* Q15 */
            x->dc += (s - x->dc) >> 10;         /* the wave's own mean, away */
            s -= x->dc;
            int32_t a = ((x->env >> 9) * vl) >> 14;
            if (x->fade) a = (a * (int32_t)fade) >> 6;
            s = (s * a) >> 15;
            vbuf_l[f0 + i] += (s * x->gl) >> 15;
            vbuf_r[f0 + i] += (s * x->gr) >> 15;
        }
        if (x->stage == ST_IDLE && !x->fade) break;
    }
}

/* ---------------------------------------------------------------------------------------- the whole sound */
static void anthem_render(int32_t *out, uint32_t frames)
{
    while (cmd_r != cmd_w) {                    /* the keys */
        cmd_t c = cmdq[cmd_r % 16u];
        cmd_r++;
        if (c.on) {
            uint32_t q8 = (uint32_t)pat.drift * 256u / 100u;   /* a key sounds the ground under the first ant */
            int32_t ddx = (int32_t)(cell_u32(ant[0].x, GW) - 0x80000000u), ddy = (int32_t)(cell_u32(ant[0].y, GH) - 0x80000000u);
            voice_start(voice_pick(), c.pitch, c.vel, 0, 0x80000000u + (uint32_t)(int32_t)(((int64_t)ddx * q8) >> 8),
                        0x80000000u + (uint32_t)(int32_t)(((int64_t)ddy * q8) >> 8), 0, c.tag, 0);
        } else for (uint32_t v = 0; v < NVOICE; v++) {
            if (vo[v].fade && vo[v].nx_tag == c.tag) vo[v].nx_tag = 0;
            if (vo[v].tag == c.tag && vo[v].stage != ST_IDLE) { vo[v].tag = 0; vo[v].stage = ST_RELEASE; }
        }
    }
    uint32_t before = lfo_ph;                   /* the LFO, once a block */
    lfo_ph += lfo_inc16[pat.lfo_rate % LFO_RATES] * (frames / SUB);
    if (lfo_ph < before) { lfo_rnd[0] = lfo_rnd[1]; lfo_rnd[1] = lfo_rnd[1] * 1664525u + 1013904223u; }
    int32_t l = lfo_value(), mod = l * pat.lfo_depth / 100;
    lfo_now = l;
    int32_t pitch_mod = pat.lfo_target == LFO_PITCH ? mod >> 7 : 0;        /* +- 1 semitone */
    int32_t size_mod = pat.lfo_target == LFO_SIZE ? mod / 2 : 0;           /* +- 50 points of SIZE */
    uint32_t shift = pat.lfo_target == LFO_SHIFT ? (uint32_t)(mod * 16384) : 0u;   /* the orbits slide: +- 1/8 */
    static int32_t level0 = Q15;
    int32_t level = pat.lfo_target == LFO_LEVEL ? Q15 - (32767 - l) * pat.lfo_depth / 200 : Q15;

    terrain_glide();
    colony_block();

    for (uint32_t i = 0; i < frames; i++) vbuf_l[i] = vbuf_r[i] = 0;
    for (uint32_t v = 0; v < NVOICE; v++)
        if (vo[v].stage != ST_IDLE) voice_render(&vo[v], frames, pitch_mod, size_mod, shift, level0, level);
    level0 = level;
    /* The voices' limiter. Long decays pile notes up (eight voices of 10 s each are four times full scale): the
     * gain comes down smoothly, before the sum reaches LIMIT, and comes back slowly (~0.4 s) once it is quieter. The
     * output's soft ceiling is then only for the echo and the reverb on top, never for the voices themselves. */
#define LIMIT 12500
    static int32_t gain = Q15;                  /* Q15 */
    int32_t peak = 0;
    for (uint32_t i = 0; i < frames; i++) {
        int32_t a = vbuf_l[i] < 0 ? -vbuf_l[i] : vbuf_l[i], b = vbuf_r[i] < 0 ? -vbuf_r[i] : vbuf_r[i];
        if (a > peak) peak = a;
        if (b > peak) peak = b;
    }
    peak /= 2;                                  /* (the voices go out at half: eight at once) */
    int32_t target = peak > LIMIT ? (LIMIT << 15) / peak : Q15;   /* (32-bit: no 64-bit division on the chip) */
    int32_t g0 = gain, g1 = target < gain ? target : gain + ((target - gain) >> 6);
    uint32_t ramp = target < gain ? 32u : frames;   /* down within 0.7 ms (no click), up over the block */
    for (uint32_t i = 0; i < frames; i++) {
        int32_t g = i < ramp ? g0 + (g1 - g0) * (int32_t)i / (int32_t)ramp : g1;
        out[2u * i] += (int32_t)(((int64_t)(vbuf_l[i] / 2) * g) >> 15);
        out[2u * i + 1u] += (int32_t)(((int64_t)(vbuf_r[i] / 2) * g) >> 15);
    }
    gain = g1;
}

static int anthem_quiet(void)
{
    for (uint32_t v = 0; v < NVOICE; v++) if (vo[v].stage != ST_IDLE) return 0;
    return 1;
}
