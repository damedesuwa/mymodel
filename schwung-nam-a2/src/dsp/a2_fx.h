/*
 * a2_fx.h - the pedals.
 *
 * One struct and one process function each, all mono, all in place, all
 * cheap. The expensive thing in this module is the amp model (~1100 us of a
 * 2370 us frame); these have to fit in what is left, seven at a time, so
 * none of them may cost more than a few tens of microseconds.
 *
 * WHY HAND-WRITTEN AND NOT CAPTURED. A .nam capture of a pedal is more
 * faithful and costs ~1100 us - the same as the amp. Two of those do not fit
 * in a frame. These cost 5-40 us, so a board can have seven of them AND the
 * amp. That is the whole trade, and it is why the character here comes from
 * getting the TOPOLOGY right rather than from matching a circuit: where the
 * clipping sits relative to the gain stage, what is filtered before it, and
 * what is filtered after.
 *
 * EVERY RECURSIVE STATE IS FLUSHED. A filter decaying toward zero passes
 * through the subnormal range, where the FPU traps to microcode - measured
 * on this device at 50x on a block that should cost 18 us, and it bites
 * when nothing is playing, so it reads as a random dropout rather than as
 * load. flush_denormal() is two compares.
 */

#ifndef A2_FX_H
#define A2_FX_H

#include "a2_common.h"

/* Flat ids. The UI groups them; the DSP does not care. Appended to, never
 * reordered - a saved board stores the number. */
enum {
    FX_OVERDRIVE = 0, FX_DISTORTION, FX_FUZZ, FX_BOOST,
    FX_COMPRESSOR, FX_GATE,
    FX_EQ, FX_AUTOWAH,
    FX_CHORUS, FX_PHASER, FX_TREMOLO,
    FX_DELAY, FX_SLAPBACK, FX_REVERB,
    FX_DOUBLER, FX_DETUNE,
    FX_COUNT
};

#define FX_MAX_DELAY 22050      /* 500 ms at 44100 */
#define FX_PARAMS 5

/* ------------------------------------------------------------------ util */

static inline float fx_onepole(float *state, float x, float a) {
    *state = flush_denormal(*state + a * (x - *state));
    return *state;
}

/* A one-pole coefficient for a cutoff in Hz. */
static inline float fx_coeff(float hz) {
    float a = 1.0f - expf(-2.0f * (float)M_PI * hz / SAMPLE_RATE);
    return (a < 0.0f) ? 0.0f : (a > 1.0f ? 1.0f : a);
}

/* ------------------------------------------------------------------ state */

typedef struct {
    int   id;                   /* which pedal */
    float p[FX_PARAMS];         /* its knobs, 0..1 */

    /* drive */
    float hp_x1, hp_y1, lp_y1;
    /* compressor / gate */
    float env, gate_gain;
    /* eq / wah */
    float lo_s, mid_s, hi_s, wah_lp1, wah_lp2, wah_bp;
    /* modulation */
    float lfo_phase, lfo2_phase;
    float ap[6];                /* phaser allpass states */
    /* delay line, shared by delay / slapback / doubler / detune / chorus */
    float *line;
    int    line_len;
    int    w;
    float  rd;                  /* fractional read head, for the pitch pair */
    /* reverb: four combs and two allpasses, Schroeder's shape */
    float *rv;                  /* one block for every reverb buffer */
    int    rv_len;
    int    comb_i[4], ap_i[2];
    float  comb_z[4];
} fx_block_t;

/* Buffer sizes are fixed, so a block never allocates on the audio thread -
 * the worker does it once and the id can then change for free. */
#define RV_COMB0 1557
#define RV_COMB1 1617
#define RV_COMB2 1491
#define RV_COMB3 1422
#define RV_AP0    225
#define RV_AP1     77
#define RV_TOTAL (RV_COMB0 + RV_COMB1 + RV_COMB2 + RV_COMB3 + RV_AP0 + RV_AP1)

