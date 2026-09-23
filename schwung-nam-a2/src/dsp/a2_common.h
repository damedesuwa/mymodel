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
#define MAX_IR_RUN 8192
#define MAX_IR_READ 65536


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
/* ======================================================================== */
/* IR BLOCK - a cabinet, as a thing rather than as part of one amp.          */
/* ======================================================================== */

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
} ir_block_t;

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
