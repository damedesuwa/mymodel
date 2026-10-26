/* A TUNER, and the two things that make one hard on a guitar.
 *
 * 1. OCTAVE ERRORS. A plucked string is not a sine - the second harmonic of
 *    a low E can be stronger than its fundamental - so a detector that takes
 *    the biggest correlation peak reports E3 for an E2 about a third of the
 *    time. That is worse than no tuner: it is confidently wrong, and the
 *    string ends up an octave out with the needle centred. The fix is
 *    McLeod's rule, below: take the FIRST peak that clears a fraction of the
 *    maximum, never the maximum.
 *
 * 2. RESOLUTION AT THE TOP. Autocorrelation resolves a period to a sample,
 *    and a sample is 2.6 cents at 82 Hz but 21 cents at 660 - useless, since
 *    a tuner has to be good to a couple of cents everywhere. So it runs in
 *    two stages: a cheap search on a decimated signal to find roughly where
 *    the period is, then a refinement at the FULL rate over a handful of
 *    lags around it, with parabolic interpolation on the peak.
 *
 * What it costs: the coarse search is 512 samples over 134 lags (~58k
 * multiply-adds) and the fine one ~31k, run once every 16 blocks - a tuner
 * that updates 21 times a second looks instant and a hardware one does not
 * do better. Amortised that is single-digit microseconds a frame against a
 * 2370 us budget.
 *
 * IT ARMS ITSELF ON BEING READ. There is no set_param to turn it on and
 * therefore nothing to leave switched on: get_param zeroes an idle counter,
 * process_block increments it, and the detector runs only while the counter
 * is low. The thing that wants the answer is the thing that keeps it
 * running, so a UI that crashes with the screen open costs nothing a second
 * later. */
#ifndef A2_TUNER_H
#define A2_TUNER_H

#include "a2_common.h"

/* IT IS CHROMATIC, so the range is the only thing that decides what it can
 * tune. Nothing here knows about strings or about a tuning: the pitch goes
 * through tuner_midi() and comes out as the nearest of all twelve, which is
 * what makes drop D, drop C, open G, DADGAD and a 7- or 8-string all the
 * same case rather than each needing a table.
 *
 *     8-string low F#   F#1   46.25 Hz
 *     drop A (6-str)    A1    55.00 Hz
 *     7-string low B    B1    61.74 Hz
 *     drop C            C2    65.41 Hz
 *     drop D            D2    73.42 Hz
 *     6-string low E    E2    82.41 Hz
 *     4-string bass E   E1    41.20 Hz
 *     5-string bass B   B0    30.87 Hz
 *     6-string bass F#  F#0   23.12 Hz
 *
 * The old floor was 37.8 Hz (512 / lag 146), which covered every guitar
 * tuning in that list - and the comment beside it said it was "below a
 * 5-string bass's low B", which is false: B0 is 30.87 and 37.8 is ABOVE
 * it. The number was right for guitars and the sentence explaining it was
 * wrong, which is the worse of the two problems, because a wrong comment
 * about a measurement is what stops anyone checking it.
 *
 * Both windows are sized from the lowest note needing ~4 periods rather
 * than from a round number: 23.12 Hz has a 43.2 ms period, so 186 ms gives
 * it 4.3. No guitar needs that - it is the basses that did - but the extra
 * lags are absorbed by slicing (see TUN_SLICES) and it stops the floor
 * being a question anyone has to ask again. */
#define TUN_DECIM     8
#define TUN_WIN       1024     /* 186 ms at 5512.5 Hz - 4.3 periods of F#0 */
#define TUN_LAG_MIN   13       /* 424 Hz */
#define TUN_LAG_MAX   240      /* 23.0 Hz - just under a 6-string bass's F#0 */
/* 186 ms at the full rate, sized the same way as the coarse window and for
 * the same reason. 2048 was measurably too short even for a guitar: the
 * fine correlation for an E2 got 2.8 periods, and a parabolic vertex on a
 * peak that broad moved the answer several cents between runs of the SAME
 * tone (measured: -3.0 then +4.8, on a 5-cent gate). At 4096 an E2 had 6.6
 * periods but an 8-string's F#1 would have had 1.1, which is not a
 * measurement. 8192 gives F#1 3.3 and E2 14. It costs 32 KB and nothing
 * per frame, because the fine phase is sliced. */