static int fx_block_alloc(fx_block_t *f) {
    if (!f->line) {
        f->line = (float *)calloc(FX_MAX_DELAY, sizeof(float));
        if (!f->line) return 0;
        f->line_len = FX_MAX_DELAY;
    }
    if (!f->rv) {
        f->rv = (float *)calloc(RV_TOTAL, sizeof(float));
        if (!f->rv) return 0;
        f->rv_len = RV_TOTAL;
    }
    return 1;
}

static void fx_block_free(fx_block_t *f) {
    free(f->line); f->line = NULL; f->line_len = 0;
    free(f->rv);   f->rv = NULL;   f->rv_len = 0;
}

/* Changing pedal must not leave the previous one's tail ringing through the
 * new one's filters. */
static void fx_block_reset(fx_block_t *f) {
    f->hp_x1 = f->hp_y1 = f->lp_y1 = 0.0f;
    f->env = 0.0f; f->gate_gain = 1.0f;
    f->lo_s = f->mid_s = f->hi_s = 0.0f;
    f->wah_lp1 = f->wah_lp2 = f->wah_bp = 0.0f;
    f->lfo_phase = f->lfo2_phase = 0.0f;
    for (int i = 0; i < 6; i++) f->ap[i] = 0.0f;
    f->w = 0; f->rd = 0.0f;
    for (int i = 0; i < 4; i++) { f->comb_i[i] = 0; f->comb_z[i] = 0.0f; }
    f->ap_i[0] = f->ap_i[1] = 0;
    if (f->line) memset(f->line, 0, (size_t)f->line_len * sizeof(float));
    if (f->rv)   memset(f->rv, 0, (size_t)f->rv_len * sizeof(float));
}

/* --------------------------------------------------------------- reading */

static inline float fx_tap(fx_block_t *f, float delay_samples) {
    float rp = (float)f->w - delay_samples;
    while (rp < 0.0f) rp += (float)f->line_len;
    int i0 = (int)rp;
    float fr = rp - i0;
    int i1 = (i0 + 1 >= f->line_len) ? 0 : i0 + 1;
    return f->line[i0] + (f->line[i1] - f->line[i0]) * fr;
}

/* ---------------------------------------------------------------- pedals */

static void fx_drive(fx_block_t *f, float *a, int n) {
    const float amt = f->p[0], tone = f->p[1], lvl = f->p[2];
    const int id = f->id;

    /* Boost is a clean lift with a bass trim: its job is to hit the amp
     * harder, not to make its own distortion. */
    const float gain = (id == FX_BOOST) ? (1.0f + amt * 8.0f)
                                        : (1.0f + amt * amt * 60.0f);
    /* A bass cut BEFORE the clip is the Tube Screamer move - the amp stays
     * tight because the mud never arrives. A fuzz keeps its bottom end, and
     * that is most of why it is a fuzz. */
    const float hp_a = (id == FX_FUZZ) ? 0.0f
                     : (id == FX_BOOST) ? 0.9985f : 0.9931f;
    const float lp_a = 0.35f + 0.60f * (1.0f - tone);

    for (int i = 0; i < n; i++) {
        float x = a[i];
        if (hp_a > 0.0f) {
            float y = x - f->hp_x1 + hp_a * f->hp_y1;
            f->hp_x1 = flush_denormal(x);
            f->hp_y1 = flush_denormal(y);
            x = y;
        }
        x *= gain;
        switch (id) {
            case FX_DISTORTION: x = tanhf(x * 1.6f); break;
            case FX_FUZZ:
                x = (x > 0.0f ? 1.0f : -1.0f) * (1.0f - expf(-fabsf(x) * 3.0f));
                break;
            case FX_BOOST:      x = tanhf(x * 0.35f) * 2.2f; break;
            default:            x = tanhf(x * ((x < 0.0f) ? 1.35f : 1.0f)); break;
        }
        f->lp_y1 = flush_denormal(f->lp_y1 + lp_a * (x - f->lp_y1));
        x = f->lp_y1 + (x - f->lp_y1) * tone;
        a[i] = sanitize_sample(x * lvl * ((id == FX_BOOST) ? 0.8f : 0.35f));
    }
}

