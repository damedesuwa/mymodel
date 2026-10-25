/*
 * a2_common.h - the parts of Nam A2 that are not about ONE amp.
 *
 * Nam A2c is eight freely-typed blocks rather than a fixed amp+cab, so the
 * pieces it shares with Nam A2 - the WAV reader, the directory scan, the IR
 * loader and its convolution, the JSON crumbs - are here rather than copied.
 * Copying is what this session spent hours on twice: two chain_params
 * literals that disagreed, and two cab-log formats that let a stale build
 * pass for a fresh one.
 *
 * Everything here is free of any instance type. The block structs at the
 * bottom own their own state, so a host can hold eight of them.
 *
 * NOT YET USED BY NAM A2 ITSELF. That module is shipped and working; moving
 * it onto these headers is a separate change with its own hardware test, and
 * doing both at once would make a regression unattributable.
 */

#ifndef A2_COMMON_H
#define A2_COMMON_H

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cmath>
#include <cstdint>
#include <cstdarg>
#include <ctime>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <algorithm>
#include <atomic>
#include <pthread.h>
#include <new>

#include "NeuralAudio/NeuralModel.h"

extern "C" {
#include "plugin_api_v1.h"
}


#define MAX_MODELS 256
#define MAX_CABS 256
#define MAX_NAME_LEN 128
#define MAX_PATH_LEN 512
#define FRAMES_PER_BLOCK 128
/* What the convolution may run, and what a file may occupy before trimming.
 *
 * These are different numbers for a measured reason. Direct convolution is
 * O(taps) per sample, and on Move the cost is not linear past a point: 4096
 * taps is ~287 us per block and 8192 is ~840, a 2.9x jump for 2x the length,
 * because the IR and its history stop fitting in cache. Against the ~1925 us
 * an FX slot has, a full-quality NAM (1190 us) plus an 8192-tap IR is 2030 us
 * - over budget, which is heard as crackling rather than as anything that
 * names itself. So the RUN length is capped and adjustable, defaulting to
 * 2048 (46 ms), which is a cabinet; past that is the room.
 *
 * The READ length is larger because a cab IR is routinely a 500 ms file
 * (the one that produced the crackle is 24000 frames at 48 kHz) and it has
 * to be resampled from its own rate before any of it can be trimmed. */
/* A CAB IR IS 23 ms AND A REVERB IR IS SECONDS, and the only thing that
 * separates them here is how many taps are RUN. Direct convolution costs
 * ~0.09 us a tap on the device, so the length knob is a CPU knob wearing
 * a time label and the budget is the honest reason it tops out:
 *
 *   2048 taps    46 ms    ~190 us
 *   4096         93 ms    ~380 us
 *   8192        186 ms    ~760 us
 *  16384        372 ms   ~1520 us   (of a 2370 us frame)
 *
 * READ is three seconds so a real hall capture is resampled from all of
 * itself rather than from its first second - what gets dropped is the
 * tail, which is the part the cap is choosing to drop anyway. */
#define MAX_IR_RUN 16384
#define MAX_IR_READ 131072
#define IR_RV_PARAMS 4
#define IR_PRE_MAX 4608          /* 104 ms of pre-delay */


#define SAMPLE_RATE 44100.0f
#define FRAME_BUDGET_US 2370.0
#define CPU_PEAK_DECAY 0.998
#define CPU_WARN_ON  70.0
#define CPU_WARN_OFF 55.0

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const host_api_v1_t *g_host = nullptr;

static void plugin_log(const char *msg) {
    if (g_host && g_host->log) g_host->log(msg);
}

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* A NaN survives clampf - both of its comparisons are false, so the value
 * is returned untouched - and casting a NaN to int16_t is undefined. In
 * practice it lands as an arbitrary bit pattern, so one NaN per sample is
 * indistinguishable from white noise. A neural net fed an unexpected model
 * or a denormal storm can produce them, and this module is built -Ofast,
 * which is -ffinite-math-only: the compiler is entitled to assume NaN and
 * Inf never occur, so isnan() and (v != v) may both be folded to false.
 *
 * Test the exponent bits instead. That cannot be optimised away, costs a
 * compare and a branch that predicts perfectly, and turns a model blowing
 * up into silence rather than into full-scale noise through the speaker. */
/* FLUSH A DENORMAL TO ZERO.
 *
 * Every recursive filter here decays toward zero when the input goes quiet,
 * and on the way it passes through the subnormal range, where the FPU traps
 * to microcode. Measured on the device, from the diag line's own numbers:
 *
 *     signal present (in pk > 0.05)   18 us avg, 25 max
 *     near silence   (in pk <= 0.001) 388 us avg, 891 max
 *
 * Fifty times, on a block whose whole budget is a few microseconds - and it
 * costs the frame exactly when nothing is playing, so it reads as a random
 * dropout rather than as a load. It pushed `Slot fx` to 2627 us against a
 * 2370 us budget and took `blocks/s` from 345 down to 326.
 *
 * Two compares, no library call, and correct under -ffinite-math-only
 * (this is a magnitude test, not a NaN test, so -Ofast cannot fold it).
 * Setting FPCR.FZ would be cheaper still, but this runs on the HOST's SPI
 * callback thread and that flag is per-thread state belonging to somebody
 * else. */
static inline float flush_denormal(float v) {
    return (v > -1e-20f && v < 1e-20f) ? 0.0f : v;
}

static inline int sample_is_zero(float v) { return v == 0.0f; }

static inline float sanitize_sample(float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    if ((bits & 0x7F800000u) == 0x7F800000u) return 0.0f;  /* NaN or Inf */
    return v;
}

/* ======================================================================== */
/* WAV reader - minimal parser for cab IR files                              */
/* ======================================================================== */

/* Read a mono float IR from a WAV file. Supports PCM16/24/32 and
 * float32/64. Multi-channel files are summed to mono. */
