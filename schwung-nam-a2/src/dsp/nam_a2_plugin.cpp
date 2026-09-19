/*
 * Nam A2 Audio FX Plugin - Neural Amp Modeler for Move Anything
 *
 * Based on the schwung-nam module (https://github.com/charlesvestal/schwung-nam)
 * by Charles Vestal, which wraps NeuralAudio (MIT, by Mike Oliphant) to run
 * .nam / .aidax neural-network guitar-amp models. This module keeps that
 * amp + cab IR core and adds:
 *
 *   - A "Quality" switch (Full / Lite). Lite halves the neural net's work by
 *     running the model at half the block's sample rate (averaging input
 *     pairs, zero-order-hold on the output) - a CPU/quality tradeoff for
 *     models too heavy to run in real time on Move's ARM core alongside
 *     the cab IR.
 *
 * A 3-band EQ and a Doubler/Echo/Reverb "solo" FX chain were both part of
 * this module and have been removed. Everything that is not the amp and its
 * speaker belongs in the host's own chain FX slots instead, where it can be
 * reordered and bypassed independently - and the solo chain's buffers cost
 * ~900 KB of allocation inside create_instance, which runs on the SPI audio
 * callback where allocation is forbidden.
 *
 * Dependencies (all header-only / static, permissive licenses):
 *   NeuralAudio  - MIT      - Mike Oliphant
 *   Eigen        - MPL2     - Eigen contributors
 *   RTNeural     - BSD-3    - Jatin Chowdhury
 *   math_approx  - BSD-3    - Jatin Chowdhury
 *   nlohmann/json- MIT      - Niels Lohmann
 *
 * Audio: 44100 Hz, 128 frames/block, stereo interleaved int16 in-place.
 * Chain: input gain -> NAM model -> DC block -> cab IR -> output gain,
 * mirroring real hardware (preamp -> power amp -> speaker cab).
 * NAM models are mono - we sum L+R to mono, process in mono, then write
 * the result to both output channels.
 */

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
#include <string>
#include <atomic>
#include <pthread.h>

/* NeuralAudio */
#include "NeuralAudio/NeuralModel.h"

/* Move Anything API */
extern "C" {
#include "plugin_api_v1.h"
}

/* ======================================================================== */

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

/* Move's per-frame slack, measured (docs/REALTIME_SAFETY.md and the shim's own
 * spi_timing tally): the SPI transfer is 389 us of a ~2.37 ms frame, the rest
 * is idle IRQ wait that our work eats into. The meter reports a percentage of
 * this, so 100% is "this block alone consumed the whole frame". */
#define FRAME_BUDGET_US 2370.0

/* ~0.3 s to fall by half at 345 blocks/s: long enough to read off a screen
 * that repaints ~4x/s, short enough to follow a quality change. */
#define CPU_PEAK_DECAY 0.998

/* Where the warning latches on and off, as a percentage of FRAME_BUDGET_US. */
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
static inline float sanitize_sample(float v) {
    uint32_t bits;
    memcpy(&bits, &v, sizeof(bits));
    if ((bits & 0x7F800000u) == 0x7F800000u) return 0.0f;  /* NaN or Inf */
    return v;
}

/* ======================================================================== */
/* WAV reader - minimal parser for cab IR files                              */
/* ======================================================================== */

/* Read a mono float IR from a WAV file. Supports PCM16/24/32 and float32/64.
 * Returns number of samples read, or 0 on failure. */
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
    snprintf(msg, sizeof(msg), "Nam A2: loaded cab IR %s (%d samples, %d ch, %d bit, fmt %d)",
             path, read_count, num_channels, bits_per_sample, audio_format);
    plugin_log(msg);

    return read_count;
}

/* ======================================================================== */
/* Instance                                                                  */
/* ======================================================================== */

