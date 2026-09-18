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
 *     models too heavy to run in real time on Move's ARM core alongside the
 *     cab IR and EQ.
 *   - A 3-band EQ (low/high shelf + mid bell) as the amp's tone stack,
 *     between the model and the cab IR.
 *
 * A Doubler/Echo/Reverb "solo" FX chain was previously part of this module
 * and has been removed: those stages are better served by the host's own
 * chain FX slots, and their buffers cost ~900 KB of allocation inside
 * create_instance - which runs on the SPI audio callback, where allocation
 * is forbidden.
 *
 * Dependencies (all header-only / static, permissive licenses):
 *   NeuralAudio  - MIT      - Mike Oliphant
 *   Eigen        - MPL2     - Eigen contributors
 *   RTNeural     - BSD-3    - Jatin Chowdhury
 *   math_approx  - BSD-3    - Jatin Chowdhury
 *   nlohmann/json- MIT      - Niels Lohmann
 *
 * Audio: 44100 Hz, 128 frames/block, stereo interleaved int16 in-place.
 * Chain: input gain -> NAM model -> 3-band EQ -> cab IR -> output gain,
 * mirroring real hardware (preamp -> tone stack -> power amp -> speaker).
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
#define MAX_IR_LEN 8192

#define SAMPLE_RATE 44100.0f

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static const host_api_v1_t *g_host = nullptr;

/* ---- Load-path diagnostics (temporary) --------------------------------
 *
 * The host drops every log line unless /data/UserData/schwung/debug_log_on
 * exists, and on a device reachable only through Schwung Manager's web UI
 * (no SSH, no file browser) there is no way to create that file. So this
 * writes straight to debug.log, which the manager's /system/logs page
 * serves - and creates the flag file on the way past, so the host's own
 * logging comes up too.
 *
 * This is instrumentation for "the module will not load at all", not
 * something to keep: it does file I/O from entry points that run on the
 * SPI audio callback. Remove it once the load path is understood. */
static void diag(const char *fmt, ...) {
    FILE *f = fopen("/data/UserData/schwung/debug.log", "a");
    if (!f) return;
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    fprintf(f, "%02d:%02d:%02d [NAMA2] ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void diag_enable_host_log(void) {
    if (access("/data/UserData/schwung/debug_log_on", F_OK) == 0) return;
    FILE *f = fopen("/data/UserData/schwung/debug_log_on", "w");
    if (f) { fputc('1', f); fclose(f); }
}

static void plugin_log(const char *msg) {
    diag("%s", msg);
    if (g_host && g_host->log) g_host->log(msg);
}

static inline float clampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* ======================================================================== */
/* WAV reader - minimal parser for cab IR files                              */
/* ======================================================================== */

/* Read a mono float IR from a WAV file. Supports PCM16/24/32 and float32/64.
 * Returns number of samples read, or 0 on failure. */
static int load_wav_ir(const char *path, float *out, int max_samples) {
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

    char msg[MAX_PATH_LEN + 128];
    snprintf(msg, sizeof(msg), "Nam A2: loaded cab IR %s (%d samples, %d ch, %d bit, fmt %d)",
             path, read_count, num_channels, bits_per_sample, audio_format);
    plugin_log(msg);

    return read_count;
}

/* ======================================================================== */
/* 3-band EQ - RBJ Audio EQ Cookbook biquads (low/high shelf + mid bell)     */
/* ======================================================================== */

typedef struct {
    float b0, b1, b2, a1, a2;
    float z1, z2; /* Direct Form 2 Transposed state */
} biquad_t;

static inline void biquad_reset(biquad_t *bq) {
    bq->z1 = 0.0f;
    bq->z2 = 0.0f;
}

static inline float biquad_process(biquad_t *bq, float x) {
    float y = bq->b0 * x + bq->z1;
    bq->z1 = bq->b1 * x - bq->a1 * y + bq->z2;
    bq->z2 = bq->b2 * x - bq->a2 * y;
    return y;
}

static void biquad_set_low_shelf(biquad_t *bq, float freq, float gain_db, float sr) {
    freq = clampf(freq, 10.0f, sr * 0.45f);
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * freq / sr;
    float cosw0 = cosf(w0), sinw0 = sinf(w0);
    float alpha = sinw0 / 2.0f * sqrtf((A + 1.0f / A) * (1.0f - 1.0f) + 2.0f); /* S = 1 shelf slope */
    float two_sqrtA_alpha = 2.0f * sqrtf(A) * alpha;

    float b0 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 + two_sqrtA_alpha);
    float b1 = 2.0f * A * ((A - 1.0f) - (A + 1.0f) * cosw0);
    float b2 = A * ((A + 1.0f) - (A - 1.0f) * cosw0 - two_sqrtA_alpha);
    float a0 = (A + 1.0f) + (A - 1.0f) * cosw0 + two_sqrtA_alpha;
    float a1 = -2.0f * ((A - 1.0f) + (A + 1.0f) * cosw0);
    float a2 = (A + 1.0f) + (A - 1.0f) * cosw0 - two_sqrtA_alpha;

    bq->b0 = b0 / a0; bq->b1 = b1 / a0; bq->b2 = b2 / a0;
    bq->a1 = a1 / a0; bq->a2 = a2 / a0;
}