static int load_wav_ir(const char *path, float *out, int max_samples,
                       uint32_t *out_rate) {
    FILE *f = fopen(path, "rb");
    if (!f) return 0;

    char riff_id[4];
    uint32_t file_size;
    char wave_id[4];
    if (fread(riff_id, 1, 4, f) != 4 || memcmp(riff_id, "RIFF", 4) != 0) { fclose(f); return 0; }
    if (fread(&file_size, 4, 1, f) != 1) { fclose(f); return 0; }
    if (fread(wave_id, 1, 4, f) != 4 || memcmp(wave_id, "WAVE", 4) != 0) { fclose(f); return 0; }

    uint16_t audio_format = 0, num_channels = 0, bits_per_sample = 0;
    uint32_t sample_rate = 0, data_size = 0;
    bool found_fmt = false, found_data = false;

    while (!found_data) {
        char chunk_id[4];
        uint32_t chunk_size;
        if (fread(chunk_id, 1, 4, f) != 4) break;
        if (fread(&chunk_size, 4, 1, f) != 1) break;

        if (memcmp(chunk_id, "fmt ", 4) == 0) {
            if (chunk_size < 16) { fclose(f); return 0; }
            fread(&audio_format, 2, 1, f);
            fread(&num_channels, 2, 1, f);
            fread(&sample_rate, 4, 1, f);
            uint32_t byte_rate; fread(&byte_rate, 4, 1, f);
            uint16_t block_align; fread(&block_align, 2, 1, f);
            fread(&bits_per_sample, 2, 1, f);
            if (chunk_size > 16) fseek(f, chunk_size - 16, SEEK_CUR);
            found_fmt = true;
        } else if (memcmp(chunk_id, "data", 4) == 0) {
            data_size = chunk_size;
            found_data = true;
        } else {
            fseek(f, chunk_size, SEEK_CUR);
        }
    }

    if (!found_fmt || !found_data) { fclose(f); return 0; }
    if (audio_format != 1 && audio_format != 3) { fclose(f); return 0; }

    int bytes_per_sample = bits_per_sample / 8;
    if (bytes_per_sample <= 0 || num_channels == 0) { fclose(f); return 0; }
    int total_samples = data_size / (bytes_per_sample * num_channels);
    if (total_samples > max_samples) total_samples = max_samples;

    int read_count = 0;
    for (int i = 0; i < total_samples; i++) {
        float sample = 0.0f;
        if (audio_format == 1 && bits_per_sample == 16) {
            int16_t s; fread(&s, 2, 1, f);
            sample = s / 32768.0f;
        } else if (audio_format == 1 && bits_per_sample == 24) {
            uint8_t b[3]; fread(b, 1, 3, f);
            int32_t s = (b[0] | (b[1] << 8) | (b[2] << 16));
            if (s & 0x800000) s |= 0xFF000000;
            sample = s / 8388608.0f;
        } else if (audio_format == 1 && bits_per_sample == 32) {
            int32_t s; fread(&s, 4, 1, f);
            sample = s / 2147483648.0f;
        } else if (audio_format == 3 && bits_per_sample == 32) {
            fread(&sample, 4, 1, f);
        } else if (audio_format == 3 && bits_per_sample == 64) {
            double d; fread(&d, 8, 1, f);
            sample = (float)d;
        } else {
            fclose(f); return 0;
        }
        if (num_channels > 1) {
            fseek(f, bytes_per_sample * (num_channels - 1), SEEK_CUR);
        }
        out[read_count++] = sample;
    }

    fclose(f);

    if (out_rate) *out_rate = sample_rate;

    char msg[MAX_PATH_LEN + 128];
    snprintf(msg, sizeof(msg), "IR read %s (%d samples, %d ch, %d bit, fmt %d)",
             path, read_count, num_channels, bits_per_sample, audio_format);
    plugin_log(msg);

    return read_count;
}

/* ======================================================================== */
/* Gains, names, directory scan                                              */
/* ======================================================================== */
static float db_to_gain(float db) {
    return powf(10.0f, db / 20.0f);
}

static float knob_to_gain(float knob) {
    float db = -24.0f + knob * 36.0f;
    return powf(10.0f, db / 20.0f);
}

static void path_to_name(const char *path, char *name, int name_len) {
    const char *slash = strrchr(path, '/');
    const char *base = slash ? slash + 1 : path;
    const char *dot = strrchr(base, '.');
    int len = dot ? (int)(dot - base) : (int)strlen(base);
    if (len >= name_len) len = name_len - 1;
    memcpy(name, base, len);
    name[len] = '\0';
}

static bool is_model_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".nam") == 0 ||
            strcasecmp(dot, ".json") == 0 ||
            strcasecmp(dot, ".aidax") == 0);
}

static bool is_cab_file(const char *name) {
    const char *dot = strrchr(name, '.');
    if (!dot) return false;
    return (strcasecmp(dot, ".wav") == 0 ||
            strcasecmp(dot, ".ir") == 0);
}

static int scan_directory(const char *dir_path,
                          char names[][MAX_NAME_LEN],
                          char paths[][MAX_PATH_LEN],
                          int max_entries,
                          bool (*filter)(const char *)) {
    int count = 0;
    DIR *dir = opendir(dir_path);
    if (!dir) return 0;

    struct dirent *entry;
    while ((entry = readdir(dir)) != nullptr && count < max_entries) {
        if (entry->d_name[0] == '.') continue;
        if (!filter(entry->d_name)) continue;

        snprintf(paths[count], MAX_PATH_LEN, "%s/%s", dir_path, entry->d_name);
        path_to_name(entry->d_name, names[count], MAX_NAME_LEN);
        count++;
    }
    closedir(dir);

    for (int i = 0; i < count - 1; i++) {
        for (int j = i + 1; j < count; j++) {
            if (strcasecmp(names[i], names[j]) > 0) {
                char tmp_name[MAX_NAME_LEN], tmp_path[MAX_PATH_LEN];
                memcpy(tmp_name, names[i], MAX_NAME_LEN);
                memcpy(tmp_path, paths[i], MAX_PATH_LEN);
                memcpy(names[i], names[j], MAX_NAME_LEN);
                memcpy(paths[i], paths[j], MAX_PATH_LEN);
                memcpy(names[j], tmp_name, MAX_NAME_LEN);
                memcpy(paths[j], tmp_path, MAX_PATH_LEN);
            }
        }
    }

    return count;
}