#define TUN_FINE_WIN  8192
#define TUN_FINE_SPAN 10       /* +-10 full-rate samples around the coarse lag */
/* Idle blocks between cycles. A CYCLE is this plus one snapshot frame plus
 * TUN_SLICES and TUN_FINE_STEPS, so 2 gives 26 frames = 75 ms = ~13
 * updates a second. That is what a hardware tuner feels like, and it costs
 * nothing extra: every frame still does at most one slice. */
#define TUN_EVERY     2
/* THE COARSE SEARCH IS SLICED ACROSS FRAMES, and that is not premature.
 *
 * Measured on the build host, the whole analysis on one frame is 135 us
 * against 1 us for a frame that only feeds the ring - and the device's core
 * is several times slower than that. One frame in sixteen costing half a
 * millisecond is exactly the shape that tips a loaded board into an
 * overrun, and an overrun while you are TUNING is a click in the audio you
 * are listening to. Eight slices of seventeen lags flattens it: every
 * frame does the same small amount of work and nothing spikes.
 *
 * The window is SNAPSHOT when a cycle starts, so the slices all correlate
 * the same 93 ms rather than a ring that moved underneath them. The fine
 * stage reads the live full-rate ring one frame later, which is sound
 * because its lag comes from the coarse answer and a note being tuned is
 * being held. */
/* SIXTEEN, not eight, and that is what keeps the wider range free. The
 * coarse search went from 134 lags over ~430 samples to 228 over ~900 -
 * 3.5x the work - so twice the slices leaves each frame doing what it did
 * before. Flat is the property being defended, not the total. */
#define TUN_SLICES    16
#define TUN_LAGS      (TUN_LAG_MAX - TUN_LAG_MIN + 1)
#define TUN_PER_SLICE ((TUN_LAGS + TUN_SLICES - 1) / TUN_SLICES)
/* THE FINE STAGE IS SLICED TOO, because slicing only the coarse half left
 * it as the spike. Measured over 6400 frames: the coarse slices flattened
 * to a p50 of 5 us, and the p99 stood at 57 - the fine frame, once a
 * cycle, which on the device is a few hundred microseconds landing 18
 * times a second on a board that is already loaded. It is 21 lags over a
 * window of ~1500 samples, five coarse slices' worth of work in one
 * frame. Three lags a frame is one slice's worth. */
#define TUN_FINE_LAGS  (2 * TUN_FINE_SPAN + 1)
#define TUN_FINE_PER   3
#define TUN_FINE_STEPS ((TUN_FINE_LAGS + TUN_FINE_PER - 1) / TUN_FINE_PER)

/* Below this the input is not a note being played. Checked on the PEAK of
 * the window rather than its RMS: a decaying string spends most of a 93 ms
 * window quiet and would gate itself off just as you were listening to it. */
#define TUN_LEVEL_GATE 0.004f  /* about -48 dBFS */
/* How good the correlation has to be to be called a pitch. A plucked note
 * sits around 0.9; noise and a muted string do not reach this. */
#define TUN_CONF_GATE  0.55f
/* McLeod: the first peak clearing this fraction of the best one wins. Lower
 * trusts the longer period more (fewer octave-up errors, more octave-down);
 * 0.85 is the value that paper settles on and it measures correctly here. */
#define TUN_PEAK_FRAC  0.85f

/* HOW LONG THE DETECTOR KEEPS RUNNING AFTER THE LAST READ, IN SAMPLES, and
 * it must be written as a TIME rather than in terms of anything else.
 *
 * This was `TUN_EVERY * 64`, which is an analysis period standing in for a
 * UI's polling interval - two numbers with nothing to do with each other.
 * TUN_EVERY then went 16 -> 8 -> 3 -> 2 for reasons about the analysis
 * rate, shrinking this 8x on the way, and at 2 the threshold was 128
 * samples against `idle += 128` a block: the detector fed TWO BLOCKS after
 * each read and then stopped. The UI reads about 22 times a second against
 * 344 blocks, so 2 blocks in every 15 reached the ring - 13% of the audio,
 * in fragments - and the window correlated to nothing. On the device that
 * is a tuner that says "play a string" however hard you play.
 *
 * Half a second is ten times the UI's polling interval, so the feed is
 * continuous between reads with room for a slow frame, and the detector
 * still goes quiet promptly once the screen is shut. */
#define TUN_IDLE_SAMPLES 22050   /* 0.5 s at 44100 */

