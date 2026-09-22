/*
 * Nam A2c - eight freely-typed blocks, in the shape of a pedalboard.
 *
 * Nam A2 is one amp and one cabinet in a fixed order. This is eight blocks
 * in series, each of which can be any type: an amp, a cabinet, a drive, or
 * nothing. Pads 1-8 are the blocks.
 *
 *   In -> [1] -> [2] -> [3] -> [4] -> [5] -> [6] -> [7] -> [8] -> Out
 *
 * MONO THROUGHOUT. A guitar arrives on one channel (Move's own setting is
 * `monoFromLeftChannel` and a TRS with its ring tied to sleeve is silent on
 * the right), every block here is a mono device, and the result is written
 * to both outputs. Summing L+R instead would halve a mono source.
 *
 * THE BUDGET IS THE DESIGN CONSTRAINT. An SPI frame has ~2370 us of slack
 * and a full-quality NAM is ~1200 us of it, so eight blocks does not mean
 * eight amps - it means one amp and seven cheap things. `cpu` is a real
 * parameter on the main page for exactly that reason: the number has to be
 * in front of you while you choose, not in a log afterwards.
 *
 *   NAM Full ~1200us   NAM Slim ~250us   IR 1024 taps ~60us   Drive ~5us
 *
 * LOADS ARE OFF THE CALLBACK. Every module entry point here - create,
 * destroy, set_param, get_param, process_block - runs on the SPI audio
 * callback at SCHED_FIFO 70. A .nam parse or a 500 ms WAV read there is a
 * dropout, so both go through the worker below, and the worker is put back
 * to SCHED_OTHER explicitly: a thread created from an entry point INHERITS
 * FIFO 70, and Move's own `Link Main` runs at 35.
 */

#include <new>

#include "a2_common.h"
#include "a2_fx.h"

#define NAM_A2C_BUILD_ID "split"

#define NUM_BLOCKS 8
#define IR_RUN_TAPS 1024        /* 23 ms - a cabinet, not a room */

/* How many entries the model and cab pickers offer. The lists are served as
 * enum options inside chain_params, so this bounds that JSON rather than
 * what may sit on the card. */
#define MAX_LISTED 48

/* BLK_NAM is any .nam capture, which is NOT the same thing as an amp: the
 * bundled OCD is a pedal, captured at the same architecture (3ch/8ch
 * WaveNet, identical weight counts) and therefore at the same cost. Naming
 * the type "Amp" would put a fuzz pedal under a label that says otherwise
 * and hide that two of them do not fit in a frame. */
enum { BLK_OFF = 0, BLK_NAM = 1, BLK_CAB = 2, BLK_FX = 3, BLK_TYPES = 4 };

/* THE NAME COLUMN OF FX_PEDALS, not a second list.
 *
 * There used to be a `FX_NAMES[]` here beside the enum in a2_fx.h, which
 * is one fact in two files and exactly the shape that has already cost
 * this module four builds elsewhere. The pedal table carries the name, so
 * this reads it. */
static inline const char *fx_name(int id) {
    return (id >= 0 && id < FX_COUNT) ? FX_PEDALS[id].name : "?";
}

static const char *block_type_name(int t) {
    switch (t) {
        case BLK_NAM:   return "NAM";
        case BLK_CAB:   return "Cab";
        case BLK_FX:    return "FX";
        default:        return "Off";
    }
}

/* ======================================================================== */
/* Load requests                                                             */
/* ======================================================================== */

/* A 16-deep single-producer/single-consumer ring. The producer is
 * set_param on the audio callback, the consumer is the worker. A ring
 * rather than one slot because changing a block's type writes its model and
 * its cab in the same breath, and a single slot loses whichever lost the
 * race - silently, leaving a block that says "Amp" and makes no sound. */
#define REQ_RING 16

#define REQ_ALLOC_FX 100     /* not a load: "give the pedals their buffers" */

typedef struct {
    int  block;
    int  kind;      /* BLK_NAM, BLK_CAB, or REQ_ALLOC_FX */
    int  index;
    char path[MAX_PATH_LEN];
} load_req_t;

typedef struct {
    load_req_t items[REQ_RING];
    std::atomic<unsigned> head;   /* worker reads */
    std::atomic<unsigned> tail;   /* callback writes */
} req_ring_t;

static int req_push(req_ring_t *r, const load_req_t *req) {
    unsigned t = r->tail.load(std::memory_order_relaxed);
    unsigned h = r->head.load(std::memory_order_acquire);
    if (t - h >= REQ_RING) return 0;          /* full - drop, never block */
    r->items[t % REQ_RING] = *req;
    r->tail.store(t + 1, std::memory_order_release);
    return 1;
}

static int req_pop(req_ring_t *r, load_req_t *out) {
    unsigned h = r->head.load(std::memory_order_relaxed);
    unsigned t = r->tail.load(std::memory_order_acquire);
    if (h == t) return 0;
    *out = r->items[h % REQ_RING];
    r->head.store(h + 1, std::memory_order_release);
    return 1;
}

/* ======================================================================== */
/* Instance                                                                  */
/* ======================================================================== */