static int json_get_float(const char *json, const char *key, float *out) {
    if (!json || !key || !out) return -1;
    char search[64];
    int n = snprintf(search, sizeof(search), "\"%s\":", key);
    if (n <= 0 || n >= (int)sizeof(search)) return -1;
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += n;
    while (*p == ' ' || *p == '\t') p++;
    *out = (float)atof(p);
    return 0;
}

static int json_get_int(const char *json, const char *key, int *out) {
    if (!json || !key || !out) return -1;
    char search[64];
    int n = snprintf(search, sizeof(search), "\"%s\":", key);
    if (n <= 0 || n >= (int)sizeof(search)) return -1;
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += n;
    while (*p == ' ' || *p == '\t') p++;
    *out = atoi(p);
    return 0;
}

static int json_get_string(const char *json, const char *key, char *out, int out_len) {
    if (!json || !key || !out || out_len <= 0) return -1;
    char search[64];
    int n = snprintf(search, sizeof(search), "\"%s\":\"", key);
    if (n <= 0 || n >= (int)sizeof(search)) return -1;
    const char *p = strstr(json, search);
    if (!p) return -1;
    p += n;
    int i = 0;
    while (*p && *p != '"' && i < out_len - 1) {
        if (*p == '\\' && p[1]) { out[i++] = p[1]; p += 2; continue; }
        out[i++] = *p++;
    }
    out[i] = '\0';
    return i;
}

static int nam_find_name(char names[][MAX_NAME_LEN], int count, const char *target) {
    if (!target || !target[0]) return -1;
    for (int i = 0; i < count; i++) {
        if (strcmp(names[i], target) == 0) return i;
    }
    return -1;
}

/* --- set_param --- */
/* A peaking bell, one biquad, used for every mid hump and every EQ band.
 * Coefficients are recomputed once per BLOCK, never per sample. */
typedef struct { float b0, b1, b2, a1, a2; } fx_bq_t;
typedef struct { float x1, x2, y1, y2; } fx_bqs_t;

static void fx_bq_peak(fx_bq_t *c, float hz, float q, float db) {
    float A = powf(10.0f, db / 40.0f);
    float w = 2.0f * (float)M_PI * hz / SAMPLE_RATE;
    float sn = sinf(w), cs = cosf(w);
    float alpha = sn / (2.0f * q);
    float a0 = 1.0f + alpha / A;
    c->b0 = (1.0f + alpha * A) / a0;
    c->b1 = (-2.0f * cs) / a0;
    c->b2 = (1.0f - alpha * A) / a0;
    c->a1 = (-2.0f * cs) / a0;
    c->a2 = (1.0f - alpha / A) / a0;
}

/* A plain 2-pole lowpass (RBJ). The peak and the shelf above are both
 * relative to unity and cannot do this: an anti-alias filter has to REMOVE
 * what is above the corner, not tilt it. Two of these in series at the same
 * corner with Q 0.54 and 1.31 make a 4-pole Butterworth, which is what the
 * tuner decimates behind. */
static void fx_bq_lowpass(fx_bq_t *c, float hz, float q) {
    float w = 2.0f * (float)M_PI * hz / SAMPLE_RATE;
    float sn = sinf(w), cs = cosf(w);
    float alpha = sn / (2.0f * q);
    float a0 = 1.0f + alpha;
    c->b0 = ((1.0f - cs) * 0.5f) / a0;
    c->b1 = (1.0f - cs) / a0;
    c->b2 = c->b0;
    c->a1 = (-2.0f * cs) / a0;
    c->a2 = (1.0f - alpha) / a0;
}

static void fx_bq_shelf(fx_bq_t *c, float hz, float db, int high) {
    float A = powf(10.0f, db / 40.0f);
    float w = 2.0f * (float)M_PI * hz / SAMPLE_RATE;
    float sn = sinf(w), cs = cosf(w);
    float alpha = sn / 2.0f * sqrtf((A + 1.0f / A) * (1.0f / 0.9f - 1.0f) + 2.0f);
    float ta = 2.0f * sqrtf(A) * alpha;
    float a0;
    if (high) {
        a0 = (A + 1.0f) - (A - 1.0f) * cs + ta;
        c->b0 = A * ((A + 1.0f) + (A - 1.0f) * cs + ta) / a0;
        c->b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cs) / a0;
        c->b2 = A * ((A + 1.0f) + (A - 1.0f) * cs - ta) / a0;
        c->a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cs) / a0;
        c->a2 = ((A + 1.0f) - (A - 1.0f) * cs - ta) / a0;
    } else {
        a0 = (A + 1.0f) + (A - 1.0f) * cs + ta;
        c->b0 = A * ((A + 1.0f) - (A - 1.0f) * cs + ta) / a0;
        c->b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cs) / a0;
        c->b2 = A * ((A + 1.0f) - (A - 1.0f) * cs - ta) / a0;
        c->a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cs) / a0;
        c->a2 = ((A + 1.0f) + (A - 1.0f) * cs - ta) / a0;
    }
}

static inline float fx_bq_run(const fx_bq_t *c, fx_bqs_t *s, float x) {
    float y = c->b0 * x + c->b1 * s->x1 + c->b2 * s->x2 - c->a1 * s->y1 - c->a2 * s->y2;
    s->x2 = s->x1; s->x1 = flush_denormal(x);
    s->y2 = s->y1; s->y1 = flush_denormal(y);
    return y;
}

/* ======================================================================== */
/* IR BLOCK - a cabinet, as a thing rather than as part of one amp.          */
/* ======================================================================== */