typedef struct {
    /* 4 poles at ~700 Hz before decimating by 8. Guitar fundamentals all sit
     * below that, so this is both the anti-alias filter and the thing that
     * stops the harmonics from being what gets measured. */
    fx_bq_t  lp_a, lp_b;
    fx_bqs_t s_a, s_b;
    int      phase;

    float d[TUN_WIN];           /* decimated ring  */
    int   dw;
    float f[TUN_FINE_WIN];      /* full-rate ring  */
    int   fw;
    int   primed;               /* samples seen, capped - see tuner_feed */

    int   blocks;               /* until the next analysis */
    /* The sliced search. step -1 = between cycles, 0..TUN_SLICES-1 = one
     * coarse slice, TUN_SLICES = the fine phase, which fine_i counts. */
    int   step, fine_i;
    float snap[TUN_WIN];        /* the window the coarse slices agree on */
    float psq[TUN_WIN + 1];     /* its prefix sums of squares */
    float nsdf[TUN_LAG_MAX + 2];
    /* The fine phase's own snapshot, for the same reason: seven frames
     * correlating a ring that keeps moving are seven frames measuring
     * seven different signals. */
    float fsnap[TUN_FINE_WIN];
    int   fine_lo, fine_hi, fine_n, fine_centre;
    float fcor[TUN_FINE_LAGS + 2];

    /* Written by the analysis, read by get_param. Plain floats: both run on
     * the SPI callback, so there is no cross-thread story to get wrong. */
    float hz;                   /* 0 = nothing confident */
    float conf;
    float level;
    int   idle;                 /* SAMPLES since the last read - see the top */
} tuner_t;

static void tuner_init(tuner_t *t) {
    memset(t, 0, sizeof(*t));
    /* Butterworth-ish pair: two cascaded 2-poles at the same corner. */
    fx_bq_lowpass(&t->lp_a, 700.0f, 0.54f);
    fx_bq_lowpass(&t->lp_b, 700.0f, 1.31f);
    t->idle = 1 << 20;          /* starts asleep */
    t->step = -1;
}

/* MIDI note number from Hz, A4 = 440. */
static float tuner_midi(float hz) {
    return 69.0f + 12.0f * log2f(hz / 440.0f);
}

static const char *TUN_NAMES[12] = {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};

/* Parabolic vertex of three samples around a maximum, as an offset in
 * [-0.5, 0.5] from the middle one. This is where the sub-sample precision
 * comes from, and without it the fine stage is no better than the coarse. */
static float tuner_vertex(float ym, float y0, float yp) {
    const float den = ym - 2.0f * y0 + yp;
    if (den > -1e-12f && den < 1e-12f) return 0.0f;
    float v = 0.5f * (ym - yp) / den;
    if (v < -1.0f) v = -1.0f;
    if (v > 1.0f) v = 1.0f;
    return v;
}

/* Read the ring oldest-first. The window is contiguous in time, not in
 * memory, and walking it in memory order splices the newest samples in
 * front of the oldest - which correlates a signal against a step. */
static inline float tun_d(const tuner_t *t, int i) {
    int k = t->dw + i;
    if (k >= TUN_WIN) k -= TUN_WIN;
    return t->d[k];
}
static inline float tun_f(const tuner_t *t, int i) {
    int k = t->fw + i;
    if (k >= TUN_FINE_WIN) k -= TUN_FINE_WIN;
    return t->f[k];
}

/* ONE CYCLE BEGINS: take the window, measure its level, build the prefix
 * sums. Everything after this reads `snap`, never the ring, so the eight
 * slices below cannot disagree about what they are looking at. */
static int tuner_snapshot(tuner_t *t) {
    /* PEAK, not RMS - see TUN_LEVEL_GATE. */
    {
        const int tail = TUN_WIN - t->dw;
        memcpy(t->snap, t->d + t->dw, (size_t)tail * sizeof(float));
        memcpy(t->snap + tail, t->d, (size_t)t->dw * sizeof(float));
    }
    float pk = 0.0f;
    for (int i = 0; i < TUN_WIN; i++) {
        const float v = t->snap[i];
        const float a = v < 0.0f ? -v : v;
        if (a > pk) pk = a;
    }
    t->level = pk;
    if (pk < TUN_LEVEL_GATE) { t->hz = 0.0f; t->conf = 0.0f; return 0; }

    /* Prefix sums of squares, so the NSDF denominator is O(1) a lag instead
     * of a second pass per lag. */
    t->psq[0] = 0.0f;
    for (int i = 0; i < TUN_WIN; i++)
        t->psq[i + 1] = t->psq[i] + t->snap[i] * t->snap[i];
    return 1;
}

/* One slice of the lag range. NSDF rather than a bare correlation over
 * r(0): dividing by r(0) biases every answer toward the short lags, which
 * is the octave-UP error this is most at risk of. */