typedef struct {
    char module_dir[MAX_PATH_LEN];

    /* Model */
    NeuralAudio::NeuralModelLoader *loader; /* owns load-mode config; CreateFromFile is
                                              * an instance method as of NeuralAudio 0.1.x -
                                              * heap-allocated (not a struct member) because
                                              * this whole instance is calloc'd, which would
                                              * skip NeuralModelLoader's constructor and its
                                              * non-zero default member initializers. */
    NeuralAudio::NeuralModel *model;
    std::atomic<NeuralAudio::NeuralModel *> pending_model;
    std::atomic<bool> loading;
    char model_path[MAX_PATH_LEN];
    char model_name[MAX_NAME_LEN];

    int model_count;
    char model_names[MAX_MODELS][MAX_NAME_LEN];
    char model_paths[MAX_MODELS][MAX_PATH_LEN];
    int current_model_index;

    /* Cabinet IR */
    float *cab_ir;
    int cab_ir_len;
    /* How many taps the convolution may run. Set from the cab_length enum;
     * the loaded IR is trimmed to it, so changing it reloads the cab. */
    int cab_run_len;
    float cab_norm_db;   /* what normalisation cost, for the load line */
    float *cab_history;
    int cab_hist_pos;
    bool cab_bypass;
    char cab_name[MAX_NAME_LEN];

    int cab_count;
    char cab_names[MAX_CABS][MAX_NAME_LEN];
    char cab_paths[MAX_CABS][MAX_PATH_LEN];
    int current_cab_index;

    /* Input / output gain */
    float input_level;
    float output_level;
    float input_gain;
    float output_gain;

    /* The model's own calibration. A .nam file records the input level it
     * was captured at and the output level that restores unity, and a model
     * is only voiced correctly when they are applied - a high-gain amp's
     * distortion character is a function of how hard its input is driven,
     * so ignoring these plays the model at the wrong point on its own
     * curve. Published by the loader thread alongside pending_model and
     * adopted by the same swap, so the pair can never be mismatched. */
    float model_in_gain;
    float model_out_gain;
    float pending_in_gain;
    float pending_out_gain;

    /* Quality (0 = Full, 1 = Lite - runs the model at half rate) */
    /* 0 = Full, 1 = Slim, 2 = Lite.
     *
     * Full and Slim are the MODEL'S OWN tiers, not ours. An A2 .nam is a
     * SlimmableContainer: it carries several trained WaveNets and picks one
     * by quality scale. The user's Dual Rectifier capture carries two, at 8
     * and 3 channels, and until now this module always got the 8-channel one
     * because NeuralModelLoader defaults its quality scale to 1.0 and nothing
     * here ever moved it. Measured, the 3-channel tier costs 0.20x of the
     * 8-channel one - and it is a properly trained smaller model rather than
     * a rate hack, so it sounds like the amp rather than like a shortcut.
     *
     * Lite is the fallback that works on anything: half rate, zero-order
     * hold. It is what a plain WaveNet .nam gets, since SetQualityScaleFactor
     * is a no-op on a model with no tiers (NeuralModel's base implementation
     * ignores it). Lite also asks for the low tier, so on an A2 model the
     * three settings are a real ladder: 1.00x, 0.20x, 0.11x. */
    int quality_mode;


    /* Block-time meter. clock_gettime is a vDSO read, not a syscall - the
     * shim times its own callback the same way - so this costs tens of
     * nanoseconds against a block that costs hundreds of microseconds.
     * Peak-held with a slow decay rather than averaged: an average hides
     * exactly the spike that drops a frame. */
    double cpu_us_peak;
    /* Latched over-budget flag. Hysteresis, because the string this drives is
     * DRAWN and, on the master and bus rows, SPOKEN - a value sitting on the
     * threshold would flap the label and talk over itself. */
    int cpu_warn;

    /* DC blocker state (see process_block) */
    float dc_x1;
    float dc_y1;

    /* Audio buffers (avoid per-block allocation) */
    float mono_in[FRAMES_PER_BLOCK];
    float mono_out[FRAMES_PER_BLOCK];
    float mono_in_lite[FRAMES_PER_BLOCK / 2];
    float mono_out_lite[FRAMES_PER_BLOCK / 2];

} nam_a2_instance_t;

/* ======================================================================== */
/* Helpers                                                                   */
/* ======================================================================== */

/* Map 0-1 knob to dB range (-24 to +12), then to linear gain */
/* Quality scale for each mode. Slim and Lite both ask for the model's low
 * tier; Lite additionally halves the rate in process_block. */
static float quality_scale_for(int mode) { return (mode == 0) ? 1.0f : 0.5f; }

/* Runs on the SPI callback, so it must not prewarm. With the loader's default
 * composite mode (LoadAll) every tier is prewarmed when the file is read, so
 * switching is an atomic index store - IsQualityChangeRealtimeSafe says so,
 * and is asked rather than assumed. A refusal leaves the tier alone instead
 * of stalling the callback. */
static void apply_quality(nam_a2_instance_t *inst) {
    if (!inst->model) return;
    float q = quality_scale_for(inst->quality_mode);
    if (!inst->model->IsQualityChangeRealtimeSafe(q)) return;
    inst->model->SetQualityScaleFactor(q);
}

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

static void scan_models(nam_a2_instance_t *inst) {
    char models_dir[MAX_PATH_LEN];
    snprintf(models_dir, sizeof(models_dir), "%s/models", inst->module_dir);
    inst->model_count = scan_directory(models_dir, inst->model_names, inst->model_paths,
                                       MAX_MODELS, is_model_file);
}

static void scan_cabs(nam_a2_instance_t *inst) {
    char cabs_dir[MAX_PATH_LEN];
    snprintf(cabs_dir, sizeof(cabs_dir), "%s/cabs", inst->module_dir);
    inst->cab_count = scan_directory(cabs_dir, inst->cab_names, inst->cab_paths,
                                     MAX_CABS, is_cab_file);
}