/* ======================================================================== */
/* MIC PLACEMENT, APPLIED AFTER THE IR RATHER THAN BAKED INTO IT            */
/* ======================================================================== */
/*
 * AN IR IS ONE MIC IN ONE PLACE, and that is its whole limitation: the
 * capture already contains a position and nothing downstream can get it
 * back out. What CAN be done is add the DIFFERENCE between where the mic
 * was and where you want it, which is what a real move does to the signal
 * and is four cheap filters plus a delay.
 *
 * Done as a post-filter, not by re-filtering the taps. Same sound either
 * way - convolution is linear, so filtering the IR and filtering its
 * output are the same operation - but a knob turn would mean rebuilding
 * 1024 taps on a thread that must not block, while a biquad coefficient
 * is three transcendentals and takes effect on the next sample. The whole
 * chain costs ~5 us against the convolution's ~90.
 *
 * What each control is modelling, and why it is that shape:
 *
 *  POS   cap -> cone edge. On a 12" speaker the dust cap beams the top
 *        end; walking the mic out to the surround loses it progressively
 *        and the low mids come up as the cone's own break-up is less
 *        direct. A high shelf and a small bell, moving together.
 *
 *  DIST  relative to wherever the capture was taken, because that is the
 *        only honest zero. Three separate things happen at once and they
 *        are three separate filters:
 *          - proximity. A cardioid's bass rise is a near-field effect
 *            that collapses within a few centimetres, so it is an
 *            exponential in distance and not a straight line.
 *          - air and off-beam loss as you back away.
 *          - TIME OF FLIGHT, 1 ms per 34 cm. Inaudible on its own and the
 *            entire point when two cabs are running in the two lanes:
 *            moving one mic back combs against the other exactly as it
 *            does in a room. This is what makes the control worth having
 *            rather than a tone knob with a distance label on it.
 *
 *  MIC   a second character on top of the captured one, so it is OFF by
 *        default. A 57 is not flat and neither is a ribbon; this is the
 *        difference between them, not a mic from nothing.
 *
 *  PHASE the dual-mic control. One sample of polarity.
 *
 * DEFAULTS ARE A BIT-EXACT NO-OP. `active` is false until something is
 * moved off its default, so a board built before this existed sounds
 * identical and pays nothing.
 */
#define MIC_DELAY_MAX 128        /* 2.9 ms - covers +60 cm with room */
#define MIC_PARAMS    5

enum { MIC_OFF = 0, MIC_D57, MIC_D421, MIC_RIBBON, MIC_COND, MIC_KINDS };

typedef struct {
    float p[MIC_PARAMS];        /* the encoders, 0..1, as the wire has them */
    int   active;               /* anything off its default? */

    fx_bq_t  pos_hi, pos_body, prox, air, chr_a, chr_b, damp_lo, damp_hi;
    fx_bqs_t s_pos_hi, s_pos_body, s_prox, s_air, s_chr_a, s_chr_b;
    fx_bqs_t s_damp_lo, s_damp_hi;
    int   use_pos, use_dist, use_chr, use_damp;
    float gain;                 /* level, and polarity, in one multiply */

    float dly[MIC_DELAY_MAX];
    int   dly_w;
    float dly_samples;
} mic_t;

/* 0 = as captured. Dist sits at the CENTRE because the capture is the
 * zero and you can move either way from it; the others are one-sided. */
static void mic_defaults(mic_t *m) {
    memset(m, 0, sizeof(*m));
    m->p[0] = 0.0f;             /* Pos   - at the cap            */
    m->p[1] = 0.5f;             /* Dist  - where it was captured */
    m->p[2] = 0.0f;             /* Mic   - Off                   */
    m->p[3] = 0.0f;             /* Phase - normal                */
    m->p[4] = 0.0f;             /* Damp  - tight, as the IR sits */
}

/* Centimetres, signed, from wherever the capture was taken.
 *
 * LINEAR AND SYMMETRIC, because the knob is LABELLED in centimetres and a
 * curve here would make the printed number a lie. It was exponential for
 * the near field until the UI side was written: 0.005 per detent over
 * +-40 cm is 0.4 cm a click, which is finer than the resolution the
 * curve was there to buy. */
#define MIC_CM_RANGE 40.0f
static float mic_cm(float p) { return (p * 2.0f - 1.0f) * MIC_CM_RANGE; }

