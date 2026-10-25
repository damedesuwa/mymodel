/* A FILE LANDS ON THE CARD WHILE THE MODULE IS PLAYING.
 *
 * Three claims, and the third is the one that cost a design:
 *
 *   1. the picker notices, with no reload of the module
 *   2. `list_gen` moves, so the UI knows to ask again
 *   3. every block keeps the capture it had - BY NAME. The lists are
 *      sorted, so a file dropped in ahead of yours shifts its index, and a
 *      refresh that only replaced the list would silently re-point the
 *      block at its neighbour while it kept PLAYING the model already in
 *      memory. Screen and sound disagree, with nothing to say why.
 *
 * It also runs process_block throughout, because the refresh exists to be
 * inaudible: the old `rescan` param called opendir on the SPI callback.
 */
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cmath>
#include <ctime>
#include <sys/stat.h>
#include "plugin_api_v1.h"

typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *, const char *);
    void  (*destroy_instance)(void *);
    void  (*process_block)(void *, int16_t *, int);
    void  (*set_param)(void *, const char *, const char *);
    int   (*get_param)(void *, const char *, char *, int);
    void  (*on_midi)(void *, const uint8_t *, int, int);
} audio_fx_api_v2_t;
extern "C" audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *);

static audio_fx_api_v2_t *api;
static void *in;
static int fails;

static const char *get(const char *k) {
    static char buf[8192];
    buf[0] = '\0';
    const int n = api->get_param(in, k, buf, sizeof(buf));
    if (n < 0) return "(null)";
    return buf;
}

static void check(const char *what, const char *got, const char *want) {
    if (strcmp(got, want) == 0) {
        printf("ok   %s = %s\n", what, got);
    } else {
        printf("FAIL %s = %s   (wanted %s)\n", what, got, want);
        fails++;
    }
}

static double mono_us(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec * 1e6 + (double)t.tv_nsec / 1e3;
}

/* Runs audio while waiting, because waiting WITHOUT audio would not notice
 * the thing this is here to prevent. Returns the worst single block. */
static double run_until(const char *key, const char *until, double timeout_s,
                        int *nan_out) {
    int16_t a[256];
    double ph = 0.0, worst = 0.0;
    const double t0 = mono_us();
    *nan_out = 0;
    while (mono_us() - t0 < timeout_s * 1e6) {
        for (int i = 0; i < 128; i++) {
            const double v = 0.4 * sin(ph);
            ph += 2.0 * M_PI * 220.0 / 44100.0;
            a[i * 2] = a[i * 2 + 1] = (int16_t)(v * 32767.0);
        }
        const double b0 = mono_us();
        api->process_block(in, a, 128);
        const double dt = mono_us() - b0;
        if (dt > worst) worst = dt;
        for (int i = 0; i < 256; i++)
            if (a[i] != a[i]) (*nan_out)++;
        if (key && strcmp(get(key), until) != 0) break;
        struct timespec ts = { 0, 2900000L };        /* one frame period */
        nanosleep(&ts, NULL);
    }
    return worst;
}

static void touch_nam(const char *dir, const char *name) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/amps/%s", dir, name);
    FILE *f = fopen(path, "wb");
    if (!f) { printf("FAIL could not write %s\n", path); fails++; return; }
    /* Content is irrelevant: the list comes from the NAME, and the stub
     * loader accepts anything. */
    fputs("{}", f);
    fclose(f);
}

int main(int argc, char **argv) {
    if (argc < 2) { printf("usage: test_refresh <module_dir>\n"); return 2; }
    const char *dir = argv[1];

    api = move_audio_fx_init_v2(NULL);
    in = api->create_instance(dir, NULL);
    if (!in) { printf("FAIL create_instance returned NULL\n"); return 1; }
    api->set_param(in, "in_level", "0.5");
    api->set_param(in, "out_level", "0.85");

    /* The fixture starts with ONE capture, "M2". */
    check("model_list at entry", get("model_list"), "[\"M2\"]");
    check("list_gen at entry", get("list_gen"), "0");

    /* Load it into b1 and let the worker finish. */
    api->set_param(in, "b1_type", "1");
    api->set_param(in, "b1_model", "0");
    int nans = 0;
    run_until(NULL, NULL, 0.5, &nans);
    check("b1_model before", get("b1_model"), "0");

    /* A file arrives that sorts BEFORE it. */
    printf("--- dropping M1.nam into amps/ while playing ---\n");
    touch_nam(dir, "M1.nam");

    const double worst = run_until("list_gen", "0", 8.0, &nans);
    printf("     worst process_block during the wait: %.0f us\n", worst);

    check("list_gen after", get("list_gen"), "1");
    check("model_list after", get("model_list"), "[\"M1\",\"M2\"]");
    /* THE CLAIM. b1 still plays M2, whose index is now 1. */
    check("b1_model rebound by name", get("b1_model"), "1");

    if (nans) { printf("FAIL %d NaN samples in the output\n", nans); fails++; }
    else printf("ok   no NaN through the refresh\n");

    /* A FLIP MAY NOT FOLLOW A FLIP INSIDE A SECOND - the pending request
     * must survive being asked too early rather than being swallowed. */
    printf("--- rescan nudge, immediately ---\n");
    touch_nam(dir, "M0.nam");
    api->set_param(in, "rescan", "1");
    run_until("list_gen", "1", 8.0, &nans);
    check("list_gen after nudge", get("list_gen"), "2");
    check("model_list after nudge", get("model_list"), "[\"M0\",\"M1\",\"M2\"]");
    check("b1_model still M2", get("b1_model"), "2");

    /* And a poll that finds nothing must NOT publish: the UI would re-read
     * four lists every two seconds for nothing. */
    printf("--- 3 s with an unchanged card ---\n");
    run_until(NULL, NULL, 3.0, &nans);
    check("list_gen unchanged", get("list_gen"), "2");

    api->destroy_instance(in);
    printf("\n%s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