static void load_cab(nam_a2_instance_t *inst, int index) {
    if (index < 0 || index >= inst->cab_count) return;

    float *raw = (float *)calloc(MAX_IR_READ, sizeof(float));
    float *new_ir = (float *)calloc(MAX_IR_RUN, sizeof(float));
    if (!raw || !new_ir) { free(raw); free(new_ir); return; }

    uint32_t file_rate = 0;
    int raw_len = load_wav_ir(inst->cab_paths[index], raw, MAX_IR_READ, &file_rate);
    if (raw_len <= 0) {
        free(raw); free(new_ir);
        char msg[MAX_PATH_LEN + 64];
        snprintf(msg, sizeof(msg), "Nam A2: failed to load cab IR %s", inst->cab_paths[index]);
        plugin_log(msg);
        return;
    }

    /* Resample to the host rate. Cab IRs are overwhelmingly 48 kHz files and
     * Move runs at 44100, so playing the samples as they sit stretches the
     * whole response 8.8% - the cabinet's resonances land in the wrong place.
     * Linear interpolation is enough here: an IR is already band-limited by
     * the speaker, and this runs once at load, not per block. */
    int want = raw_len;
    if (file_rate > 0 && file_rate != (uint32_t)SAMPLE_RATE) {
        double ratio = (double)SAMPLE_RATE / (double)file_rate;
        want = (int)(raw_len * ratio);
    }
    if (want > inst->cab_run_len) want = inst->cab_run_len;
    if (want > MAX_IR_RUN) want = MAX_IR_RUN;
    if (want < 1) want = 1;

    for (int i = 0; i < want; i++) {
        double src = (file_rate > 0 && file_rate != (uint32_t)SAMPLE_RATE)
                   ? (double)i * (double)file_rate / (double)SAMPLE_RATE
                   : (double)i;
        int i0 = (int)src;
        double fr = src - i0;
        float a = (i0 < raw_len) ? raw[i0] : 0.0f;
        float b = (i0 + 1 < raw_len) ? raw[i0 + 1] : 0.0f;
        new_ir[i] = (float)(a + (b - a) * fr);
    }

    /* Trimming an IR mid-tail leaves a step, which is a click smeared across
     * the whole spectrum. Fade the last eighth out so the response ends where
     * it is cut. Only when there IS a tail to cut - a file shorter than the
     * run length ends on its own. */
    int resampled_total = (file_rate > 0 && file_rate != (uint32_t)SAMPLE_RATE)
        ? (int)(raw_len * (double)SAMPLE_RATE / (double)file_rate) : raw_len;
    if (want < resampled_total) {
        int fade = want / 8;
        for (int i = 0; i < fade; i++) {
            float g = (float)(fade - i) / (float)fade;
            new_ir[want - fade + i] *= g * g;
        }
    }

    /* NORMALISE SO THE CABINET NEVER BOOSTS.
     *
     * A cab IR is a recording and its level is an accident of the capture,
     * but the trap is subtler than a level: the file's ENERGY is already
     * close to unity here (sum h^2 = 1.18, a 0.7 dB correction) while its
     * frequency response peaks at +13 dB around 230 Hz, which is a 4x12 with
     * V30s doing what a 4x12 does. Energy normalisation therefore changed
     * nothing measurable - tried, and the output rms moved from 0.3373 to
     * 0.3383 - and the boost went on driving the output into its clamp: at a
     * -8 dBFS input, 2.31% of samples pinned at full scale, which on an
     * already-distorted signal is heard as crackling.
     *
     * Dividing by max|H(f)| makes the cabinet a filter that only ever cuts,
     * so it cannot cause clipping at any frequency and the cab switch stops
     * being a loudness switch. The DFT is coarse on purpose - a speaker's
     * response is smooth, so 64 log-spaced points across the band a guitar
     * cab actually shapes find the peak - and it is bounded to 2048 taps
     * because this runs in load_cab, which is on the SPI callback.
     *
     * sum|h| would also guarantee no clipping and needs no transform, but it
     * is the bound for the worst possible input rather than for this filter:
     * 13.0 here, a 22.3 dB cut where 13.0 dB is what the response asks for. */
    {
        const int an = (want < 2048) ? want : 2048;
        double peak = 0.0;
        for (int j = 0; j < 64; j++) {
            /* 50 Hz .. 8 kHz, log spaced */
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
        inst->cab_norm_db = (float)(20.0 * log10(norm));
    }

    free(raw);
    int ir_len = want;

    float *old_ir = inst->cab_ir;
    float *old_hist = inst->cab_history;

    inst->cab_ir = new_ir;
    inst->cab_ir_len = ir_len;
    inst->current_cab_index = index;
    path_to_name(inst->cab_paths[index], inst->cab_name, MAX_NAME_LEN);

    /* Doubled length: apply_cab_ir() writes each sample at BOTH pos and
     * pos + hist_len so its inner loop can read backward as a contiguous
     * slice with no wraparound branch - see the comment there. */
    inst->cab_history = (float *)calloc(2 * (ir_len + FRAMES_PER_BLOCK), sizeof(float));
    inst->cab_hist_pos = 0;

    free(old_ir);
    free(old_hist);

    char msg[MAX_PATH_LEN + 64];
    snprintf(msg, sizeof(msg),
             "Nam A2: cab '%s' %d Hz -> %d Hz, %d run taps (%.0f ms), limit %d, gain %+.1f dB",
             inst->cab_name, (int)file_rate, (int)SAMPLE_RATE, ir_len,
             1000.0f * ir_len / SAMPLE_RATE, inst->cab_run_len,
             inst->cab_norm_db);
    plugin_log(msg);
}

/* Direct time-domain convolution via a DOUBLED circular history buffer.
 *
 * The original (schwung-nam-derived) version kept a single hist_len-sized
 * buffer and stepped the read pointer backward with
 * `if (--p < 0) p = hist_len - 1;` inside the innermost per-tap loop. That
 * branch is evaluated ir_len times per output sample - for a several-
 * thousand-tap cabinet IR that is the hot loop, and a data-dependent
 * branch there defeats auto-vectorization entirely (measured ~7.8x slower
 * than the branch-free version below at -Ofast on a 4096-tap IR - at
 * -Ofast that one branchy loop alone was already ~22% of the 128-sample
 * real-time budget, before the NAM model, EQ or solo FX chain get their
 * share, which is what produced audible crackling with a normal-length
 * cab IR loaded).
 *
 * Fix: `cab_history` is allocated at 2x size (see load_cab) and every
 * sample is written at BOTH pos and pos + hist_len. Reading backward from
 * `&hist[pos + hist_len]` is then always a contiguous, branch-free slice -
 * `h[-k]` for k in [0, ir_len) never needs to wrap, because the mirrored
 * copy is already sitting where the wrapped read would have landed. This
 * changes nothing about the output (verified sample-for-sample identical
 * against the old implementation, aside from float-reassociation-level
 * noise from vectorization, ~1e-5) - it only removes the branch so the
 * compiler can vectorize it.
 */
static void apply_cab_ir(nam_a2_instance_t *inst, float *audio, int frames) {
    if (!inst->cab_ir || inst->cab_ir_len <= 0 || !inst->cab_history) return;

    const float *ir = inst->cab_ir;
    const int ir_len = inst->cab_ir_len;
    float *hist = inst->cab_history;
    const int hist_len = ir_len + FRAMES_PER_BLOCK;
    int pos = inst->cab_hist_pos;

    for (int i = 0; i < frames; i++) {
        float x = audio[i];
        hist[pos] = x;
        hist[pos + hist_len] = x;

        const float *h = &hist[pos + hist_len]; /* h[0] = newest sample, h[-1] = previous, ... */
        float sum = 0.0f;
        for (int k = 0; k < ir_len; k++) {
            sum += ir[k] * h[-k];
        }

        audio[i] = sum;
        if (++pos >= hist_len) pos = 0;
    }

    inst->cab_hist_pos = pos;
}

static void *model_loader_thread(void *arg) {
    nam_a2_instance_t *inst = (nam_a2_instance_t *)arg;

    char msg[MAX_PATH_LEN + 64];
    snprintf(msg, sizeof(msg), "Nam A2: loading model %s", inst->model_path);
    plugin_log(msg);

    NeuralAudio::NeuralModel *new_model = inst->loader->CreateFromFile(inst->model_path);

    if (new_model) {
        snprintf(msg, sizeof(msg), "Nam A2: model loaded successfully (sample_rate=%.0f)",
                 new_model->GetSampleRate());
        plugin_log(msg);
    } else {
        snprintf(msg, sizeof(msg), "Nam A2: failed to load model %s", inst->model_path);
        plugin_log(msg);
    }

    if (new_model) {
        inst->pending_in_gain  = db_to_gain(new_model->GetRecommendedInputDBAdjustment());
        inst->pending_out_gain = db_to_gain(new_model->GetRecommendedOutputDBAdjustment());
    } else {
        inst->pending_in_gain = 1.0f;
        inst->pending_out_gain = 1.0f;
    }

    /* Release-stores the model last, so the two gains above are visible to
     * any thread that sees the pointer. */
    inst->pending_model.store(new_model, std::memory_order_release);
    inst->loading.store(false, std::memory_order_release);

    return nullptr;
}

static void load_model_async(nam_a2_instance_t *inst, const char *path) {
    if (inst->loading.load(std::memory_order_acquire)) {
        plugin_log("Nam A2: already loading a model, skipping");
        return;
    }

    strncpy(inst->model_path, path, MAX_PATH_LEN - 1);
    inst->model_path[MAX_PATH_LEN - 1] = '\0';
    path_to_name(path, inst->model_name, MAX_NAME_LEN);

    inst->loading.store(true, std::memory_order_release);

    pthread_t thread;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&thread, &attr, model_loader_thread, inst);
    pthread_attr_destroy(&attr);
}