static void tuner_slice(tuner_t *t, int slice) {
    int k0 = TUN_LAG_MIN + slice * TUN_PER_SLICE;
    int k1 = k0 + TUN_PER_SLICE;
    if (k1 > TUN_LAG_MAX + 1) k1 = TUN_LAG_MAX + 1;
    for (int k = k0; k < k1; k++) {
        const int n = TUN_WIN - k;
        float acc = 0.0f;
        for (int i = 0; i < n; i++) acc += t->snap[i] * t->snap[i + k];
        const float e = t->psq[n] + (t->psq[TUN_WIN] - t->psq[k]);
        t->nsdf[k] = (e > 1e-20f) ? (2.0f * acc / e) : 0.0f;
    }
}

/* The coarse slices are all in: pick the period and set the fine phase up.
 * Returns 0 if there is nothing to refine. */
static int tuner_pick(tuner_t *t) {
    float best = -1.0f;
    for (int k = TUN_LAG_MIN; k <= TUN_LAG_MAX; k++)
        if (t->nsdf[k] > best) best = t->nsdf[k];
    if (best < TUN_CONF_GATE) { t->hz = 0.0f; t->conf = best; return 0; }

    /* McLEOD: THE FIRST PEAK THAT CLEARS THE BAR, NOT THE BIGGEST.
     *
     * The biggest peak of a plucked string is often at half its period,
     * because the second harmonic is often the loudest partial. Taking it
     * reports the octave above with full confidence and a centred needle,
     * which is the one failure a tuner must not have. Walking up from the
     * shortest lag and stopping at the first local maximum above
     * TUN_PEAK_FRAC of the best finds the true period instead. */
    const float bar = TUN_PEAK_FRAC * best;
    int kb = -1;
    for (int k = TUN_LAG_MIN + 1; k < TUN_LAG_MAX; k++) {
        if (t->nsdf[k] >= bar && t->nsdf[k] >= t->nsdf[k - 1] &&
            t->nsdf[k] >= t->nsdf[k + 1]) {
            kb = k;
            break;
        }
    }
    if (kb < 0) { t->hz = 0.0f; t->conf = best; return 0; }
    t->conf = t->nsdf[kb];

    /* FINE PHASE, at the full rate. The coarse lag is one decimated sample,
     * which is 21 cents at the top of the range - a tuner that cannot do
     * better than that is a tuner nobody can use. */
    t->fine_centre = kb * TUN_DECIM;
    t->fine_lo = t->fine_centre - TUN_FINE_SPAN;
    t->fine_hi = t->fine_centre + TUN_FINE_SPAN;
    if (t->fine_lo < 2) t->fine_lo = 2;
    if (t->fine_hi > TUN_FINE_WIN - 64) t->fine_hi = TUN_FINE_WIN - 64;
    if (t->fine_hi <= t->fine_lo) {
        t->hz = (float)SAMPLE_RATE / (float)t->fine_centre;
        return 0;
    }
    t->fine_n = TUN_FINE_WIN - t->fine_hi - 1;
    /* Two memcpys: the window is contiguous in memory in two runs, fw to
     * the end and then the start back to fw.
     *
     * NOT A SPEEDUP, and the first version of this comment claimed it was.
     * The guess was that walking 8192 samples through the wrap test was
     * the biggest chunk left in any one frame; measured over 6400 frames
     * it moved nothing at all (p50 19 us and p99 57 either way - the
     * compiler was already handling the walk). Kept because a snapshot is
     * a copy and this says so, not for the performance it does not buy. */
    {
        const int tail = TUN_FINE_WIN - t->fw;
        memcpy(t->fsnap, t->f + t->fw, (size_t)tail * sizeof(float));
        memcpy(t->fsnap + tail, t->f, (size_t)t->fw * sizeof(float));
    }
    t->fine_i = 0;
    return 1;
}

static void tuner_fine_slice(tuner_t *t) {
    int k0 = t->fine_lo + t->fine_i * TUN_FINE_PER;
    int k1 = k0 + TUN_FINE_PER;
    if (k1 > t->fine_hi + 1) k1 = t->fine_hi + 1;
    for (int k = k0; k < k1; k++) {
        float acc = 0.0f;
        for (int i = 0; i < t->fine_n; i++)
            acc += t->fsnap[i] * t->fsnap[i + k];
        t->fcor[k - t->fine_lo] = acc;
    }
}