static void mic_update(mic_t *m) {
    const float pos = m->p[0];
    const float cm  = mic_cm(m->p[1]);
    const int   knd = (int)(m->p[2] * (MIC_KINDS - 1) + 0.5f);
    const int   inv = (m->p[3] >= 0.5f);

    m->use_pos  = (pos > 0.0005f);
    m->use_dist = (cm > 0.05f || cm < -0.05f);
    m->use_chr  = (knd > MIC_OFF && knd < MIC_KINDS);

    /* WHY AN IR SOUNDS STIFFER THAN THE CAB IT CAME FROM.
     *
     * A speaker is not a flat load. Its impedance PEAKS at the cabinet's
     * resonance - around 95 Hz for a closed 4x12 - and climbs again above
     * a kilohertz with the voice coil's inductance. A valve output stage
     * has a high output impedance and poor damping, so it delivers MORE
     * voltage exactly where the load fights it, and the rig gets a bump
     * at both ends. A solid state amp, or an IR played through an FRFR,
     * does not.
     *
     * The IR cannot contain this: it was measured through ONE amp, and
     * whatever that amp's damping was is baked in as a constant. So this
     * is the knob for which power stage is driving it - 0 is tight, and
     * up is a valve amp letting the cabinet ring.
     */
    const float dmp = m->p[4];
    m->use_damp = (dmp > 0.001f);
    if (m->use_damp) {
        fx_bq_peak (&m->damp_lo,   95.0f, 1.2f, 5.0f * dmp);
        fx_bq_shelf(&m->damp_hi, 3000.0f, 2.5f * dmp, 1);
    }

    m->active   = (m->use_pos || m->use_dist || m->use_chr ||
                   m->use_damp || inv);

    if (m->use_pos) {
        /* Measured spread between a 57 on the cap and the same mic at the
         * surround is 10-14 dB from about 3 kHz up, with the low mids
         * coming up a little as it goes. */
        fx_bq_shelf(&m->pos_hi,  3000.0f, -12.0f * pos, 1);
        fx_bq_peak (&m->pos_body, 450.0f, 0.9f, 2.5f * pos);
    }

    float gain_db = 0.0f;
    m->dly_samples = 0.0f;
    if (m->use_dist) {
        if (cm > 0.0f) {
            /* Backing off: proximity collapses, air takes the top, and
             * the arrival moves later. Level drops a little - not the
             * inverse square, which over this range would make the knob
             * a volume control with a tone side effect. */
            fx_bq_shelf(&m->prox, 120.0f, 0.0f, 0);
            fx_bq_shelf(&m->air, 7000.0f, -2.5f * (cm / MIC_CM_RANGE), 1);
            m->dly_samples = (cm / 34300.0f) * SAMPLE_RATE;
            gain_db = -3.0f * (cm / MIC_CM_RANGE);
        } else {
            /* Closing in: the cardioid's bass rise, and a touch more top
             * because you are further inside the beam. */
            const float c = -cm / MIC_CM_RANGE;        /* 0 .. 1 */
            fx_bq_shelf(&m->prox, 120.0f, 9.0f * c * c, 0);
            fx_bq_shelf(&m->air, 5000.0f, 1.5f * c, 1);
        }
    }
    m->gain = powf(10.0f, gain_db / 20.0f) * (inv ? -1.0f : 1.0f);

    if (m->use_chr) {
        switch (knd) {
            case MIC_D57:                        /* presence, thin below */
                fx_bq_peak (&m->chr_a, 5500.0f, 1.0f, 4.0f);
                fx_bq_shelf(&m->chr_b,   90.0f, -3.0f, 0);
                break;
            case MIC_D421:                       /* fuller, softer top   */
                fx_bq_peak (&m->chr_a, 3500.0f, 0.8f, 2.0f);
                fx_bq_shelf(&m->chr_b,  110.0f, 2.0f, 0);
                break;
            case MIC_RIBBON:                     /* dark and warm        */
                fx_bq_shelf(&m->chr_a, 5000.0f, -7.0f, 1);
                fx_bq_peak (&m->chr_b,  250.0f, 0.8f, 2.0f);
                break;
            default:                             /* condenser: open, airy */
                fx_bq_shelf(&m->chr_a, 9000.0f, 3.0f, 1);
                fx_bq_peak (&m->chr_b,  800.0f, 0.9f, -1.5f);
                break;
        }
    }
}

static void mic_set(mic_t *m, int idx, float v) {
    if (idx < 0 || idx >= MIC_PARAMS) return;
    if (v < 0.0f) v = 0.0f;
    if (v > 1.0f) v = 1.0f;
    if (m->p[idx] == v) return;
    m->p[idx] = v;
    mic_update(m);
}

/* Changing cab must not leave the previous one's filter tails running
 * through the new one, and must not leave a delay line full of it. */
static void mic_reset_state(mic_t *m) {
    memset(&m->s_pos_hi, 0, sizeof(m->s_pos_hi));
    memset(&m->s_pos_body, 0, sizeof(m->s_pos_body));
    memset(&m->s_prox, 0, sizeof(m->s_prox));
    memset(&m->s_air, 0, sizeof(m->s_air));
    memset(&m->s_chr_a, 0, sizeof(m->s_chr_a));
    memset(&m->s_chr_b, 0, sizeof(m->s_chr_b));
    memset(&m->s_damp_lo, 0, sizeof(m->s_damp_lo));
    memset(&m->s_damp_hi, 0, sizeof(m->s_damp_hi));
    memset(m->dly, 0, sizeof(m->dly));
    m->dly_w = 0;
}

static void mic_process(mic_t *m, float *audio, int n) {
    if (!m->active) return;

    if (m->use_pos)
        for (int i = 0; i < n; i++) {
            float x = fx_bq_run(&m->pos_hi, &m->s_pos_hi, audio[i]);
            audio[i] = fx_bq_run(&m->pos_body, &m->s_pos_body, x);
        }
    if (m->use_dist)
        for (int i = 0; i < n; i++) {
            float x = fx_bq_run(&m->prox, &m->s_prox, audio[i]);
            audio[i] = fx_bq_run(&m->air, &m->s_air, x);
        }
    if (m->use_chr)
        for (int i = 0; i < n; i++) {
            float x = fx_bq_run(&m->chr_a, &m->s_chr_a, audio[i]);
            audio[i] = fx_bq_run(&m->chr_b, &m->s_chr_b, x);
        }
    if (m->use_damp)
        for (int i = 0; i < n; i++) {
            float x = fx_bq_run(&m->damp_lo, &m->s_damp_lo, audio[i]);
            audio[i] = fx_bq_run(&m->damp_hi, &m->s_damp_hi, x);
        }

    /* FRACTIONAL, because the whole value of this is the comb it makes
     * against the other lane and a delay that snaps to whole samples
     * moves that comb in steps you can hear as zips. */
    if (m->dly_samples > 0.001f) {
        const float d = (m->dly_samples > (float)(MIC_DELAY_MAX - 2))
                        ? (float)(MIC_DELAY_MAX - 2) : m->dly_samples;
        const int   di = (int)d;
        const float fr = d - (float)di;
        for (int i = 0; i < n; i++) {
            m->dly[m->dly_w] = flush_denormal(audio[i]);
            int r0 = m->dly_w - di;      if (r0 < 0) r0 += MIC_DELAY_MAX;
            int r1 = r0 - 1;             if (r1 < 0) r1 += MIC_DELAY_MAX;
            audio[i] = m->dly[r0] * (1.0f - fr) + m->dly[r1] * fr;
            if (++m->dly_w >= MIC_DELAY_MAX) m->dly_w = 0;
        }
    }

    if (m->gain != 1.0f)
        for (int i = 0; i < n; i++) audio[i] *= m->gain;
}