/* ======================================================================== */
/* audio_fx_api_v2 implementation                                            */
/* ======================================================================== */

#define AUDIO_FX_API_VERSION_2 2
#define AUDIO_FX_INIT_V2_SYMBOL "move_audio_fx_init_v2"

typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *config_json);
    void (*destroy_instance)(void *instance);
    void (*process_block)(void *instance, int16_t *audio_inout, int frames);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
} audio_fx_api_v2_t;

typedef audio_fx_api_v2_t* (*audio_fx_init_v2_fn)(const host_api_v1_t *host);

/* --- create_instance --- */
static void* v2_create_instance(const char *module_dir, const char *config_json) {
    (void)config_json;

    nam_a2_instance_t *inst = (nam_a2_instance_t *)calloc(1, sizeof(nam_a2_instance_t));
    if (!inst) return nullptr;

    inst->loader = new NeuralAudio::NeuralModelLoader();
    inst->loader->SetDefaultMaxAudioBufferSize(FRAMES_PER_BLOCK);
    /* NeuralModelLoader defaults externalSampleRate to 48000, and Move runs
     * at 44100. The loader uses it to decide whether a model needs
     * oversampling (OversampleNAMConfig), so leaving it at the default
     * describes a host that is not this one. */
    inst->loader->SetExternalSampleRate((int)SAMPLE_RATE);

    strncpy(inst->module_dir, module_dir, MAX_PATH_LEN - 1);
    inst->model = nullptr;
    inst->pending_model.store(nullptr);
    inst->loading.store(false);
    inst->current_model_index = -1;

    /* Cabinet IR defaults */
    inst->cab_ir = nullptr;
    inst->cab_ir_len = 0;
    inst->cab_run_len = 1024;   /* 23 ms. See cab_length. */
    inst->cab_history = nullptr;
    inst->cab_hist_pos = 0;
    inst->cab_bypass = false;
    inst->cab_name[0] = '\0';
    inst->current_cab_index = -1;

    /* Input/output/quality defaults */
    inst->input_level = 0.5f;
    /* 0.85, not 0.5. The knob maps 0..1 to -24..+12 dB, so 0.5 is -6 dB -
     * and a saturated amp model's output is a near-constant ~-22 dBFS
     * whatever you feed it, so -6 dB on top of that is simply quiet. The
     * level matters more than usual here because Move monitors its own line
     * input on a hardware path this module cannot see or remove: the dry
     * guitar is a fixed level, so the only thing that changes the balance
     * is how loud the amp is. Measured on a -34 dBFS input: 0.5 gives rms
     * 0.075 / peak 0.11, 0.85 gives 0.320 / 0.48, 1.0 gives 0.595 / 0.89,
     * none of them clipping a sample. 0.85 is +12.6 dB over the old default
     * with 6 dB still in hand for a hotter model. */
    inst->output_level = 0.85f;
    /* Derived from the levels above, never from a repeated literal - the
     * pair silently disagreed the moment the output default moved. */
    inst->input_gain  = knob_to_gain(inst->input_level);
    inst->output_gain = knob_to_gain(inst->output_level);
    inst->quality_mode = 0;   /* Full */
    inst->cpu_us_peak = 0.0;
    inst->cpu_warn = 0;

    inst->model_in_gain = 1.0f;
    inst->model_out_gain = 1.0f;
    inst->pending_in_gain = 1.0f;
    inst->pending_out_gain = 1.0f;


    /* Scan for model/cab files and load the first of each */
    scan_models(inst);
    scan_cabs(inst);

    if (inst->model_count > 0) {
        inst->current_model_index = 0;
        load_model_async(inst, inst->model_paths[0]);
    }
    if (inst->cab_count > 0) {
        load_cab(inst, 0);
    }

    return inst;
}