static void biquad_set_high_shelf(biquad_t *bq, float freq, float gain_db, float sr) {
    freq = clampf(freq, 10.0f, sr * 0.45f);
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * freq / sr;
    float cosw0 = cosf(w0), sinw0 = sinf(w0);
    float alpha = sinw0 / 2.0f * sqrtf((A + 1.0f / A) * (1.0f - 1.0f) + 2.0f);
    float two_sqrtA_alpha = 2.0f * sqrtf(A) * alpha;

    float b0 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 + two_sqrtA_alpha);
    float b1 = -2.0f * A * ((A - 1.0f) + (A + 1.0f) * cosw0);
    float b2 = A * ((A + 1.0f) + (A - 1.0f) * cosw0 - two_sqrtA_alpha);
    float a0 = (A + 1.0f) - (A - 1.0f) * cosw0 + two_sqrtA_alpha;
    float a1 = 2.0f * ((A - 1.0f) - (A + 1.0f) * cosw0);
    float a2 = (A + 1.0f) - (A - 1.0f) * cosw0 - two_sqrtA_alpha;

    bq->b0 = b0 / a0; bq->b1 = b1 / a0; bq->b2 = b2 / a0;
    bq->a1 = a1 / a0; bq->a2 = a2 / a0;
}

/* Fixed-Q peaking (bell) filter for the mid band. */
#define EQ_MID_Q 0.9f