static void tuner_fine_done(tuner_t *t) {
    float fbest = -1e30f; int fk = t->fine_centre;
    for (int k = t->fine_lo; k <= t->fine_hi; k++) {
        const float v = t->fcor[k - t->fine_lo];
        if (v > fbest) { fbest = v; fk = k; }
    }
    float frac = 0.0f;
    if (fk > t->fine_lo && fk < t->fine_hi) {
        const int o = fk - t->fine_lo;
        frac = tuner_vertex(t->fcor[o - 1], t->fcor[o], t->fcor[o + 1]);
    }
    const float period = (float)fk + frac;
    t->hz = (period > 1.0f) ? ((float)SAMPLE_RATE / period) : 0.0f;
}

/* Called from process_block with the module's INPUT - the dry guitar, ahead
 * of every block. Tuning through a fuzz is tuning the fuzz's idea of the
 * note, and a gate or a tremolo earlier in the chain would chop the window
 * the detector needs.
 *
 * Takes the INTERLEAVED int16 the host handed over, and at UNITY. Reading
 * the module's own mono buffer would be one line shorter and wrong twice:
 * that buffer carries in_gain, so the level gate would move when the input
 * trim did - a tuner that stops seeing the guitar for a reason nobody
 * would look for - and it would need a second scratch buffer to survive
 * the chain writing over it. Pitch does not care about gain. */
static void tuner_feed(tuner_t *t, const int16_t *lr, int n) {
    if (t->idle > TUN_IDLE_SAMPLES) {    /* nobody is looking */
        t->idle += n;
        return;
    }
    t->idle += n;

    for (int i = 0; i < n; i++) {
        const float x = (float)lr[i * 2] / 32768.0f;
        t->f[t->fw] = flush_denormal(x);
        t->fw = (t->fw + 1 >= TUN_FINE_WIN) ? 0 : t->fw + 1;

        float y = fx_bq_run(&t->lp_a, &t->s_a, x);
        y = fx_bq_run(&t->lp_b, &t->s_b, y);
        if (++t->phase >= TUN_DECIM) {
            t->phase = 0;
            t->d[t->dw] = flush_denormal(y);
            t->dw = (t->dw + 1 >= TUN_WIN) ? 0 : t->dw + 1;
            if (t->primed < TUN_WIN) t->primed++;
        }
    }

    /* A WINDOW THAT IS NOT FULL IS NOT A SIGNAL. Analysing a ring that is
     * still half zeros correlates the note against silence and answers a
     * confident nonsense for the first 93 ms, which is exactly when
     * someone is looking at the screen having just opened it. */
    if (t->primed < TUN_WIN) return;

    /* ONE STEP A FRAME, and no step does more than a slice' worth of work.
     * That is the whole point of the state machine: flat, not fast. */
    if (t->step < 0) {
        if (++t->blocks < TUN_EVERY) return;
        t->blocks = 0;
        if (tuner_snapshot(t)) t->step = 0;    /* too quiet -> stay idle */
        return;
    }
    if (t->step < TUN_SLICES) {
        tuner_slice(t, t->step);
        if (++t->step >= TUN_SLICES && !tuner_pick(t)) t->step = -1;
        return;
    }
    tuner_fine_slice(t);
    if (++t->fine_i >= TUN_FINE_STEPS) {
        tuner_fine_done(t);
        t->step = -1;
    }
}

/* "<hz> <note> <octave> <cents> <conf> <level>", or hz 0 for "listening,
 * nothing yet" - which is a real answer and not the same as an empty read.
 * See the tri-state rule: "" would mean the key produced nothing. */
static int tuner_report(const tuner_t *t, char *buf, int buf_len) {
    if (t->hz <= 0.0f) {
        return snprintf(buf, buf_len, "{\"hz\":0,\"lvl\":%.4f,\"conf\":%.3f}",
                        t->level, t->conf);
    }
    const float m = tuner_midi(t->hz);
    int nearest = (int)(m + 0.5f);
    if (m < 0.0f) nearest = (int)(m - 0.5f);
    const float cents = (m - (float)nearest) * 100.0f;
    int pc = nearest % 12;
    if (pc < 0) pc += 12;
    const int oct = (nearest / 12) - 1;
    return snprintf(buf, buf_len,
                    "{\"hz\":%.2f,\"note\":\"%s\",\"oct\":%d,\"cents\":%.1f,"
                    "\"conf\":%.3f,\"lvl\":%.4f}",
                    t->hz, TUN_NAMES[pc], oct, cents, t->conf, t->level);
}

#endif /* A2_TUNER_H */