/* --- destroy_instance --- */
static void v2_destroy_instance(void *instance) {
    nam_a2_instance_t *inst = (nam_a2_instance_t *)instance;
    if (!inst) return;

    while (inst->loading.load(std::memory_order_acquire)) {
        struct timespec ts = {0, 10000000}; /* 10ms */
        nanosleep(&ts, nullptr);
    }

    NeuralAudio::NeuralModel *pending = inst->pending_model.load(std::memory_order_acquire);
    if (pending) delete pending;
    if (inst->model) delete inst->model;
    delete inst->loader;

    free(inst->cab_ir);
    free(inst->cab_history);

    free(inst);
    plugin_log("Nam A2: instance destroyed");
}

/* --- process_block --- */
static void v2_process_block(void *instance, int16_t *audio_inout, int frames) {
    nam_a2_instance_t *inst = (nam_a2_instance_t *)instance;
    if (!inst) return;

    struct timespec cpu_t0;
    clock_gettime(CLOCK_MONOTONIC, &cpu_t0);

    /* Check for newly loaded model (lock-free swap) */
    NeuralAudio::NeuralModel *pending = inst->pending_model.load(std::memory_order_acquire);
    if (pending) {
        NeuralAudio::NeuralModel *old = inst->model;
        inst->model = pending;
        apply_quality(inst);      /* a new model starts at the loader's 1.0 */
        inst->model_in_gain = inst->pending_in_gain;
        inst->model_out_gain = inst->pending_out_gain;
        inst->pending_model.store(nullptr, std::memory_order_release);
        if (old) delete old;
    }

    /* No model loaded - pass through untouched (matches schwung-nam) */
    if (!inst->model) { inst->cpu_us_peak *= CPU_PEAK_DECAY; return; }

    int n = (frames > FRAMES_PER_BLOCK) ? FRAMES_PER_BLOCK : frames;

    /* Deinterleave stereo int16 -> mono float, with input gain */
    float ig = inst->input_gain * inst->model_in_gain;
    for (int i = 0; i < n; i++) {
        float l = audio_inout[i * 2]     / 32768.0f;
        float r = audio_inout[i * 2 + 1] / 32768.0f;
        /* LEFT only. A mono guitar reaches Move through a TRS jack with its
         * ring tied to sleeve, so the right channel is silent - Move's own
         * setting says `monoFromLeftChannel`. Averaging L+R would drive the
         * model at (L + 0) / 2. A stereo source into a mono guitar amp is
         * not a case worth a control. */
        (void)r;
        inst->mono_in[i] = l * ig;
    }

    /* NAM model - Full runs every sample; Lite halves the neural net's work
     * by averaging input pairs, running the model at half rate, and holding
     * each output sample for two frames. */
    if (inst->quality_mode == 2 && n >= 2) {
        int half = n / 2;
        for (int i = 0; i < half; i++) {
            inst->mono_in_lite[i] = 0.5f * (inst->mono_in[2 * i] + inst->mono_in[2 * i + 1]);
        }
        inst->model->Process(inst->mono_in_lite, inst->mono_out_lite, (size_t)half);
        for (int i = 0; i < half; i++) {
            inst->mono_out[2 * i] = inst->mono_out_lite[i];
            inst->mono_out[2 * i + 1] = inst->mono_out_lite[i];
        }
        if (n & 1) {
            /* Odd leftover sample (block sizes are always even in practice) -
             * process it directly so nothing is dropped. */
            inst->model->Process(&inst->mono_in[n - 1], &inst->mono_out[n - 1], 1);
        }
    } else {
        inst->model->Process(inst->mono_in, inst->mono_out, (size_t)n);
    }

    /* DC blocker. Fed exact silence this model settles to a constant
     * 0.00114 (-58.8 dBFS) rather than to zero - measured, and normal: it
     * is the amp's idle bias point, not a fault. Left in it wastes
     * headroom and offsets everything downstream. One-pole high pass at
     * about 20 Hz. */
    for (int i = 0; i < n; i++) {
        float x = inst->mono_out[i];
        float y = x - inst->dc_x1 + 0.9971f * inst->dc_y1;
        inst->dc_x1 = x;
        inst->dc_y1 = y;
        inst->mono_out[i] = y;
    }

    /* Cab IR convolution - the speaker is the last acoustic stage */
    if (!inst->cab_bypass && inst->cab_ir) {
        apply_cab_ir(inst, inst->mono_out, n);
    }

    /* Output gain, then back to stereo int16 (mono source written to both) */
    float og = inst->output_gain * inst->model_out_gain;
    for (int i = 0; i < n; i++) {
        float s = clampf(sanitize_sample(inst->mono_out[i] * og), -1.0f, 1.0f);
        int16_t sample = (int16_t)(s * 32767.0f);
        audio_inout[i * 2]     = sample;
        audio_inout[i * 2 + 1] = sample;
    }

    {
        struct timespec cpu_t1;
        clock_gettime(CLOCK_MONOTONIC, &cpu_t1);
        double us = (cpu_t1.tv_sec - cpu_t0.tv_sec) * 1e6
                  + (cpu_t1.tv_nsec - cpu_t0.tv_nsec) / 1e3;
        /* Peak-hold with a slow decay. An average would hide the one block
         * that overruns, which is the only block worth seeing. */
        double decayed = inst->cpu_us_peak * CPU_PEAK_DECAY;
        inst->cpu_us_peak = (us > decayed) ? us : decayed;

        /* Schmitt trigger on the frame budget. Above CPU_WARN_ON the block is
         * close enough to the whole frame that anything else in the chain
         * pushes it over, and a missed deadline is heard as a rapid stutter
         * rather than as anything that names itself. */
        double pct = 100.0 * inst->cpu_us_peak / FRAME_BUDGET_US;
        if (pct > CPU_WARN_ON)       inst->cpu_warn = 1;
        else if (pct < CPU_WARN_OFF) inst->cpu_warn = 0;
    }
}