static void fx_compressor(fx_block_t *f, float *a, int n) {
    /* Threshold -40..0 dB, ratio 1..20, makeup 0..+12 dB. A guitar
     * compressor is mostly about the attack being audible, so the release
     * is long and the attack is short.
     *
     * The makeup was 0..+20 dB and pinned the output at full scale at two
     * thirds of a turn - compression RAISES the rms, so the same makeup
     * that suits a clean signal clips a compressed one. +12 dB is the
     * useful part of that range, and the output is soft-limited so no
     * setting of three knobs can produce a square wave by accident. */
    const float thr = powf(10.0f, (-40.0f + 40.0f * f->p[0]) / 20.0f);
    const float ratio = 1.0f + f->p[1] * 19.0f;
    const float makeup = powf(10.0f, (f->p[2] * 12.0f) / 20.0f);
    const float atk = fx_coeff(200.0f);
    const float rel = fx_coeff(4.0f);

    for (int i = 0; i < n; i++) {
        float x = a[i];
        float mag = fabsf(x);
        f->env = flush_denormal(f->env + (mag > f->env ? atk : rel) * (mag - f->env));
        float g = 1.0f;
        if (f->env > thr && f->env > 1e-9f) {
            float over = f->env / thr;
            g = powf(over, (1.0f / ratio) - 1.0f);
        }
        a[i] = sanitize_sample(tanhf(x * g * makeup * 1.2f) * 0.85f);
    }
}

static void fx_gate(fx_block_t *f, float *a, int n) {
    /* Hysteresis, or the gate chatters on the decay of every note. */
    const float open_at  = powf(10.0f, (-70.0f + 60.0f * f->p[0]) / 20.0f);
    const float close_at = open_at * 0.5f;           /* 6 dB below */
    const float rel = fx_coeff(2.0f + f->p[1] * 60.0f);
    const float atk = fx_coeff(800.0f);

    for (int i = 0; i < n; i++) {
        float mag = fabsf(a[i]);
        f->env = flush_denormal(f->env + (mag > f->env ? atk : rel) * (mag - f->env));
        float want = (f->env > open_at) ? 1.0f : (f->env < close_at ? 0.0f : f->gate_gain);
        f->gate_gain = flush_denormal(f->gate_gain +
                       ((want > f->gate_gain) ? atk : rel) * (want - f->gate_gain));
        a[i] = sanitize_sample(a[i] * f->gate_gain);
    }
}

static void fx_eq(fx_block_t *f, float *a, int n) {
    /* Three one-poles split the band; the knobs are gains on the parts.
     * Not a parametric - a guitar EQ is bass/mid/treble and that is what
     * three knobs should be. */
    const float lo_g  = f->p[0] * 2.0f;
    const float mid_g = f->p[1] * 2.0f;
    const float hi_g  = f->p[2] * 2.0f;
    const float a_lo = fx_coeff(220.0f), a_hi = fx_coeff(2600.0f);

    for (int i = 0; i < n; i++) {
        float x = a[i];
        float lo = fx_onepole(&f->lo_s, x, a_lo);
        float below_hi = fx_onepole(&f->hi_s, x, a_hi);
        float mid = below_hi - lo;
        float hi = x - below_hi;
        a[i] = sanitize_sample(lo * lo_g + mid * mid_g + hi * hi_g);
    }
}

static void fx_autowah(fx_block_t *f, float *a, int n) {
    /* An envelope-swept state-variable bandpass. The sweep follows what you
     * play, which is the whole point of an auto wah over an LFO filter. */
    const float sens = 0.5f + f->p[0] * 8.0f;
    const float range = f->p[1];
    const float mix = f->p[2];
    const float env_a = fx_coeff(30.0f);

    for (int i = 0; i < n; i++) {
        float x = a[i];
        float mag = fabsf(x);
        f->env = flush_denormal(f->env + env_a * (mag - f->env));
        float e = f->env * sens;
        if (e > 1.0f) e = 1.0f;
        float hz = 300.0f + e * (300.0f + 2200.0f * range);
        float fc = 2.0f * sinf((float)M_PI * hz / SAMPLE_RATE);
        if (fc > 0.7f) fc = 0.7f;
        const float q = 0.25f;
        float hpf = x - f->wah_lp2 - q * f->wah_bp;
        f->wah_bp = flush_denormal(f->wah_bp + fc * hpf);
        f->wah_lp2 = flush_denormal(f->wah_lp2 + fc * f->wah_bp);
        a[i] = sanitize_sample(x * (1.0f - mix) + f->wah_bp * 2.0f * mix);
    }
}