typedef struct {
    float *ir;           /* resampled, trimmed, normalised taps */
    int    len;          /* how many of them the convolution runs */
    int    run_cap;      /* the ceiling a file is trimmed to */
    float *history;      /* [len-1 carried][FRAMES_PER_BLOCK new] */
    float  norm_db;      /* what normalisation cost, for the load line */
    float  peak;         /* diagnostics */
    float  sum;
    int    index;        /* which entry of the shared cab list */
    char   name[MAX_NAME_LEN];
    mic_t  mic;          /* where the mic is, relative to the capture */

    /* REVERB ONLY. The same struct runs both because the difference
     * between a cabinet and a room is the tap count, not the code - and a
     * block is one type at a time, so two sets of storage would be two
     * ways for one block to be two things. */
    float  rv[IR_RV_PARAMS];
    float  pre[IR_PRE_MAX];       /* pre-delay, up to 100 ms */
    int    pre_w;
    float  hp_x1, hp_y1, hp_a;    /* one-pole low cut */
} ir_block_t;

enum { IR_RV_MIX = 0, IR_RV_LEN, IR_RV_PRE, IR_RV_LOCUT };

/* The four lengths the Len knob offers, in taps. */
static const int IR_RV_TAPS[4] = { 2048, 4096, 8192, 16384 };

static void ir_rv_defaults(ir_block_t *b) {
    b->rv[IR_RV_MIX]   = 0.30f;   /* a pedal's Reverb knob, not a wash   */
    b->rv[IR_RV_LEN]   = 1.0f / 3.0f;  /* 93 ms - the cheap useful one   */
    b->rv[IR_RV_PRE]   = 0.0f;
    b->rv[IR_RV_LOCUT] = 0.0f;    /* off; the knob is 20..400 Hz         */
    b->pre_w = 0;
    b->hp_x1 = b->hp_y1 = 0.0f;
    b->hp_a = 0.0f;
    memset(b->pre, 0, sizeof(b->pre));
}

static int ir_rv_taps(const ir_block_t *b) {
    int i = (int)(b->rv[IR_RV_LEN] * 3.0f + 0.5f);
    if (i < 0) i = 0;
    if (i > 3) i = 3;
    return IR_RV_TAPS[i];
}

/* Direct time-domain convolution, TAP-OUTER.
 *
 * The obvious nest - for each output sample, sum over every tap - reads the
 * history BACKWARDS once per sample, so a 1024-tap IR walks 4 KB backwards
 * 128 times a block and keeps nothing in L1. On the device that measured
 * 1440 us for a single cabinet, which is 91 MMAC/s: a sixtieth of what the
 * core does, and the whole of the crackling this module shipped with. Both
 * nests vectorise, so the disassembly says nothing and only the device's
 * own `Slot fx` number finds it.
 *
 * Inverted, the tap is broadcast once and both arrays are read FORWARD, so
 * the touched window is 4.6 KB and stays in L1 for the block. Measured 3.3x
 * faster on x86 and ~20x on the device once the run length came down.
 */
static void ir_block_process(ir_block_t *b, float *audio, int frames) {
    if (!b->ir || b->len <= 0 || !b->history) return;

    const float *ir = b->ir;
    const int L = b->len;
    float *hist = b->history;
    const int n = frames;

    memmove(hist, hist + n, (size_t)(L - 1) * sizeof(float));
    for (int i = 0; i < n; i++) hist[(L - 1) + i] = flush_denormal(audio[i]);

    float acc[FRAMES_PER_BLOCK];
    for (int i = 0; i < n; i++) acc[i] = 0.0f;

    for (int k = 0; k < L; k++) {
        const float c = ir[k];
        const float *srcp = hist + (L - 1) - k;
        for (int i = 0; i < n; i++) acc[i] += c * srcp[i];
    }

    for (int i = 0; i < n; i++) audio[i] = acc[i];
}

/* A REVERB IS THE SAME CONVOLUTION WITH THE DRY KEPT.
 *
 * The cab runs in place and is 100% wet, which is what a speaker is. A
 * room is not: the dry has to survive, so the wet is a COPY and the two
 * meet at the end. Pre-delay before the convolution (it is a gap before
 * the room answers, not a delay on the answer), low cut after it (what
 * you are cutting is the room's own mud, and cutting before would also
 * thin what feeds it).
 *
 * The dry stays at UNITY and Mix only raises the wet, like the one knob
 * on a reverb pedal - so a block at Mix 0 is transparent rather than
 * quiet, which is also what makes it safe to drop into a live chain.
 */
static void ir_reverb_process(ir_block_t *b, float *audio, float *wet, int n) {
    if (!b->ir || b->len <= 0 || !b->history) return;

    const int   pre = (int)(b->rv[IR_RV_PRE] * (float)(IR_PRE_MAX - 1));
    const float mix = b->rv[IR_RV_MIX] * 1.5f;

    if (pre > 0) {
        for (int i = 0; i < n; i++) {
            b->pre[b->pre_w] = flush_denormal(audio[i]);
            int r = b->pre_w - pre;
            if (r < 0) r += IR_PRE_MAX;
            wet[i] = b->pre[r];
            if (++b->pre_w >= IR_PRE_MAX) b->pre_w = 0;
        }
    } else {
        for (int i = 0; i < n; i++) wet[i] = audio[i];
    }

    ir_block_process(b, wet, n);

    if (b->rv[IR_RV_LOCUT] > 0.001f) {
        /* 20 Hz is off in all but name; 400 Hz is a reverb that stays out
         * of the way of a guitar's body. One pole is enough for a tail. */
        const float hz = 20.0f + b->rv[IR_RV_LOCUT] * 380.0f;
        b->hp_a = expf(-2.0f * (float)M_PI * hz / SAMPLE_RATE);
        for (int i = 0; i < n; i++) {
            const float x = wet[i];
            const float y = b->hp_a * (b->hp_y1 + x - b->hp_x1);
            b->hp_x1 = flush_denormal(x);
            b->hp_y1 = flush_denormal(y);
            wet[i] = y;
        }
    }

    for (int i = 0; i < n; i++) audio[i] += wet[i] * mix;
}