typedef struct {
    char module_dir[MAX_PATH_LEN];

    int  type[NUM_BLOCKS];
    int  on[NUM_BLOCKS];          /* 1 = in circuit, 0 = bypassed */

    nam_block_t   amp[NUM_BLOCKS];
    ir_block_t    cab[NUM_BLOCKS];
    fx_block_t    fx[NUM_BLOCKS];

    /* Per-block cost, peak-held. What makes the budget legible: the main
     * page shows the total, and a block's own page shows its share. */
    double us_peak[NUM_BLOCKS];

    int  sel_block;               /* which block the grid is editing */

    float in_level, out_level;
    /* THE ONE STEREO THING IN A MONO CHAIN.
     *
     * Every block processes `mono` in place; `side` is the left-minus-right
     * difference, zeroed every block and written only by the doubler, whose
     * whole character is the width (see fx_doubler). At the output the two
     * recombine as L = mono + side, R = mono - side, so a board with no
     * doubler on it is bit-identical to before and costs one memset. */
    float side[FRAMES_PER_BLOCK];
    float in_gain,  out_gain;

    /* Shared lists, scanned once. A model or a cab belongs to the module,
     * not to a block - eight blocks pointing at one file is ordinary. */
    int  model_count;
    char model_names[MAX_MODELS][MAX_NAME_LEN];
    char model_paths[MAX_MODELS][MAX_PATH_LEN];
    int  cab_count;
    char cab_names[MAX_CABS][MAX_NAME_LEN];
    char cab_paths[MAX_CABS][MAX_PATH_LEN];

    /* Scratch. Allocated once in create_instance - never per block. */
    float mono[FRAMES_PER_BLOCK];
    /*
     * TWO LANES, AND THE SPLIT NEVER REJOINS.
     *
     * Before `split_at` there is one signal in `mono`. At it, mono is
     * copied into both lanes and every block from there on belongs to one
     * of them. They meet again only at the output, where the pans put
     * them where they go.
     *
     * NOT REJOINING IS THE DESIGN, not a shortcut. A block that processed
     * both lanes would have to do it with ONE set of state - one delay
     * line, one filter memory, one envelope - so lane A's tail would come
     * out of lane B, and the bug would be subtle enough to read as a bad
     * sounding preset rather than as a wiring fault. Giving every block
     * two instances is 3.9 MB and doubles every cost. Both reference
     * pictures - Move's own split, TONE3000's two cabs - end in two
     * outputs, so the rule costs nothing anyone wanted.
     */
    float laneA[FRAMES_PER_BLOCK];
    float laneB[FRAMES_PER_BLOCK];
    int   split_at;                 /* 0 = off, else the 1-based block */
    int   lane[NUM_BLOCKS];         /* 0 = A/left, 1 = B/right */
    float pan_a, pan_b;             /* -1 .. +1 */
    float scratch[FRAMES_PER_BLOCK];

    req_ring_t   reqs;
    volatile int worker_stop;
    pthread_t    worker_tid;
    int          worker_running;

    double cpu_us_peak;
    int    cpu_warn;

    uint64_t blocks_seen;
    double   rate_t0_us;
    double   rate_hz;
    float    in_peak_l, in_peak_r;
    float    out_peak;
    /* THE SCREEN NEEDS ITS OWN PEAK HOLD, not a share of the log's.
     *
     * The diag thread resets out_peak every two seconds, so a second
     * consumer reading the same field would see whatever happened to
     * accumulate between the other one's resets - each getting part of the
     * signal, neither getting the maximum. A clip meter that misses half
     * the clips is worse than none, because it is believed. */
    float    ui_peak;
    /* A LATCH, IN SAMPLES, so one clipped block survives to be drawn.
     * A single block is 2.9 ms and the screen repaints at ~30 Hz; without
     * a hold, the clip that matters is the one nobody sees. */
    int      clip_hold;
    uint64_t nan_samples;

    volatile int diag_stop;
    pthread_t    diag_tid;
    int          diag_running;
} a2c_t;

/* ======================================================================== */
/* Threads                                                                   */
/* ======================================================================== */

/* A thread started from a module entry point inherits the SPI callback's
 * SCHED_FIFO 70. Nothing here needs to be realtime and two of Move's own
 * threads (`Link Main` at 35) would starve behind a model parse, so both of
 * ours are put back explicitly rather than left to inherit. */
static int start_low_prio_thread(pthread_t *tid, void *(*fn)(void *), void *arg,
                                 int detached) {
    pthread_attr_t at;
    struct sched_param sp;
    pthread_attr_init(&at);
    pthread_attr_setinheritsched(&at, PTHREAD_EXPLICIT_SCHED);
    pthread_attr_setschedpolicy(&at, SCHED_OTHER);
    sp.sched_priority = 0;
    pthread_attr_setschedparam(&at, &sp);
    if (detached) pthread_attr_setdetachstate(&at, PTHREAD_CREATE_DETACHED);
    int rc = pthread_create(tid, &at, fn, arg);
    pthread_attr_destroy(&at);
    return rc == 0;
}

static void *worker_thread(void *arg) {
    a2c_t *s = (a2c_t *)arg;
    char msg[MAX_PATH_LEN + 96];

    while (!s->worker_stop) {
        load_req_t r;
        if (!req_pop(&s->reqs, &r)) {
            struct timespec ts = { 0, 20000000L };   /* 20 ms */
            nanosleep(&ts, NULL);
            continue;
        }
        if (r.kind == REQ_ALLOC_FX) {
            /* A pedal's delay line and reverb buffers are ~114 KB, and
             * create_instance runs on the SPI callback where allocation is
             * forbidden. All eight are taken here, once, so changing a
             * block's pedal afterwards costs nothing and can happen on the
             * callback. fx_block_process checks for the buffer, so a block
             * whose allocation has not landed yet is silent rather than a
             * null dereference. */
            int okc = 0;
            for (int i = 0; i < NUM_BLOCKS; i++) okc += fx_block_alloc(&s->fx[i]);
            snprintf(msg, sizeof(msg), "Nam A2c: pedal buffers %d/%d", okc, NUM_BLOCKS);
            plugin_log(msg);
            continue;
        }
        if (r.block < 0 || r.block >= NUM_BLOCKS) continue;

        /* SUPERSEDED? Turning the Model knob queues one request per detent,
         * and the device log has a block reloading OCD, Recto, OCD, Recto
         * six times in two seconds - six full .nam parses, of which five are
         * thrown away, delaying the one that matters by a second. The
         * producer writes `index` at request time, so a request whose index
         * is no longer the block's current one has already been overtaken.
         * A plain int read; the ring stays single-producer. */
        if (r.kind == BLK_NAM && s->amp[r.block].index != r.index) continue;
        if (r.kind == BLK_CAB && s->cab[r.block].index != r.index) continue;

        if (r.kind == BLK_NAM) {
            nam_block_t *b = &s->amp[r.block];
            NeuralAudio::NeuralModelLoader loader;
            NeuralAudio::NeuralModel *m = loader.CreateFromFile(r.path);
            if (m) {
                /* Published BEFORE the model, and adopted with it by the
                 * same exchange, so a block can never run a model at
                 * another model's calibration. */
                /* LINEAR here, not dB: converting at publish time means the
                 * audio thread multiplies rather than calling powf twice a
                 * block, and there is only one place that can get the
                 * conversion wrong. */
                b->pending_in_gain  = db_to_gain(m->GetRecommendedInputDBAdjustment());
                b->pending_out_gain = db_to_gain(m->GetRecommendedOutputDBAdjustment());
                path_to_name(r.path, b->name, MAX_NAME_LEN);
                NeuralAudio::NeuralModel *stale =
                    b->pending.exchange(m, std::memory_order_acq_rel);
                delete stale;
                snprintf(msg, sizeof(msg), "Nam A2c: block %d amp '%s' loaded",
                         r.block + 1, b->name);
            } else {
                snprintf(msg, sizeof(msg), "Nam A2c: block %d amp FAILED %s",
                         r.block + 1, r.path);
            }
            b->loading.store(false, std::memory_order_release);
            plugin_log(msg);
        } else if (r.kind == BLK_CAB) {
            ir_block_t *b = &s->cab[r.block];
            b->run_cap = IR_RUN_TAPS;
            int n = ir_block_load(b, r.path);
            if (n > 0)
                snprintf(msg, sizeof(msg),
                         "Nam A2c: block %d cab '%s' %d taps (%.0f ms) gain %+.1f dB",
                         r.block + 1, b->name, n, 1000.0f * n / SAMPLE_RATE,
                         b->norm_db);
            else
                snprintf(msg, sizeof(msg), "Nam A2c: block %d cab FAILED %s",
                         r.block + 1, r.path);
            plugin_log(msg);
        }
    }
    return NULL;
}