static void fx_mod(fx_block_t *f, float *a, int n) {
    const int id = f->id;
    const float rate = 0.05f + f->p[0] * f->p[0] * 8.0f;   /* Hz */
    const float depth = f->p[1];
    const float mix = f->p[2];
    const float inc = rate / SAMPLE_RATE;

    for (int i = 0; i < n; i++) {
        f->lfo_phase += inc;
        if (f->lfo_phase >= 1.0f) f->lfo_phase -= 1.0f;
        float lfo = sinf(2.0f * (float)M_PI * f->lfo_phase);
        float x = a[i], y;

        if (id == FX_TREMOLO) {
            y = x * (1.0f - depth * 0.5f * (1.0f - lfo));
            a[i] = sanitize_sample(y);
            continue;
        }

        if (id == FX_PHASER) {
            /* Six allpass stages swept together. The notches move; the
             * sound is the notches, so the count matters more than the
             * exactness of any one stage. */
            float d = 0.35f + 0.45f * (0.5f + 0.5f * lfo) * depth;
            float v = x;
            for (int k = 0; k < 6; k++) {
                float out = -d * v + f->ap[k];
                f->ap[k] = flush_denormal(v + d * out);
                v = out;
            }
            a[i] = sanitize_sample(x * (1.0f - mix * 0.5f) + v * mix);
            continue;
        }

        /* Chorus: one modulated tap around 12 ms. */
        f->line[f->w] = x;
        float dly = (0.006f + 0.006f * (0.5f + 0.5f * lfo) * depth) * SAMPLE_RATE;
        y = fx_tap(f, dly);
        f->w = (f->w + 1 >= f->line_len) ? 0 : f->w + 1;
        a[i] = sanitize_sample(x * (1.0f - mix * 0.5f) + y * mix);
    }
}

static void fx_delay(fx_block_t *f, float *a, int n) {
    const int slap = (f->id == FX_SLAPBACK);
    /* A slapback is one short repeat; a delay is a line you can hang on. */
    const float ms = slap ? (40.0f + f->p[0] * 100.0f)
                          : (20.0f + f->p[0] * 460.0f);
    const float fb = slap ? (f->p[1] * 0.25f) : (f->p[1] * 0.85f);
    const float mix = f->p[2];
    const float dly = ms * 0.001f * SAMPLE_RATE;
    /* Repeats darken, which is what keeps a long feedback from turning into
     * a pile of treble. */
    const float damp = fx_coeff(2500.0f);

    for (int i = 0; i < n; i++) {
        float x = a[i];
        float y = fx_tap(f, dly);
        float into = x + fx_onepole(&f->lp_y1, y, damp) * fb;
        f->line[f->w] = flush_denormal(into);
        f->w = (f->w + 1 >= f->line_len) ? 0 : f->w + 1;
        a[i] = sanitize_sample(x * (1.0f - mix * 0.5f) + y * mix);
    }
}