static void ir_block_free(ir_block_t *b) {
    free(b->ir);      b->ir = nullptr;
    free(b->history); b->history = nullptr;
    b->len = 0;
    b->name[0] = '\0';
    b->index = -1;
}

/* Load, resample to the host rate, trim with a fade, normalise, publish.
 *
 * Runs wherever the caller calls it. In Nam A2 that was the SPI callback,
 * which is a realtime violation this module inherits and does not fix - the
 * 8-block host defers it to its own worker instead.
 */
static int ir_block_load(ir_block_t *b, const char *path) {
    float *raw     = (float *)calloc(MAX_IR_READ, sizeof(float));
    float *new_ir  = (float *)calloc(MAX_IR_RUN, sizeof(float));
    if (!raw || !new_ir) { free(raw); free(new_ir); return -1; }

    uint32_t file_rate = 0;
    int raw_len = load_wav_ir(path, raw, MAX_IR_READ, &file_rate);
    if (raw_len <= 0) { free(raw); free(new_ir); return -1; }

    int cap = (b->run_cap > 0) ? b->run_cap : 1024;
    if (cap > MAX_IR_RUN) cap = MAX_IR_RUN;

    /* A cab IR is overwhelmingly a 48 kHz file and Move runs at 44100, so
     * playing the samples as they sit stretches the response 8.8% and puts
     * every resonance in the wrong place. Linear is enough - the speaker
     * already band-limited it - and this runs once, not per block. */
    int want = raw_len;
    if (file_rate > 0 && file_rate != (uint32_t)SAMPLE_RATE)
        want = (int)(raw_len * (double)SAMPLE_RATE / (double)file_rate);
    int resampled_total = want;
    if (want > cap) want = cap;
    if (want < 1) want = 1;

    for (int i = 0; i < want; i++) {
        double srcpos = (file_rate > 0 && file_rate != (uint32_t)SAMPLE_RATE)
                      ? (double)i * (double)file_rate / (double)SAMPLE_RATE
                      : (double)i;
        int i0 = (int)srcpos;
        double fr = srcpos - i0;
        float a = (i0 < raw_len) ? raw[i0] : 0.0f;
        float c = (i0 + 1 < raw_len) ? raw[i0 + 1] : 0.0f;
        new_ir[i] = (float)(a + (c - a) * fr);
    }

    /* Trimming mid-tail leaves a step, which is a click smeared across the
     * whole spectrum. Only when there IS a tail to cut. */
    if (want < resampled_total) {
        int fade = want / 8;
        for (int i = 0; i < fade; i++) {
            float g = (float)(fade - i) / (float)fade;
            new_ir[want - fade + i] *= g * g;
        }
    }

    /* NORMALISE SO THE CABINET NEVER BOOSTS.
     *
     * Not by energy: the file's energy is already near unity while its
     * response peaks +13 dB around 230 Hz, which is a 4x12 with V30s being
     * a 4x12. Energy normalisation therefore moved the output rms from
     * 0.3373 to 0.3383 and the boost went on driving the output into its
     * clamp - 2.31% of samples pinned at full scale on an already-distorted
     * signal, heard as crackling. Dividing by max|H(f)| makes the cabinet a
     * filter that only ever cuts. 64 log-spaced points is coarse on purpose:
     * a speaker's response is smooth. */
    {
        const int an = (want < 2048) ? want : 2048;
        double peak = 0.0;
        for (int j = 0; j < 64; j++) {
            double f = 50.0 * pow(160.0, j / 63.0);
            double w = 2.0 * M_PI * f / (double)SAMPLE_RATE;
            double re = 0.0, im = 0.0;
            for (int k = 0; k < an; k++) {
                double ph = w * k;
                re += new_ir[k] * cos(ph);
                im -= new_ir[k] * sin(ph);
            }
            double mag = sqrt(re * re + im * im);
            if (mag > peak) peak = mag;
        }
        double norm = (peak > 1e-9) ? 1.0 / peak : 1.0;
        for (int i = 0; i < want; i++) new_ir[i] = (float)(new_ir[i] * norm);
        b->norm_db = (float)(20.0 * log10(norm));
    }

    float pk = 0.0f; double sum = 0.0;
    for (int i = 0; i < want; i++) {
        float a = new_ir[i] < 0 ? -new_ir[i] : new_ir[i];
        if (a > pk) pk = a;
        sum += new_ir[i];
    }
    free(raw);

    float *new_hist = (float *)calloc((size_t)(want - 1) + FRAMES_PER_BLOCK, sizeof(float));
    if (!new_hist) { free(new_ir); return -1; }

    float *old_ir = b->ir, *old_hist = b->history;
    b->ir = new_ir;
    b->len = want;
    b->history = new_hist;
    b->peak = pk;
    b->sum = (float)sum;
    path_to_name(path, b->name, MAX_NAME_LEN);
    free(old_ir);
    free(old_hist);
    return want;
}

/* ======================================================================== */
/* DRIVE BLOCK - an overdrive pedal in front of the amp.                    */
/* ======================================================================== */

/* Three curves rather than three pedals. The character of an OD in front of
 * a neural amp comes mostly from how hard it hits the amp and what it does
 * to the low end, not from a faithful circuit - the amp model is doing the
 * expensive part. So: a gain, a shape, a tilt and a level, at ~5 us.
 *
 * DRIVE_OD is a soft asymmetric clip with a bass cut before it (the Tube
 * Screamer move: the amp stays tight because the mud never arrives).
 * DRIVE_DIST is harder and symmetric. DRIVE_FUZZ squares hard and keeps the
 * low end, which is what makes it a fuzz and not a louder distortion.
 */