/* Minimal JSON field readers for state restore — encoder writes a flat
 * {"key":value,...} object so strstr + atof/sscanf is enough. */
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
static void v2_set_param(void *instance, const char *key, const char *val) {
    nam_a2_instance_t *inst = (nam_a2_instance_t *)instance;
    if (!inst || !key || !val) return;

    /* Bulk state restore from get_param("state"). Match model/cab by saved
     * name first (survives reordering after rescans), fall back to index. */
    if (strcmp(key, "state") == 0) {
        float f; int i; char name[MAX_NAME_LEN];

        if (json_get_float(val, "input_level", &f) == 0) {
            inst->input_level = clampf(f, 0.0f, 1.0f);
            inst->input_gain  = knob_to_gain(inst->input_level);
        }
        if (json_get_float(val, "output_level", &f) == 0) {
            inst->output_level = clampf(f, 0.0f, 1.0f);
            inst->output_gain  = knob_to_gain(inst->output_level);
        }
        if (json_get_int(val, "quality", &i) == 0) {
            inst->quality_mode = (i < 0) ? 0 : (i > 2) ? 2 : i;
            apply_quality(inst);
        }

        int target_model = -1;
        if (json_get_string(val, "model_name", name, sizeof(name)) > 0)
            target_model = nam_find_name(inst->model_names, inst->model_count, name);
        if (target_model < 0 && json_get_int(val, "model_index", &i) == 0)
            if (i >= 0 && i < inst->model_count) target_model = i;
        if (target_model >= 0 && target_model != inst->current_model_index) {
            inst->current_model_index = target_model;
            load_model_async(inst, inst->model_paths[target_model]);
        }

        int target_cab = -1;
        if (json_get_string(val, "cab_name", name, sizeof(name)) > 0)
            target_cab = nam_find_name(inst->cab_names, inst->cab_count, name);
        if (target_cab < 0 && json_get_int(val, "cab_index", &i) == 0)
            if (i >= 0 && i < inst->cab_count) target_cab = i;
        if (target_cab >= 0 && target_cab != inst->current_cab_index)
            load_cab(inst, target_cab);
        if (json_get_int(val, "cab_run_len", &i) == 0 && i >= 1024 && i <= MAX_IR_RUN)
            inst->cab_run_len = i;
        if (json_get_int(val, "cab_bypass", &i) == 0) inst->cab_bypass = (i != 0);



        return;
    }

    if (strcmp(key, "input_level") == 0) {
        inst->input_level = clampf(atof(val), 0.0f, 1.0f);
        inst->input_gain = knob_to_gain(inst->input_level);
    } else if (strcmp(key, "output_level") == 0) {
        inst->output_level = clampf(atof(val), 0.0f, 1.0f);
        inst->output_gain = knob_to_gain(inst->output_level);
    } else if (strcmp(key, "quality") == 0) {
        int m = atoi(val);
        inst->quality_mode = (m < 0) ? 0 : (m > 2) ? 2 : m;
        apply_quality(inst);
    } else if (strcmp(key, "model_index") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < inst->model_count && idx != inst->current_model_index) {
            inst->current_model_index = idx;
            load_model_async(inst, inst->model_paths[idx]);
        }
    } else if (strcmp(key, "model") == 0) {
        load_model_async(inst, val);
    } else if (strcmp(key, "cab_index") == 0) {
        int idx = atoi(val);
        if (idx >= 0 && idx < inst->cab_count && idx != inst->current_cab_index) {
            load_cab(inst, idx);
        }
    } else if (strcmp(key, "cab_length") == 0) {
        static const int lens[4] = { 1024, 2048, 4096, 8192 };
        int i = atoi(val);
        if (i < 0) i = 0; if (i > 3) i = 3;
        if (lens[i] != inst->cab_run_len) {
            inst->cab_run_len = lens[i];
            /* The trim happens at load, so the cab has to be read again.
             * load_cab allocates - it already did, on this same callback,
             * when the cab was first chosen - and this is a deliberate user
             * action rather than something that happens while playing. */
            if (inst->current_cab_index >= 0) load_cab(inst, inst->current_cab_index);
        }
    } else if (strcmp(key, "cab_bypass") == 0) {
        inst->cab_bypass = (atoi(val) != 0);
    }
}