static void biquad_set_peak(biquad_t *bq, float freq, float gain_db, float q, float sr) {
    freq = clampf(freq, 10.0f, sr * 0.45f);
    float A = powf(10.0f, gain_db / 40.0f);
    float w0 = 2.0f * (float)M_PI * freq / sr;
    float cosw0 = cosf(w0), sinw0 = sinf(w0);
    float alpha = sinw0 / (2.0f * q);

    float b0 = 1.0f + alpha * A;
    float b1 = -2.0f * cosw0;
    float b2 = 1.0f - alpha * A;
    float a0 = 1.0f + alpha / A;
    float a1 = -2.0f * cosw0;
    float a2 = 1.0f - alpha / A;

    bq->b0 = b0 / a0; bq->b1 = b1 / a0; bq->b2 = b2 / a0;
    bq->a1 = a1 / a0; bq->a2 = a2 / a0;
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

    /* Quality (0 = Full, 1 = Lite - runs the model at half rate) */
    int quality_lite;

    /* 3-band EQ (mono, post cab IR) */
    float eq_low_gain, eq_low_freq;
    float eq_mid_gain, eq_mid_freq;
    float eq_high_gain, eq_high_freq;
    biquad_t eq_low, eq_mid, eq_high;

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

    float *new_ir = (float *)calloc(MAX_IR_LEN, sizeof(float));
    if (!new_ir) return;

    int ir_len = load_wav_ir(inst->cab_paths[index], new_ir, MAX_IR_LEN);
    if (ir_len <= 0) {
        free(new_ir);
        char msg[MAX_PATH_LEN + 64];
        snprintf(msg, sizeof(msg), "Nam A2: failed to load cab IR %s", inst->cab_paths[index]);
        plugin_log(msg);
        return;
    }

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
    snprintf(msg, sizeof(msg), "Nam A2: loaded cab IR '%s' (%d samples)", inst->cab_name, ir_len);
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

/* --- EQ --- */

static void update_eq(nam_a2_instance_t *inst) {
    biquad_set_low_shelf(&inst->eq_low, inst->eq_low_freq, inst->eq_low_gain, SAMPLE_RATE);
    biquad_set_peak(&inst->eq_mid, inst->eq_mid_freq, inst->eq_mid_gain, EQ_MID_Q, SAMPLE_RATE);
    biquad_set_high_shelf(&inst->eq_high, inst->eq_high_freq, inst->eq_high_gain, SAMPLE_RATE);
}

static inline float eq_process(nam_a2_instance_t *inst, float x) {
    float y = biquad_process(&inst->eq_low, x);
    y = biquad_process(&inst->eq_mid, y);
    y = biquad_process(&inst->eq_high, y);
    return y;
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
    diag("create_instance entered, module_dir=%s", module_dir ? module_dir : "(null)");

    nam_a2_instance_t *inst = (nam_a2_instance_t *)calloc(1, sizeof(nam_a2_instance_t));
    if (!inst) { diag("create_instance FAILED: calloc of %zu bytes returned null",
                      sizeof(nam_a2_instance_t)); return nullptr; }
    diag("instance allocated (%zu bytes)", sizeof(nam_a2_instance_t));

    inst->loader = new NeuralAudio::NeuralModelLoader();
    diag("NeuralModelLoader constructed");
    inst->loader->SetDefaultMaxAudioBufferSize(FRAMES_PER_BLOCK);

    strncpy(inst->module_dir, module_dir, MAX_PATH_LEN - 1);
    inst->model = nullptr;
    inst->pending_model.store(nullptr);
    inst->loading.store(false);
    inst->current_model_index = -1;

    /* Cabinet IR defaults */
    inst->cab_ir = nullptr;
    inst->cab_ir_len = 0;
    inst->cab_history = nullptr;
    inst->cab_hist_pos = 0;
    inst->cab_bypass = false;
    inst->cab_name[0] = '\0';
    inst->current_cab_index = -1;

    /* Input/output/quality defaults */
    inst->input_level = 0.5f;
    inst->output_level = 0.5f;
    inst->input_gain = knob_to_gain(0.5f);
    inst->output_gain = knob_to_gain(0.5f);
    inst->quality_lite = 0;

    /* EQ defaults (flat) */
    inst->eq_low_gain = 0.0f;  inst->eq_low_freq = 100.0f;
    inst->eq_mid_gain = 0.0f;  inst->eq_mid_freq = 800.0f;
    inst->eq_high_gain = 0.0f; inst->eq_high_freq = 3000.0f;
    update_eq(inst);

    /* Scan for model/cab files and load the first of each */
    scan_models(inst);
    diag("scan_models done: %d models", inst->model_count);
    scan_cabs(inst);
    diag("scan_cabs done: %d cabs", inst->cab_count);

    if (inst->model_count > 0) {
        inst->current_model_index = 0;
        load_model_async(inst, inst->model_paths[0]);
        diag("model load thread started for %s", inst->model_paths[0]);
    } else {
        diag("no models found - plugin will pass audio through untouched");
    }
    if (inst->cab_count > 0) {
        load_cab(inst, 0);
    }

    diag("create_instance OK (returning %p)", (void *)inst);
    return inst;
}

/* --- destroy_instance --- */
static void v2_destroy_instance(void *instance) {
    nam_a2_instance_t *inst = (nam_a2_instance_t *)instance;
    if (!inst) return;
    diag("destroy_instance entered");

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

    {   /* first block only - proves audio actually reaches the plugin */
        static int first = 1;
        if (first) { first = 0; diag("first process_block (frames=%d)", frames); }
    }

    /* Check for newly loaded model (lock-free swap) */
    NeuralAudio::NeuralModel *pending = inst->pending_model.load(std::memory_order_acquire);
    if (pending) {
        NeuralAudio::NeuralModel *old = inst->model;
        inst->model = pending;
        inst->pending_model.store(nullptr, std::memory_order_release);
        if (old) delete old;
    }

    /* No model loaded - pass through untouched (matches schwung-nam) */
    if (!inst->model) return;

    int n = (frames > FRAMES_PER_BLOCK) ? FRAMES_PER_BLOCK : frames;

    /* Deinterleave stereo int16 -> mono float, with input gain */
    float ig = inst->input_gain;
    for (int i = 0; i < n; i++) {
        float l = audio_inout[i * 2]     / 32768.0f;
        float r = audio_inout[i * 2 + 1] / 32768.0f;
        inst->mono_in[i] = (l + r) * 0.5f * ig;
    }

    /* NAM model - Full runs every sample; Lite halves the neural net's work
     * by averaging input pairs, running the model at half rate, and holding
     * each output sample for two frames. */
    if (inst->quality_lite && n >= 2) {
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

    /* 3-band EQ (mono, in place) - this is the amp's tone stack, so it sits
     * BEFORE the cabinet, exactly as on real hardware: preamp -> tone stack
     * -> power amp -> speaker cab. Running it after the IR instead would let
     * the EQ boost back the top end the cabinet exists to roll off, which is
     * where the harshness a real cab removes lives. */
    for (int i = 0; i < n; i++) {
        inst->mono_out[i] = eq_process(inst, inst->mono_out[i]);
    }

    /* Cab IR convolution - the speaker is the last acoustic stage */
    if (!inst->cab_bypass && inst->cab_ir) {
        apply_cab_ir(inst, inst->mono_out, n);
    }

    /* Output gain, then back to stereo int16 (mono source written to both) */
    float og = inst->output_gain;
    for (int i = 0; i < n; i++) {
        float s = clampf(inst->mono_out[i] * og, -1.0f, 1.0f);
        int16_t sample = (int16_t)(s * 32767.0f);
        audio_inout[i * 2]     = sample;
        audio_inout[i * 2 + 1] = sample;
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
        if (json_get_int(val, "quality", &i) == 0) inst->quality_lite = (i != 0);

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
        if (json_get_int(val, "cab_bypass", &i) == 0) inst->cab_bypass = (i != 0);

        if (json_get_float(val, "eq_low_gain", &f) == 0) inst->eq_low_gain = clampf(f, -15.0f, 15.0f);
        if (json_get_float(val, "eq_low_freq", &f) == 0) inst->eq_low_freq = clampf(f, 40.0f, 500.0f);
        if (json_get_float(val, "eq_mid_gain", &f) == 0) inst->eq_mid_gain = clampf(f, -15.0f, 15.0f);
        if (json_get_float(val, "eq_mid_freq", &f) == 0) inst->eq_mid_freq = clampf(f, 200.0f, 4000.0f);
        if (json_get_float(val, "eq_high_gain", &f) == 0) inst->eq_high_gain = clampf(f, -15.0f, 15.0f);
        if (json_get_float(val, "eq_high_freq", &f) == 0) inst->eq_high_freq = clampf(f, 1000.0f, 10000.0f);
        update_eq(inst);

        return;
    }

    if (strcmp(key, "input_level") == 0) {
        inst->input_level = clampf(atof(val), 0.0f, 1.0f);
        inst->input_gain = knob_to_gain(inst->input_level);
    } else if (strcmp(key, "output_level") == 0) {
        inst->output_level = clampf(atof(val), 0.0f, 1.0f);
        inst->output_gain = knob_to_gain(inst->output_level);
    } else if (strcmp(key, "quality") == 0) {
        inst->quality_lite = (atoi(val) != 0);
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
    } else if (strcmp(key, "cab_bypass") == 0) {
        inst->cab_bypass = (atoi(val) != 0);
    } else if (strcmp(key, "eq_low_gain") == 0) {
        inst->eq_low_gain = clampf(atof(val), -15.0f, 15.0f); update_eq(inst);
    } else if (strcmp(key, "eq_low_freq") == 0) {
        inst->eq_low_freq = clampf(atof(val), 40.0f, 500.0f); update_eq(inst);
    } else if (strcmp(key, "eq_mid_gain") == 0) {
        inst->eq_mid_gain = clampf(atof(val), -15.0f, 15.0f); update_eq(inst);
    } else if (strcmp(key, "eq_mid_freq") == 0) {
        inst->eq_mid_freq = clampf(atof(val), 200.0f, 4000.0f); update_eq(inst);
    } else if (strcmp(key, "eq_high_gain") == 0) {
        inst->eq_high_gain = clampf(atof(val), -15.0f, 15.0f); update_eq(inst);
    } else if (strcmp(key, "eq_high_freq") == 0) {
        inst->eq_high_freq = clampf(atof(val), 1000.0f, 10000.0f); update_eq(inst);
    }
}

/* --- get_param --- */
static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    nam_a2_instance_t *inst = (nam_a2_instance_t *)instance;
    if (!inst || !key || !buf) return -1;

    /* Bulk serialization for slot autosave. */
    if (strcmp(key, "state") == 0) {
        return snprintf(buf, buf_len,
            "{\"input_level\":%.4f,\"output_level\":%.4f,\"quality\":%d,"
            "\"model_index\":%d,\"model_name\":\"%s\","
            "\"cab_index\":%d,\"cab_name\":\"%s\",\"cab_bypass\":%d,"
            "\"eq_low_gain\":%.2f,\"eq_low_freq\":%.1f,"
            "\"eq_mid_gain\":%.2f,\"eq_mid_freq\":%.1f,"
            "\"eq_high_gain\":%.2f,\"eq_high_freq\":%.1f}",
            inst->input_level, inst->output_level, inst->quality_lite,
            inst->current_model_index, inst->model_name,
            inst->current_cab_index, inst->cab_name, inst->cab_bypass ? 1 : 0,
            inst->eq_low_gain, inst->eq_low_freq,
            inst->eq_mid_gain, inst->eq_mid_freq,
            inst->eq_high_gain, inst->eq_high_freq);
    }

    if (strcmp(key, "input_level") == 0) return snprintf(buf, buf_len, "%.2f", inst->input_level);
    if (strcmp(key, "output_level") == 0) return snprintf(buf, buf_len, "%.2f", inst->output_level);
    if (strcmp(key, "quality") == 0) return snprintf(buf, buf_len, "%d", inst->quality_lite);

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
    if (strcmp(key, "cab_bypass") == 0) return snprintf(buf, buf_len, "%d", inst->cab_bypass ? 1 : 0);

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

    if (strcmp(key, "eq_low_gain") == 0) return snprintf(buf, buf_len, "%.2f", inst->eq_low_gain);
    if (strcmp(key, "eq_low_freq") == 0) return snprintf(buf, buf_len, "%.1f", inst->eq_low_freq);
    if (strcmp(key, "eq_mid_gain") == 0) return snprintf(buf, buf_len, "%.2f", inst->eq_mid_gain);
    if (strcmp(key, "eq_mid_freq") == 0) return snprintf(buf, buf_len, "%.1f", inst->eq_mid_freq);
    if (strcmp(key, "eq_high_gain") == 0) return snprintf(buf, buf_len, "%.2f", inst->eq_high_gain);
    if (strcmp(key, "eq_high_freq") == 0) return snprintf(buf, buf_len, "%.1f", inst->eq_high_freq);

    /* ui_hierarchy - returned dynamically (static shape, but kept alongside
     * the rest of the dynamic get_param handling for a single source of
     * truth with module.json). */
    if (strcmp(key, "ui_hierarchy") == 0) {
        const char *hierarchy = "{"
            "\"modes\":null,"
            "\"levels\":{"
                "\"root\":{"
                    "\"label\":\"Nam A2\","
                    "\"children\":null,"
                    "\"knobs\":[\"input_level\",\"output_level\",\"quality\"],"
                    "\"params\":["
                        "{\"key\":\"input_level\",\"label\":\"Input\"},"
                        "{\"key\":\"output_level\",\"label\":\"Output\"},"
                        "{\"key\":\"quality\",\"label\":\"Quality\"},"
                        "{\"key\":\"cab_bypass\",\"label\":\"Cab Bypass\"},"
                        "{\"level\":\"models\",\"label\":\"Choose Model\"},"
                        "{\"level\":\"eq\",\"label\":\"3-Band EQ\"},"
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
                "\"eq\":{"
                    "\"label\":\"3-Band EQ\","
                    "\"children\":null,"
                    "\"knobs\":[\"eq_low_gain\",\"eq_low_freq\",\"eq_mid_gain\","
                               "\"eq_mid_freq\",\"eq_high_gain\",\"eq_high_freq\"],"
                    "\"params\":[\"eq_low_gain\",\"eq_low_freq\",\"eq_mid_gain\","
                                "\"eq_mid_freq\",\"eq_high_gain\",\"eq_high_freq\"]"
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
    diag_enable_host_log();
    diag("move_audio_fx_init_v2 entered (host=%p)", (void *)host);
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