enum { DRIVE_OD = 0, DRIVE_DIST = 1, DRIVE_FUZZ = 2 };

typedef struct {
    int   mode;
    float drive;      /* 0..1 */
    float tone;       /* 0..1, tilt */
    float level;      /* 0..1 */
    /* one-pole states: pre-emphasis bass cut, post tone tilt */
    float hp_x1, hp_y1;
    float lp_y1;
} drive_block_t;

static void drive_block_process(drive_block_t *d, float *audio, int frames) {
    const float gain = 1.0f + d->drive * d->drive * 60.0f;
    const float out  = d->level;

    /* Bass cut ahead of the clip, only for OD and DIST. ~0.9931 is ~160 Hz
     * at 44100; the fuzz keeps its bottom end on purpose. */
    const float hp_a = (d->mode == DRIVE_FUZZ) ? 0.0f : 0.9931f;
    /* Tone as a one-pole tilt: 0 = dark, 1 = bright. */
    const float lp_a = 0.35f + 0.60f * (1.0f - d->tone);

    for (int i = 0; i < frames; i++) {
        float x = audio[i];

        if (hp_a > 0.0f) {
            float y = x - d->hp_x1 + hp_a * d->hp_y1;
            d->hp_x1 = flush_denormal(x);
            d->hp_y1 = flush_denormal(y);
            x = y;
        }

        x *= gain;

        switch (d->mode) {
            case DRIVE_DIST:
                x = tanhf(x * 1.6f);
                break;
            case DRIVE_FUZZ:
                /* Hard square with a little rounding, then a touch of the
                 * dry back so it is a fuzz rather than a gate. */
                x = (x > 0.0f ? 1.0f : -1.0f) * (1.0f - expf(-fabsf(x) * 3.0f));
                break;
            default: {
                /* Asymmetric soft clip: the negative half folds a little
                 * sooner, which is where the even harmonics come from. */
                float k = (x < 0.0f) ? 1.35f : 1.0f;
                x = tanhf(x * k);
                break;
            }
        }

        d->lp_y1 = flush_denormal(d->lp_y1 + lp_a * (x - d->lp_y1));
        x = d->lp_y1 + (x - d->lp_y1) * d->tone;

        audio[i] = sanitize_sample(x * out * 0.35f);
    }
}

/* ======================================================================== */
/* NAM BLOCK - one amp model, loaded off the callback.                      */
/* ======================================================================== */

typedef struct {
    NeuralAudio::NeuralModel *model;
    std::atomic<NeuralAudio::NeuralModel *> pending;
    std::atomic<bool> loading;

    /* The model's OWN calibration. A .nam records the input level it was
     * captured at and the output level that restores unity, and a high-gain
     * amp is only voiced correctly when both are applied - its distortion
     * character is a function of how hard the input is driven. Published
     * with `pending` and adopted by the same swap, so the pair can never be
     * mismatched. */
    float in_gain, out_gain;                  /* LINEAR, converted at publish */
    float pending_in_gain, pending_out_gain;

    int   quality;     /* 0 Full, 1 Slim, 2 Lite */
    int   index;       /* which entry of the shared model list */
    char  name[MAX_NAME_LEN];
    char  path[MAX_PATH_LEN];

    /* DC blocker after the model: a neural amp has an offset and the cab
     * convolution turns one into a thump on every transient. */
    float dc_x1, dc_y1;
} nam_block_t;

/* Slim and Lite both ask the model for its low tier; Lite additionally
 * halves the rate. On an A2 (SlimmableContainer) .nam the three settings
 * are a real ladder - measured 1.00x, 0.20x, 0.11x - because the low tier is
 * a separately trained smaller net rather than a rate hack. On a plain
 * WaveNet .nam SetQualityScaleFactor is a documented no-op, so Slim costs
 * what Full costs and only Lite does anything. */
static void nam_block_apply_quality(nam_block_t *b) {
    if (!b->model) return;
    float q = (b->quality == 0) ? 1.0f : 0.5f;
    if (!b->model->IsQualityChangeRealtimeSafe(q)) return;
    b->model->SetQualityScaleFactor(q);
}

/* Adopt a model the loader published. Called from the audio thread, which is
 * why it is a plain exchange and never a wait.
 *
 * Returns 1 when a model actually changed hands, so the caller can ramp
 * the block in: two captures do not agree about level and the swap is a
 * step discontinuity otherwise. */
static int nam_block_adopt_pending(nam_block_t *b) {
    NeuralAudio::NeuralModel *fresh = b->pending.exchange(nullptr, std::memory_order_acq_rel);
    if (!fresh) return 0;
    NeuralAudio::NeuralModel *old = b->model;
    b->model = fresh;
    b->in_gain = b->pending_in_gain;
    b->out_gain = b->pending_out_gain;
    b->dc_x1 = b->dc_y1 = 0.0f;
    nam_block_apply_quality(b);
    delete old;
    return 1;
}

static void nam_block_process(nam_block_t *b, float *mono, float *scratch,
                              int frames) {
    if (!b->model) return;

    if (b->quality == 2 && frames >= 2) {
        int half = frames / 2;
        for (int i = 0; i < half; i++)
            scratch[i] = 0.5f * (mono[2 * i] + mono[2 * i + 1]);
        b->model->Process(scratch, scratch, (size_t)half);
        for (int i = 0; i < half; i++) {
            mono[2 * i]     = scratch[i];
            mono[2 * i + 1] = scratch[i];
        }
        if (frames & 1)
            b->model->Process(&mono[frames - 1], &mono[frames - 1], 1);
    } else {
        b->model->Process(mono, mono, (size_t)frames);
    }

    for (int i = 0; i < frames; i++) {
        float x = mono[i];
        float y = x - b->dc_x1 + 0.9971f * b->dc_y1;
        b->dc_x1 = flush_denormal(x);
        b->dc_y1 = flush_denormal(y);
        mono[i] = sanitize_sample(y);
    }
}

#endif /* A2_COMMON_H */