/* --- get_param --- */
static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    nam_a2_instance_t *inst = (nam_a2_instance_t *)instance;
    if (!inst || !key || !buf) return -1;

    /* The host asks every component for these on entry and keeps asking.
     * Answering -1 means "not implemented", which js_shadow_get_param turns
     * into `null` - and `null` does not mean "this module has no preset
     * name", it means THE READ DID NOT COMPLETE (claim refused, timed out,
     * answered by someone else). The UI's correct response to that is to
     * retry, so a module that simply has no preset loops forever: measured
     * on device at ~11 failed reads per second, each one an IPC round trip
     * the draw budget has to pay for.
     *
     * "Served, and the key produced nothing" is the empty string - a
     * return of 0. chain_host.c makes exactly this clamp for split_voices
     * and says why in the same words. Answer it properly instead.
     *
     * display_name is deliberately NOT handled the same way. The host
     * already backs that one off to ~32s for a module that answers -1
     * (pollFxDisplayName), and it resets the backoff on any non-null
     * answer - so returning "" there would turn a key that costs almost
     * nothing into a poll every second, the opposite of this fix.
     *
     * Nor is this a catch-all `return 0` for unknown keys: chain_host falls
     * back to module.json's own chain_params when the plugin answers -1 for
     * that key, and an empty answer would look authoritative and suppress
     * the fallback. */
    /* The chain-edit line draws `<prefix>:name` and re-reads it about twice a
     * second, which makes it the one string here that is both VISIBLE and
     * live. So the CPU warning rides on it.
     *
     * Serving it at all also stops a retry storm: an unserved key answers -1,
     * the host turns that into `null` - "the read did not complete" - and a
     * failed read is never cached, so the device logged ~50 failed reads per
     * second on fx1:name alone. Same defect as preset_name below.
     *
     * The string is STABLE by construction. It carries no percentage, and the
     * flag behind it has hysteresis, because display_name is SPOKEN on change
     * and a number on the threshold would talk over itself. */
    if (strcmp(key, "name") == 0) {
        return snprintf(buf, buf_len, "%s", inst->cpu_warn ? "Nam A2 CPU!" : "Nam A2");
    }
    if (strcmp(key, "display_name") == 0) {
        /* Only while warning. Answering nothing the rest of the time keeps the
         * host's backoff on this key, which is what it is for. */
        if (!inst->cpu_warn) return -1;
        return snprintf(buf, buf_len, "Nam A2 CPU overload");
    }

    if (strcmp(key, "preset_name") == 0) {
        if (buf_len > 0) buf[0] = '\0';
        return 0;
    }

    /* Bulk serialization for slot autosave. */
    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"input_level\":%.4f,\"output_level\":%.4f,\"quality\":%d,"
            "\"model_index\":%d,\"model_name\":\"%s\","
            "\"cab_index\":%d,\"cab_name\":\"%s\",\"cab_bypass\":%d,\"cab_run_len\":%d}",
            inst->input_level, inst->output_level, inst->quality_mode,
            inst->current_model_index, inst->model_name,
            inst->current_cab_index, inst->cab_name, inst->cab_bypass ? 1 : 0, inst->cab_run_len);
    }

    if (strcmp(key, "input_level") == 0) return snprintf(buf, buf_len, "%.2f", inst->input_level);
    if (strcmp(key, "output_level") == 0) return snprintf(buf, buf_len, "%.2f", inst->output_level);
    if (strcmp(key, "quality") == 0) return snprintf(buf, buf_len, "%d", inst->quality_mode);


    if (strcmp(key, "model_name") == 0)
        return snprintf(buf, buf_len, "%s", inst->model_name[0] ? inst->model_name : "(none)");
    if (strcmp(key, "model_count") == 0) return snprintf(buf, buf_len, "%d", inst->model_count);
    if (strcmp(key, "model_index") == 0) return snprintf(buf, buf_len, "%d", inst->current_model_index);

    if (strcmp(key, "model_list") == 0) {
        scan_models(inst);
        int written = 0;
        written += snprintf(buf + written, buf_len - written, "[");
        for (int i = 0; i < inst->model_count && written < buf_len - 40; i++) {
            if (i > 0) written += snprintf(buf + written, buf_len - written, ",");
            written += snprintf(buf + written, buf_len - written,
                "{\"label\":\"%s\",\"index\":%d}", inst->model_names[i], i);
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }

    if (strcmp(key, "loading") == 0)
        return snprintf(buf, buf_len, "%d", inst->loading.load(std::memory_order_acquire) ? 1 : 0);

    if (strcmp(key, "cab_name") == 0)
        return snprintf(buf, buf_len, "%s", inst->cab_name[0] ? inst->cab_name : "(none)");
    if (strcmp(key, "cab_count") == 0) return snprintf(buf, buf_len, "%d", inst->cab_count);
    if (strcmp(key, "cab_index") == 0) return snprintf(buf, buf_len, "%d", inst->current_cab_index);
    if (strcmp(key, "cab_length") == 0) {
        int i = (inst->cab_run_len <= 1024) ? 0 : (inst->cab_run_len <= 2048) ? 1
              : (inst->cab_run_len <= 4096) ? 2 : 3;
        return snprintf(buf, buf_len, "%d", i);
    }
    if (strcmp(key, "cab_bypass") == 0) return snprintf(buf, buf_len, "%d", inst->cab_bypass ? 1 : 0, inst->cab_run_len);

    if (strcmp(key, "cab_list") == 0) {
        scan_cabs(inst);
        int written = 0;
        written += snprintf(buf + written, buf_len - written, "[");
        for (int i = 0; i < inst->cab_count && written < buf_len - 40; i++) {
            if (i > 0) written += snprintf(buf + written, buf_len - written, ",");
            written += snprintf(buf + written, buf_len - written,
                "{\"label\":\"%s\",\"index\":%d}", inst->cab_names[i], i);
        }
        written += snprintf(buf + written, buf_len - written, "]");
        return written;
    }


    /* ui_hierarchy - returned dynamically (static shape, but kept alongside
     * the rest of the dynamic get_param handling for a single source of
     * truth with module.json). */
    /* Served HERE and not left to module.json, because the C side drops two
     * fields on the way through. chain_param_info_t carries key, name, type,
     * min, max, default, step, unit, display_format and options - and has no
     * member for `access` or `live`, so a parser that reads them from
     * module.json has nowhere to put them and the host's re-serialisation
     * cannot emit them. The CPU meter came back as an ordinary float knob
     * sitting at its default, which is exactly what it looked like on the
     * device.
     *
     * chain_host asks the PLUGIN first and returns its answer verbatim when
     * it is non-empty (chain_params_answer_is_useful), falling back to
     * module.json only otherwise. So a plugin that serves its own contract
     * gets every field through, and module.json stays as the fallback for a
     * host reading the file without loading us. Keep the two in step. */
    if (strcmp(key, "chain_params") == 0) {
        return snprintf(buf, buf_len,
            "["
              "{\"key\":\"input_level\",\"name\":\"Input\",\"type\":\"float\","
                "\"min\":0.0,\"max\":1.0,\"default\":0.5,\"step\":0.01},"
              "{\"key\":\"output_level\",\"name\":\"Output\",\"type\":\"float\","
                "\"min\":0.0,\"max\":1.0,\"default\":0.85,\"step\":0.01},"
              "{\"key\":\"quality\",\"name\":\"Quality\",\"type\":\"enum\","
                "\"options\":[\"Full\",\"Slim\",\"Lite\"],\"default\":0},"
              "{\"key\":\"cab_bypass\",\"name\":\"Cab Bypass\",\"type\":\"int\","
                "\"min\":0,\"max\":1,\"default\":0,\"step\":1},"
              "{\"key\":\"cab_length\",\"name\":\"Cab Len\",\"type\":\"enum\","
                "\"options\":[\"1024\",\"2048\",\"4096\",\"8192\"],\"default\":0}"
            "]");
    }

    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy = "{"
            "\"modes\":null,"
            "\"levels\":{"
                "\"root\":{"
                    "\"label\":\"Nam A2\","
                    "\"children\":null,"
                    "\"knobs\":[\"input_level\",\"output_level\",\"quality\",\"cab_length\"],"
                    "\"params\":["
                        "{\"key\":\"input_level\",\"label\":\"Input\"},"
                        "{\"key\":\"output_level\",\"label\":\"Output\"},"
                        "{\"key\":\"quality\",\"label\":\"Quality\"},"
                        "{\"key\":\"cab_bypass\",\"label\":\"Cab Bypass\"},"
                        "{\"key\":\"cab_length\",\"short_name\":\"CabLen\",\"label\":\"Cab Length\"},"
                        "{\"level\":\"models\",\"label\":\"Choose Model\"},"
                        "{\"level\":\"cabs\",\"label\":\"Choose Cabinet\"}"
                    "]"
                "},"
                "\"models\":{"
                    "\"label\":\"Model\","
                    "\"items_param\":\"model_list\","
                    "\"select_param\":\"model_index\","
                    "\"children\":null,\"knobs\":[],\"params\":[]"
                "},"
                "\"cabs\":{"
                    "\"label\":\"Cabinet\","
                    "\"items_param\":\"cab_list\","
                    "\"select_param\":\"cab_index\","
                    "\"children\":null,\"knobs\":[],\"params\":[]"
                "},"
            "}"
            "}"
        "}";
        return snprintf(buf, buf_len, "%s", hierarchy);
    }

    return -1;
}

/* ======================================================================== */
/* Entry point                                                               */
/* ======================================================================== */

static audio_fx_api_v2_t g_fx_api_v2;

extern "C" audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;

    memset(&g_fx_api_v2, 0, sizeof(g_fx_api_v2));
    g_fx_api_v2.api_version      = AUDIO_FX_API_VERSION_2;
    g_fx_api_v2.create_instance  = v2_create_instance;
    g_fx_api_v2.destroy_instance = v2_destroy_instance;
    g_fx_api_v2.process_block    = v2_process_block;
    g_fx_api_v2.set_param        = v2_set_param;
    g_fx_api_v2.get_param        = v2_get_param;
    g_fx_api_v2.on_midi          = nullptr; /* No MIDI handling needed */

    plugin_log("Nam A2: audio FX plugin initialized (NeuralAudio by Mike Oliphant)");

    return &g_fx_api_v2;
}