static void *diag_thread(void *arg) {
    a2c_t *s = (a2c_t *)arg;
    int first = 1;
    while (!s->diag_stop) {
        struct timespec ts = { first ? 0 : 2, first ? 200000000L : 0 };
        first = 0;
        nanosleep(&ts, NULL);
        if (s->diag_stop) break;

        /* THE ROUTING IS IN THE LINE, because a split board whose log
         * looks exactly like a series one is a board nobody can diagnose
         * from a paste. `/` is where the lanes part; L and R say which
         * side each block after it is on. */
        char chain[220]; int w = 0;
        for (int i = 0; i < NUM_BLOCKS; i++) {
            const int split = (s->split_at > 0 && i >= s->split_at - 1);
            w += snprintf(chain + w, sizeof(chain) - w, "%s%s%s%s%.0f",
                          i ? " " : "",
                          (s->split_at > 0 && i == s->split_at - 1) ? "/" : "",
                          split ? (s->lane[i] ? "R" : "L") : "",
                          s->type[i] == BLK_OFF ? "-" :
                          (s->on[i] ? block_type_name(s->type[i]) : "("),
                          s->type[i] == BLK_OFF ? 0.0 : s->us_peak[i]);
        }

        char msg[512];
        snprintf(msg, sizeof(msg),
            "Nam A2c diag: blocks/s=%.0f (expect 344) | total_us=%.0f | %s | "
            "in pkL=%.4f pkR=%.4f | out pk=%.4f nan=%lu",
            s->rate_hz, s->cpu_us_peak, chain,
            s->in_peak_l, s->in_peak_r, s->out_peak,
            (unsigned long)s->nan_samples);
        plugin_log(msg);

        s->in_peak_l = s->in_peak_r = 0.0f;
        s->out_peak = 0.0f;
    }
    return NULL;
}

/* ======================================================================== */
/* Lists                                                                     */
/* ======================================================================== */

static void scan_lists(a2c_t *s) {
    char dir[MAX_PATH_LEN];
    snprintf(dir, sizeof(dir), "%s/models", s->module_dir);
    s->model_count = scan_directory(dir, s->model_names, s->model_paths,
                                    MAX_MODELS, is_model_file);
    snprintf(dir, sizeof(dir), "%s/cabs", s->module_dir);
    s->cab_count = scan_directory(dir, s->cab_names, s->cab_paths,
                                  MAX_CABS, is_cab_file);
}

/* ======================================================================== */
/* API v2                                                                    */
/* ======================================================================== */

typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *config_json);
    void  (*destroy_instance)(void *instance);
    void  (*process_block)(void *instance, int16_t *audio_inout, int frames);
    void  (*set_param)(void *instance, const char *key, const char *val);
    int   (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void  (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
} audio_fx_api_v2_t;

static void request_load(a2c_t *s, int block, int kind, int index) {
    load_req_t r;
    r.block = block;
    r.kind = kind;
    r.index = index;
    if (kind == BLK_NAM) {
        if (index < 0 || index >= s->model_count) return;
        snprintf(r.path, sizeof(r.path), "%s", s->model_paths[index]);
        s->amp[block].index = index;
        s->amp[block].loading.store(true, std::memory_order_release);
    } else {
        if (index < 0 || index >= s->cab_count) return;
        snprintf(r.path, sizeof(r.path), "%s", s->cab_paths[index]);
        s->cab[block].index = index;
    }
    req_push(&s->reqs, &r);
}

/* WHAT IS ACTUALLY ON THE CARD, reported at load.
 *
 * The pads need ui_chain.js; ui_chain.js is only loaded when the module
 * declares NO ui_hierarchy (enterComponentEdit returns early when it finds
 * one); and the host reads that declaration out of module.json ON DISK,
 * with a plain strstr for the literal "ui_hierarchy"
 * (chain_params.c:824). So three facts decide whether the pads exist, all
 * three live in the install rather than in this binary, and none of them
 * says anything when it is wrong - the grid simply comes up and the pads
 * are silently not a feature.
 *
 * Two rounds of this hunt were spent on a device whose files did not match
 * the tarball that was being discussed. So the module reads its own
 * install and says what it found. */
static void report_install(a2c_t *s) {
    char path[MAX_PATH_LEN];
    char msg[MAX_PATH_LEN + 192];

    snprintf(path, sizeof(path), "%s/module.json", s->module_dir);
    FILE *f = fopen(path, "r");
    if (!f) {
        snprintf(msg, sizeof(msg), "Nam A2c INSTALL: no module.json at %s", path);
        plugin_log(msg);
        return;
    }
    static char buf[65536];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    buf[n] = '\0';
    fclose(f);

    int has_hier = strstr(buf, "\"ui_hierarchy\"") != NULL;

    char ver[32] = "?";
    const char *v = strstr(buf, "\"version\"");
    if (v) {
        v = strchr(v + 9, '"');
        if (v) {
            v++;
            int i = 0;
            while (*v && *v != '"' && i < (int)sizeof(ver) - 1) ver[i++] = *v++;
            ver[i] = '\0';
        }
    }

    snprintf(path, sizeof(path), "%s/ui_chain.js", s->module_dir);
    int has_ui = access(path, R_OK) == 0;

    snprintf(msg, sizeof(msg),
             "Nam A2c INSTALL: module.json v%s | ui_hierarchy %s | ui_chain.js %s | PADS %s",
             ver,
             has_hier ? "PRESENT" : "absent",
             has_ui ? "present" : "MISSING",
             (!has_hier && has_ui) ? "OK"
                 : has_hier ? "DEAD - stale module.json, the host grid wins"
                 : "DEAD - ui_chain.js did not install");
    plugin_log(msg);
}

static void *v2_create_instance(const char *module_dir, const char *config_json) {
    (void)config_json;
    a2c_t *s = (a2c_t *)calloc(1, sizeof(a2c_t));
    if (!s) return NULL;

    plugin_log("Nam A2c BUILD " NAM_A2C_BUILD_ID);

    snprintf(s->module_dir, sizeof(s->module_dir), "%s",
             module_dir ? module_dir : ".");

    for (int i = 0; i < NUM_BLOCKS; i++) {
        s->type[i] = BLK_OFF;
        s->on[i] = 1;
        s->amp[i].index = -1;
        s->amp[i].quality = 0;
        new (&s->amp[i].pending) std::atomic<NeuralAudio::NeuralModel *>(nullptr);
        new (&s->amp[i].loading) std::atomic<bool>(false);
        s->cab[i].index = -1;
        s->cab[i].run_cap = IR_RUN_TAPS;
        s->fx[i].id = FX_TS808;
        for (int k = 0; k < FX_PARAMS; k++) s->fx[i].p[k] = 0.5f;
    }
    new (&s->reqs.head) std::atomic<unsigned>(0);
    new (&s->reqs.tail) std::atomic<unsigned>(0);

    s->in_level = 0.5f;
    s->out_level = 0.85f;
    s->in_gain = knob_to_gain(s->in_level);
    s->out_gain = knob_to_gain(s->out_level);
    s->sel_block = 0;
    s->split_at = 0;
    /* ALTERNATING, so switching the split on is immediately a working
     * split rather than a silent right channel. The commonest shape by a
     * long way is amp, split, two cabs. */
    for (int i = 0; i < NUM_BLOCKS; i++) s->lane[i] = i & 1;
    s->pan_a = -1.0f;
    s->pan_b = 1.0f;

    report_install(s);
    scan_lists(s);

    s->worker_stop = 0;
    s->worker_running = start_low_prio_thread(&s->worker_tid, worker_thread, s, 0);
    if (!s->worker_running) plugin_log("Nam A2c: worker FAILED to start");
    { load_req_t r; memset(&r, 0, sizeof(r)); r.kind = REQ_ALLOC_FX; req_push(&s->reqs, &r); }

    /* NOTHING IS LOADED. An empty board is what "eight blocks you fill"
     * means, and choosing the first two files on the card for somebody is a
     * guess that also costs 1500 us of the frame before they have asked for
     * anything. The pickers are one knob away.
     *
     * It used to build OCD -> Recto -> Cab so a fresh install made a sound
     * rather than reading as broken; the pedalboard picture answers that
     * now - eight empty boxes are visibly eight empty boxes. */

    s->diag_stop = 0;
    s->diag_running = start_low_prio_thread(&s->diag_tid, diag_thread, s, 0);

    return s;
}

static void v2_destroy_instance(void *instance) {
    a2c_t *s = (a2c_t *)instance;
    if (!s) return;

    s->diag_stop = 1;
    if (s->diag_running) pthread_join(s->diag_tid, NULL);
    s->worker_stop = 1;
    if (s->worker_running) pthread_join(s->worker_tid, NULL);

    for (int i = 0; i < NUM_BLOCKS; i++) {
        delete s->amp[i].model;
        delete s->amp[i].pending.exchange(nullptr, std::memory_order_acq_rel);
        ir_block_free(&s->cab[i]);
        fx_block_free(&s->fx[i]);
    }
    free(s);
    plugin_log("Nam A2c: instance destroyed");
}

static void v2_process_block(void *instance, int16_t *audio_inout, int frames) {
    a2c_t *s = (a2c_t *)instance;
    if (!s || !audio_inout) return;

    struct timespec t0;
    clock_gettime(CLOCK_MONOTONIC, &t0);

    int n = (frames > FRAMES_PER_BLOCK) ? FRAMES_PER_BLOCK : frames;

    /* Call-rate meter. If this module is called 344 times a second and the
     * output still has whole blocks of silence in it, the silence is
     * inserted after us; if it is called appreciably fewer, the host is
     * skipping our slot. Nothing else tells those two apart. */
    double now_us = t0.tv_sec * 1e6 + t0.tv_nsec / 1e3;
    if (s->rate_t0_us == 0.0) s->rate_t0_us = now_us;
    s->blocks_seen++;
    if (now_us - s->rate_t0_us >= 1e6) {
        s->rate_hz = s->blocks_seen * 1e6 / (now_us - s->rate_t0_us);
        s->blocks_seen = 0;
        s->rate_t0_us = now_us;
    }

    const float ig = s->in_gain;
    for (int i = 0; i < n; i++) {
        float l = audio_inout[i * 2]     / 32768.0f;
        float r = audio_inout[i * 2 + 1] / 32768.0f;
        float al = l < 0 ? -l : l, ar = r < 0 ? -r : r;
        if (al > s->in_peak_l) s->in_peak_l = al;
        if (ar > s->in_peak_r) s->in_peak_r = ar;
        s->mono[i] = l * ig;
        s->side[i] = 0.0f;
    }

    /* Tempo for the delays' Note mode, read ONCE per block rather than per
     * pedal: get_bpm walks a fallback chain (MIDI clock -> set tempo ->
     * settings -> 120) and eight blocks asking it eight times a block is
     * eight times the work for one answer. A host that does not offer the
     * callback at all leaves this 0 and fx_time_ms falls back itself. */
    const float bpm = (g_host && g_host->get_bpm) ? g_host->get_bpm() : 0.0f;

    /* A SPLIT PAST THE LAST BLOCK IS NO SPLIT. Clamped here rather than
     * at the setter so a board saved with eight blocks and a split at 8
     * cannot leave a lane that nothing ever writes. */
    const int split_at = (s->split_at > 0 && s->split_at <= NUM_BLOCKS) ? s->split_at : 0;
    int split = 0;

    for (int b = 0; b < NUM_BLOCKS; b++) {
        if (split_at && b == split_at - 1) {
            memcpy(s->laneA, s->mono, sizeof(float) * (size_t)n);
            memcpy(s->laneB, s->mono, sizeof(float) * (size_t)n);
            split = 1;
        }
        float *buf = split ? (s->lane[b] ? s->laneB : s->laneA) : s->mono;

        int t = s->type[b];
        if (t == BLK_OFF) { s->us_peak[b] = 0.0; continue; }

        /* Adopt a finished load even while bypassed: coming off bypass
         * should not then wait for a model that has been ready for a
         * minute. */
        if (t == BLK_NAM) nam_block_adopt_pending(&s->amp[b]);

        if (!s->on[b]) { s->us_peak[b] *= CPU_PEAK_DECAY; continue; }

        struct timespec b0, b1;
        clock_gettime(CLOCK_MONOTONIC, &b0);
        switch (t) {
            case BLK_NAM:
                if (s->amp[b].model) {
                    const float mg = s->amp[b].in_gain;
                    for (int i = 0; i < n; i++) buf[i] *= mg;
                    nam_block_process(&s->amp[b], buf, s->scratch, n);
                    const float mo = s->amp[b].out_gain;
                    for (int i = 0; i < n; i++) buf[i] *= mo;
                }
                break;
            case BLK_CAB:
                ir_block_process(&s->cab[b], buf, n);
                break;
            case BLK_FX:
                if (s->fx[b].line && s->fx[b].rv) {
                    s->fx[b].bpm = bpm;
                    fx_block_process(&s->fx[b], buf, s->side, n);
                }
                break;
            default: break;
        }
        clock_gettime(CLOCK_MONOTONIC, &b1);
        double us = (b1.tv_sec - b0.tv_sec) * 1e6 + (b1.tv_nsec - b0.tv_nsec) / 1e3;
        double decayed = s->us_peak[b] * CPU_PEAK_DECAY;
        s->us_peak[b] = (us > decayed) ? us : decayed;
    }

    const float og = s->out_gain;
    /* EQUAL POWER, so sweeping a lane across the field does not make it
     * louder in the middle. Computed once per block; the pans are knobs,
     * not audio. */
    const float aa = (s->pan_a * 0.5f + 0.5f) * 1.5707963f;
    const float bb = (s->pan_b * 0.5f + 0.5f) * 1.5707963f;
    const float a_l = cosf(aa), a_r = sinf(aa);
    const float b_l = cosf(bb), b_r = sinf(bb);

    for (int i = 0; i < n; i++) {
        float la, ra;
        if (split) {
            /* Two independent signals, each placed by its own pan. */
            float A = s->laneA[i] * og, B = s->laneB[i] * og;
            la = A * a_l + B * b_l;
            ra = A * a_r + B * b_r;
        } else {
            float m0 = s->mono[i] * og;
            la = ra = m0;
        }
        float m = sanitize_sample((la + ra) * 0.5f);
        if (m != m) s->nan_samples++;
        float sd = sanitize_sample(s->side[i] * og + (la - ra) * 0.5f);
        float lv = m + sd, rv = m - sd;
        /* MEASURED ON THE WIDER CHANNEL, AND BEFORE THE QUANTISER CLAMPS
         * IT. Taking it afterwards would report 0.999 for a signal that
         * was 2.0 - the clamp is exactly what hides the thing this is
         * looking for. And the wider channel, not the mono sum, because
         * the doubler puts real level into the side: a mid of 0.8 with a
         * side of 0.4 is 1.2 on the left and only the left is clipping. */
        float a = fabsf(lv) > fabsf(rv) ? fabsf(lv) : fabsf(rv);
        if (a > s->out_peak) s->out_peak = a;
        if (a > s->ui_peak) s->ui_peak = a;
        if (a >= 1.0f) s->clip_hold = (int)(SAMPLE_RATE / 2);   /* 500 ms */
        else if (s->clip_hold > 0) s->clip_hold--;
        int32_t ql = (int32_t)lrintf(lv * 32767.0f);
        int32_t qr = (int32_t)lrintf(rv * 32767.0f);
        if (ql > 32767) ql = 32767;
        if (ql < -32768) ql = -32768;
        if (qr > 32767) qr = 32767;
        if (qr < -32768) qr = -32768;
        audio_inout[i * 2]     = (int16_t)ql;
        audio_inout[i * 2 + 1] = (int16_t)qr;
    }

    struct timespec t1;
    clock_gettime(CLOCK_MONOTONIC, &t1);
    double total = (t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3;
    double dec = s->cpu_us_peak * CPU_PEAK_DECAY;
    s->cpu_us_peak = (total > dec) ? total : dec;
    double pct = 100.0 * s->cpu_us_peak / FRAME_BUDGET_US;
    if (pct > CPU_WARN_ON)       s->cpu_warn = 1;
    else if (pct < CPU_WARN_OFF) s->cpu_warn = 0;
}

static void v2_on_midi(void *instance, const uint8_t *msg, int len, int source) {
    (void)instance; (void)msg; (void)len; (void)source;
}

/* ------------------------------------------------------------------ params */

/* `b<N>_<key>`, the shape `child_key_template` in the hierarchy resolves to.
 * Returns the block index, or -1 when the key is not a block key. */
static int block_key(const char *key, const char **rest) {
    if (key[0] != 'b') return -1;
    if (key[1] < '1' || key[1] > '0' + NUM_BLOCKS) return -1;
    if (key[2] != '_') return -1;
    *rest = key + 3;
    return key[1] - '1';
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    a2c_t *s = (a2c_t *)instance;
    if (!s || !key || !val) return;

    const char *sub;
    int b = block_key(key, &sub);
    if (b >= 0) {
        if (strcmp(sub, "type") == 0) {
            int t = atoi(val);
            if (t < 0) t = 0;
            if (t >= BLK_TYPES) t = BLK_TYPES - 1;
            if (t == s->type[b]) return;
            s->type[b] = t;
            /* A block given a type with nothing chosen yet takes the first
             * thing on the card, so selecting "Amp" makes a sound rather
             * than a silence you then have to diagnose. */
            if (t == BLK_NAM && s->amp[b].index < 0 && s->model_count > 0)
                request_load(s, b, BLK_NAM, 0);
            if (t == BLK_CAB && s->cab[b].index < 0 && s->cab_count > 0)
                request_load(s, b, BLK_CAB, 0);
            if (t == BLK_FX) fx_block_reset(&s->fx[b]);
        } else if (strcmp(sub, "on") == 0) {
            /* Enum: 0 = On, 1 = Bypass. Reads the way the pad does. */
            s->on[b] = (atoi(val) == 0) ? 1 : 0;
        } else if (strcmp(sub, "model") == 0) {
            int i = atoi(val);
            if (i != s->amp[b].index) request_load(s, b, BLK_NAM, i);
        } else if (strcmp(sub, "quality") == 0) {
            int q = atoi(val);
            s->amp[b].quality = (q < 0) ? 0 : (q > 2) ? 2 : q;
            nam_block_apply_quality(&s->amp[b]);
        } else if (strcmp(sub, "cab") == 0) {
            int i = atoi(val);
            if (i != s->cab[b].index) request_load(s, b, BLK_CAB, i);
        } else if (strcmp(sub, "fx") == 0) {
            int m = atoi(val);
            if (m < 0) m = 0;
            if (m >= FX_COUNT) m = FX_COUNT - 1;
            if (m != s->fx[b].id) {
                s->fx[b].id = m;
                /* Or the previous pedal's tail rings on through the new
                 * one's filters. */
                fx_block_reset(&s->fx[b]);
            }
        } else if (strcmp(sub, "lane") == 0) {
            s->lane[b] = (atoi(val) != 0) ? 1 : 0;
        } else if (sub[0] == 'p' && sub[1] >= '1' && sub[1] <= '5' && sub[2] == 0) {
            s->fx[b].p[sub[1] - '1'] = clampf(atof(val), 0.0f, 1.0f);
        }
        return;
    }

    if (strcmp(key, "in_level") == 0) {
        s->in_level = clampf(atof(val), 0.0f, 1.0f);
        s->in_gain = knob_to_gain(s->in_level);
    } else if (strcmp(key, "out_level") == 0) {
        s->out_level = clampf(atof(val), 0.0f, 1.0f);
        s->out_gain = knob_to_gain(s->out_level);
    } else if (strcmp(key, "split") == 0) {
        int v = atoi(val);
        s->split_at = (v < 0) ? 0 : (v > NUM_BLOCKS) ? NUM_BLOCKS : v;
    } else if (strcmp(key, "pan_a") == 0) {
        s->pan_a = clampf(atof(val), -1.0f, 1.0f);
    } else if (strcmp(key, "pan_b") == 0) {
        s->pan_b = clampf(atof(val), -1.0f, 1.0f);
    } else if (strcmp(key, "sel_block") == 0) {
        int i = atoi(val);
        s->sel_block = (i < 0) ? 0 : (i >= NUM_BLOCKS) ? NUM_BLOCKS - 1 : i;
    } else if (strcmp(key, "rescan") == 0) {
        scan_lists(s);
    }
}

/* The option list for a picker, as chain_params enum options.
 *
 * Emitted from the SCANNED list rather than declared, so a model dropped on
 * the card by the file browser appears without a rebuild. Quotes and
 * backslashes in a filename are dropped rather than escaped: both readers of
 * this JSON truncate a quoted value at its first quote, so an escaped name
 * would be worse than a shortened one. */
static int emit_options(char *buf, int buf_len,
                        char names[][MAX_NAME_LEN], int count) {
    int w = 0;
    w += snprintf(buf + w, buf_len - w, "[");
    int shown = (count < MAX_LISTED) ? count : MAX_LISTED;
    if (shown == 0) {
        w += snprintf(buf + w, buf_len - w, "\"(none)\"");
    }
    for (int i = 0; i < shown && w < buf_len - 96; i++) {
        if (i) w += snprintf(buf + w, buf_len - w, ",");
        w += snprintf(buf + w, buf_len - w, "\"");
        for (const char *p = names[i]; *p && w < buf_len - 8; p++)
            if (*p != '"' && *p != '\\') buf[w++] = *p;
        buf[w] = '\0';
        w += snprintf(buf + w, buf_len - w, "\"");
    }
    w += snprintf(buf + w, buf_len - w, "]");
    return w;
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    a2c_t *s = (a2c_t *)instance;
    if (!s || !key || !buf) return -1;

    const char *sub;
    int b = block_key(key, &sub);
    if (b >= 0) {
        if (strcmp(sub, "type") == 0)    return snprintf(buf, buf_len, "%d", s->type[b]);
        if (strcmp(sub, "on") == 0)      return snprintf(buf, buf_len, "%d", s->on[b] ? 0 : 1);
        if (strcmp(sub, "model") == 0)   return snprintf(buf, buf_len, "%d", s->amp[b].index < 0 ? 0 : s->amp[b].index);
        if (strcmp(sub, "quality") == 0) return snprintf(buf, buf_len, "%d", s->amp[b].quality);
        if (strcmp(sub, "cab") == 0)     return snprintf(buf, buf_len, "%d", s->cab[b].index < 0 ? 0 : s->cab[b].index);
        if (strcmp(sub, "fx") == 0)      return snprintf(buf, buf_len, "%d", s->fx[b].id);
        if (sub[0] == 'p' && sub[1] >= '1' && sub[1] <= '5' && sub[2] == 0)
            return snprintf(buf, buf_len, "%.4f", s->fx[b].p[sub[1] - '1']);
        if (strcmp(sub, "lane") == 0)    return snprintf(buf, buf_len, "%d", s->lane[b]);
        if (strcmp(sub, "cpu") == 0) {
            double pct = 100.0 * s->us_peak[b] / FRAME_BUDGET_US;
            return snprintf(buf, buf_len, "%d", (int)(clampf(pct, 0.0f, 100.0f) + 0.5f));
        }
        return -1;
    }

    if (strcmp(key, "in_level") == 0)  return snprintf(buf, buf_len, "%.4f", s->in_level);
    if (strcmp(key, "out_level") == 0) return snprintf(buf, buf_len, "%.4f", s->out_level);
    if (strcmp(key, "split") == 0)  return snprintf(buf, buf_len, "%d", s->split_at);
    if (strcmp(key, "pan_a") == 0)  return snprintf(buf, buf_len, "%.4f", s->pan_a);
    if (strcmp(key, "pan_b") == 0)  return snprintf(buf, buf_len, "%.4f", s->pan_b);
    if (strcmp(key, "sel_block") == 0) return snprintf(buf, buf_len, "%d", s->sel_block);
    /* OUTPUT LEVEL, AS A PERCENTAGE OF FULL SCALE, PEAK-HELD.
     *
     * Reading it CONSUMES the hold, which is what makes a meter read once
     * every few frames honest: whatever the loudest sample was since the
     * last look is what comes back, so nothing between reads is missed.
     * Over 100 means the quantiser clamped - the board is clipping and the
     * Out knob is the fix. Capped at 400 so a runaway reads as pinned
     * rather than as a number nobody can use. */
    if (strcmp(key, "peak") == 0) {
        int pct = (int)(s->ui_peak * 100.0f + 0.5f);
        if (pct > 400) pct = 400;
        s->ui_peak = 0.0f;
        return snprintf(buf, buf_len, "%d", (s->clip_hold > 0 && pct < 100) ? 100 : pct);
    }
    /* ON THE SCREEN, not only in the log.
     *
     * Three rounds of this were spent establishing which build was running,
     * twice from a log that did not include the load line. The panel is
     * always in front of you and the log is not. */
    if (strcmp(key, "build") == 0) return snprintf(buf, buf_len, "%s", NAM_A2C_BUILD_ID);
    if (strcmp(key, "cpu") == 0) {
        double pct = 100.0 * s->cpu_us_peak / FRAME_BUDGET_US;
        return snprintf(buf, buf_len, "%d", (int)(clampf(pct, 0.0f, 100.0f) + 0.5f));
    }

    /* Never -1 for these two: an unserved key answers null, which means THE
     * READ DID NOT COMPLETE, is never cached, and costs ~50 failed reads a
     * second on a key the chain line polls twice a second. */
    if (strcmp(key, "name") == 0 || strcmp(key, "preset_name") == 0) {
        int amp = -1;
        for (int i = 0; i < NUM_BLOCKS; i++)
            if (s->type[i] == BLK_NAM && s->amp[i].name[0]) { amp = i; break; }
        return snprintf(buf, buf_len, "%s%s",
                        amp >= 0 ? s->amp[amp].name : "A2c",
                        s->cpu_warn ? " !" : "");
    }
    if (strcmp(key, "display_name") == 0) {
        /* EMPTY, NEVER -1. -1 is the wire's "the read did not complete",
         * which the host retries and then reports as a give-up: the device
         * log carried `param_giveup ... last_key=fx1:display_name` every
         * eight seconds for as long as this answered -1, because the chain
         * line polls this key and nothing was ever going to answer it. An
         * empty string is the honest "served, and there is nothing here". */
        if (!s->cpu_warn) { if (buf_len > 0) buf[0] = 0; return 0; }
        return snprintf(buf, buf_len, "Nam A2c CPU overload");
    }
    if (strcmp(key, "is_loading") == 0 || strcmp(key, "loading") == 0) {
        int any = 0;
        for (int i = 0; i < NUM_BLOCKS; i++)
            if (s->amp[i].loading.load(std::memory_order_acquire)) any = 1;
        return snprintf(buf, buf_len, "%d", any);
    }

    if (strcmp(key, "state") == 0) {
        int w = 0;
        w += snprintf(buf + w, buf_len - w,
                      "{\"in_level\":%.4f,\"out_level\":%.4f,\"split\":%d,"
                      "\"pan_a\":%.4f,\"pan_b\":%.4f,\"blocks\":[",
                      s->in_level, s->out_level, s->split_at, s->pan_a, s->pan_b);
        for (int i = 0; i < NUM_BLOCKS && w < buf_len - 256; i++) {
            w += snprintf(buf + w, buf_len - w,
                "%s{\"type\":%d,\"on\":%d,\"lane\":%d,\"model\":%d,\"quality\":%d,\"cab\":%d,"
                "\"fx\":%d,\"p\":[%.4f,%.4f,%.4f,%.4f,%.4f]}",
                i ? "," : "", s->type[i], s->on[i], s->lane[i],
                s->amp[i].index, s->amp[i].quality, s->cab[i].index,
                s->fx[i].id,
                s->fx[i].p[0], s->fx[i].p[1], s->fx[i].p[2],
                s->fx[i].p[3], s->fx[i].p[4]);
        }
        w += snprintf(buf + w, buf_len - w, "]}");
        return w;
    }

    /* chain_params is BUILT rather than written out, because eight blocks of
     * nine keys is 72 declarations and a literal that long is a literal that
     * drifts. Served here and not left to module.json for a second reason:
     * the C side's chain_param_info_t has no member for `access` or `live`,
     * so the CPU readout only survives when the plugin's own string is
     * returned verbatim - and it only IS returned verbatim while it parses,
     * which a trailing comma once quietly prevented for four builds. */
    /* The picker lists, as plain JSON arrays for ui_chain.js.
     *
     * With no hierarchy there is no host grid to render an enum's options,
     * so the module's own screen has to name the loaded model rather than
     * show an index - "2" tells you nothing about which amp is in block 2.
     * The same names go into chain_params as enum options; both are built
     * from the one scan so they cannot disagree. */
    if (strcmp(key, "model_list") == 0)
        return emit_options(buf, buf_len, s->model_names, s->model_count);
    if (strcmp(key, "cab_list") == 0)
        return emit_options(buf, buf_len, s->cab_names, s->cab_count);
    /* THE KNOB TABLE, SERVED ONCE.
     *
     * The screen has to print "480 ms" and "-6.0 dB" rather than "37", and
     * the only honest way to do that is for the number it formats and the
     * number the DSP reads to come from the same row. So the UI reads this
     * at load, exactly as it reads the model and cab lists, and formats
     * locally - no per-cell IPC, and no second copy of any range in
     * JavaScript.
     *
     * Index is the wire value throughout: a knob the pedal does not have
     * is `null` at its own position, never a compaction, because `p3` is
     * p3 whether or not p2 exists. */
    if (strcmp(key, "fx_specs") == 0) {
        int w = 0;
        w += snprintf(buf + w, buf_len - w, "[");
        for (int i = 0; i < FX_COUNT && w < buf_len - 512; i++) {
            const fx_pedal_t *pd = &FX_PEDALS[i];
            w += snprintf(buf + w, buf_len - w, "%s{\"n\":\"%s\",\"aw\":%d,\"k\":[",
                          i ? "," : "", pd->name, pd->alt_when);
            for (int k = 0; k < FX_PARAMS; k++) {
                if (k) w += snprintf(buf + w, buf_len - w, ",");
                const fx_knob_t *kn = &pd->knob[k];
                if (!kn->name) { w += snprintf(buf + w, buf_len - w, "null"); continue; }
                w += snprintf(buf + w, buf_len - w,
                    "{\"n\":\"%s\",\"u\":\"%s\",\"lo\":%g,\"hi\":%g,\"c\":%d",
                    kn->name, kn->unit, (double)kn->lo, (double)kn->hi, (int)kn->curve);
                if (kn->opts)
                    w += snprintf(buf + w, buf_len - w, ",\"o\":\"%s\"", kn->opts);
                if (kn->alt) {
                    const fx_knob_t *al = kn->alt;
                    w += snprintf(buf + w, buf_len - w,
                        ",\"a\":{\"n\":\"%s\",\"u\":\"%s\",\"lo\":%g,\"hi\":%g,"
                        "\"c\":%d,\"o\":\"%s\"}",
                        al->name, al->unit, (double)al->lo, (double)al->hi,
                        (int)al->curve, al->opts ? al->opts : "");
                }
                w += snprintf(buf + w, buf_len - w, "}");
            }
            w += snprintf(buf + w, buf_len - w, "]}");
        }
        w += snprintf(buf + w, buf_len - w, "]");
        return w;
    }

    if (strcmp(key, "fx_list") == 0) {
        int w = 0;
        w += snprintf(buf + w, buf_len - w, "[");
        for (int i = 0; i < FX_COUNT; i++)
            w += snprintf(buf + w, buf_len - w, "%s\"%s\"", i ? "," : "", fx_name(i));
        w += snprintf(buf + w, buf_len - w, "]");
        return w;
    }

    if (strcmp(key, "chain_params") == 0) {
        char models[4096], cabs[4096];
        emit_options(models, sizeof(models), s->model_names, s->model_count);
        emit_options(cabs, sizeof(cabs), s->cab_names, s->cab_count);

        int w = 0;
        w += snprintf(buf + w, buf_len - w,
            "[{\"key\":\"in_level\",\"name\":\"Input\",\"type\":\"float\","
              "\"min\":0.0,\"max\":1.0,\"default\":0.5,\"step\":0.01},"
             "{\"key\":\"out_level\",\"name\":\"Output\",\"type\":\"float\","
              "\"min\":0.0,\"max\":1.0,\"default\":0.85,\"step\":0.01},"
             "{\"key\":\"cpu\",\"name\":\"CPU\",\"type\":\"int\",\"min\":0,"
              "\"max\":100,\"default\":0,\"step\":1,\"unit\":\"%%\","
              "\"access\":\"read\",\"live\":true},"
             "{\"key\":\"sel_block\",\"name\":\"Block\",\"type\":\"int\","
              "\"min\":0,\"max\":%d,\"default\":0,\"step\":1},"
             "{\"key\":\"split\",\"name\":\"Split\",\"type\":\"int\","
              "\"min\":0,\"max\":%d,\"default\":0,\"step\":1},"
             "{\"key\":\"pan_a\",\"name\":\"Pan L\",\"type\":\"float\","
              "\"min\":-1.0,\"max\":1.0,\"default\":-1.0,\"step\":0.02},"
             "{\"key\":\"pan_b\",\"name\":\"Pan R\",\"type\":\"float\","
              "\"min\":-1.0,\"max\":1.0,\"default\":1.0,\"step\":0.02}",
             NUM_BLOCKS - 1, NUM_BLOCKS);

        char fxopts[1024]; int fw = 0;
        fw += snprintf(fxopts + fw, sizeof(fxopts) - fw, "[");
        for (int i = 0; i < FX_COUNT; i++)
            fw += snprintf(fxopts + fw, sizeof(fxopts) - fw, "%s\"%s\"",
                           i ? "," : "", fx_name(i));
        snprintf(fxopts + fw, sizeof(fxopts) - fw, "]");

        /* The per-block cost is two option lists plus the pedal list plus
         * the fixed text, so the headroom to check is MEASURED rather than
         * guessed at. It was a flat 3072 while one iteration can write over
         * 8 KB with a full card - snprintf truncates safely, but a truncated
         * array is invalid JSON, which is silently rejected and falls back
         * to module.json, which is how `access` and `live` went missing for
         * four builds. */
        const int per_block = (int)(strlen(models) * 1 + strlen(cabs) * 1 +
                                    strlen(fxopts) + 1024);
        for (int i = 1; i <= NUM_BLOCKS && w + per_block < buf_len; i++) {
            w += snprintf(buf + w, buf_len - w,
                ",{\"key\":\"b%d_type\",\"name\":\"Type\",\"type\":\"enum\","
                  "\"options\":[\"Off\",\"NAM\",\"Cab\",\"FX\"],\"default\":0}"
                ",{\"key\":\"b%d_on\",\"name\":\"On\",\"type\":\"enum\","
                  "\"options\":[\"On\",\"Bypass\"],\"default\":0}"
                ",{\"key\":\"b%d_lane\",\"name\":\"Lane\",\"type\":\"enum\","
                  "\"options\":[\"L\",\"R\"],\"default\":0}"
                ",{\"key\":\"b%d_model\",\"name\":\"Model\",\"type\":\"enum\","
                  "\"options\":%s,\"default\":0}"
                ",{\"key\":\"b%d_quality\",\"name\":\"Qual\",\"type\":\"enum\","
                  "\"options\":[\"Full\",\"Slim\",\"Lite\"],\"default\":0}"
                ",{\"key\":\"b%d_cab\",\"name\":\"Cab\",\"type\":\"enum\","
                  "\"options\":%s,\"default\":0}"
                ",{\"key\":\"b%d_fx\",\"name\":\"Pedal\",\"type\":\"enum\","
                  "\"options\":%s,\"default\":0}",
                i, i, i, i, models, i, i, cabs, i, fxopts);
            for (int k = 1; k <= FX_PARAMS; k++)
                w += snprintf(buf + w, buf_len - w,
                    ",{\"key\":\"b%d_p%d\",\"name\":\"P%d\",\"type\":\"float\","
                      "\"min\":0.0,\"max\":1.0,\"default\":0.5,\"step\":0.01}",
                    i, k, k);
            w += snprintf(buf + w, buf_len - w,
                ",{\"key\":\"b%d_cpu\",\"name\":\"CPU\",\"type\":\"int\","
                  "\"min\":0,\"max\":100,\"default\":0,\"step\":1,\"unit\":\"%%\","
                  "\"access\":\"read\",\"live\":true}", i);
        }
        w += snprintf(buf + w, buf_len - w, "]");
        return w;
    }

    /* NO ui_hierarchy, DELIBERATELY.
     *
     * enterComponentEdit (shadow_ui.js:16018) asks for one FIRST and, if it
     * gets it, opens the host's knob grid and returns - so a module that
     * declares a hierarchy never reaches enterComponentEditFallback, which
     * is the only place loadModuleUi runs. The two are exclusive and the
     * hierarchy wins. This module wants the PADS, which need
     * host_pad_block, which needs ui_chain.js, so it declares no hierarchy
     * and draws its own screen.
     *
     * The FX hierarchy cache is filled from module.json by
     * parse_ui_hierarchy_cache (it takes a DIRECTORY), and an empty cache
     * falls through to this get_param - so BOTH have to stay silent or the
     * grid comes back and the pads go away again.
     *
     * chain_params stays: the chain line, the LFO target picker and the
     * screen reader all read it, and none of them is the grid. */
    return -1;
}

static audio_fx_api_v2_t g_fx_api_v2;

extern "C" audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host) {
    g_host = host;
    g_fx_api_v2.api_version     = 2;
    g_fx_api_v2.create_instance = v2_create_instance;
    g_fx_api_v2.destroy_instance = v2_destroy_instance;
    g_fx_api_v2.process_block   = v2_process_block;
    g_fx_api_v2.set_param       = v2_set_param;
    g_fx_api_v2.get_param       = v2_get_param;
    g_fx_api_v2.on_midi         = v2_on_midi;
    return &g_fx_api_v2;
}