static void fx_reverb(fx_block_t *f, float *a, int n) {
    /* Schroeder: four parallel damped combs into two allpasses. Small, and
     * it is a room rather than a hall - which is what a guitar wants in
     * front of it rather than at the end of a mix. */
    const int len[4] = { RV_COMB0, RV_COMB1, RV_COMB2, RV_COMB3 };
    float *comb[4];
    comb[0] = f->rv;
    comb[1] = comb[0] + RV_COMB0;
    comb[2] = comb[1] + RV_COMB1;
    comb[3] = comb[2] + RV_COMB2;
    float *ap0 = comb[3] + RV_COMB3;
    float *ap1 = ap0 + RV_AP0;

    const float room = 0.70f + f->p[0] * 0.28f;
    const float damp = 0.2f + (1.0f - f->p[1]) * 0.6f;
    const float mix = f->p[2];

    for (int i = 0; i < n; i++) {
        float x = a[i] * 0.25f;
        float acc = 0.0f;
        for (int c = 0; c < 4; c++) {
            float y = comb[c][f->comb_i[c]];
            f->comb_z[c] = flush_denormal(y * (1.0f - damp) + f->comb_z[c] * damp);
            comb[c][f->comb_i[c]] = flush_denormal(x + f->comb_z[c] * room);
            if (++f->comb_i[c] >= len[c]) f->comb_i[c] = 0;
            acc += y;
        }
        for (int s = 0; s < 2; s++) {
            float *buf = s ? ap1 : ap0;
            int L = s ? RV_AP1 : RV_AP0;
            float y = buf[f->ap_i[s]];
            buf[f->ap_i[s]] = flush_denormal(acc + y * 0.5f);
            acc = y - acc;
            if (++f->ap_i[s] >= L) f->ap_i[s] = 0;
        }
        a[i] = sanitize_sample(a[i] * (1.0f - mix * 0.5f) + acc * mix);
    }
}

static void fx_pitch(fx_block_t *f, float *a, int n) {
    /* Two delay taps reading at a rate slightly off 1.0, crossfaded so the
     * wrap is hidden. That is a doubler; the same machine at a wider offset
     * is a detune. No octaves - a delay-line shifter at 2x is artefact, and
     * offering it would be offering something that does not sound like the
     * thing it is named after. */
    const int detune = (f->id == FX_DETUNE);
    const float cents = detune ? (4.0f + f->p[0] * 26.0f) : (4.0f + f->p[0] * 12.0f);
    const float mix = f->p[2];
    const float ratio = powf(2.0f, cents / 1200.0f);
    const float win = (detune ? 0.050f : 0.030f) * SAMPLE_RATE;
    /* A doubler is also LATE, or it is just a chorus. */
    const float base = (detune ? 0.012f : 0.022f) * SAMPLE_RATE;

    for (int i = 0; i < n; i++) {
        float x = a[i];
        f->line[f->w] = x;

        f->rd += (ratio - 1.0f);
        while (f->rd >= win) f->rd -= win;
        while (f->rd < 0.0f)  f->rd += win;

        float d0 = base + f->rd;
        float d1 = base + f->rd + win * 0.5f;
        if (d1 >= base + win) d1 -= win;

        /* Equal-power crossfade on where each tap sits in its window. */
        float t = f->rd / win;
        float g0 = sinf((float)M_PI * (1.0f - t));
        float g1 = sinf((float)M_PI * t);

        float y = fx_tap(f, d0) * g0 + fx_tap(f, d1) * g1;
        f->w = (f->w + 1 >= f->line_len) ? 0 : f->w + 1;
        a[i] = sanitize_sample(x * (1.0f - mix * 0.5f) + y * mix);
    }
}

/* ------------------------------------------------------------- dispatch */

static void fx_block_process(fx_block_t *f, float *a, int n) {
    switch (f->id) {
        case FX_OVERDRIVE: case FX_DISTORTION:
        case FX_FUZZ:      case FX_BOOST:      fx_drive(f, a, n); break;
        case FX_COMPRESSOR:                    fx_compressor(f, a, n); break;
        case FX_GATE:                          fx_gate(f, a, n); break;
        case FX_EQ:                            fx_eq(f, a, n); break;
        case FX_AUTOWAH:                       fx_autowah(f, a, n); break;
        case FX_CHORUS: case FX_PHASER:
        case FX_TREMOLO:                       fx_mod(f, a, n); break;
        case FX_DELAY:  case FX_SLAPBACK:      fx_delay(f, a, n); break;
        case FX_REVERB:                        fx_reverb(f, a, n); break;
        case FX_DOUBLER: case FX_DETUNE:       fx_pitch(f, a, n); break;
        default: break;
    }
}

#endif /* A2_FX_H */
