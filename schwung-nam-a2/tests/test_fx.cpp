/* Run every pedal on a guitar-like signal and report what comes out.
 *
 * A pedal that outputs silence, NaN, or full-scale is a pedal that is
 * broken, and none of those needs the device to find. This has caught more
 * on this branch than any amount of reading has: a denormal spike, an IR
 * that boosted, a convolution that was memory-bound. */
#include <cstdio>
#include <cstring>
#include <cmath>
#include <ctime>
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

extern "C" audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *host);
static void host_log(const char *) {}

int main(int argc, char **argv) {
    host_api_v1_t host; memset(&host, 0, sizeof(host)); host.log = host_log;
    audio_fx_api_v2_t *api = move_audio_fx_init_v2(&host);
    void *in = api->create_instance(argc > 1 ? argv[1] : ".", "{}");
    if (!in) { printf("FAIL create_instance\n"); return 1; }

    /* Let the worker take the pedal buffers. */
    for (int i = 0; i < 40; i++) { struct timespec t = {0, 20000000L}; nanosleep(&t, NULL); }

    /* Only the pedal under test: every other block off. */
    for (int b = 1; b <= 8; b++) { char k[16]; snprintf(k, 16, "b%d_type", b); api->set_param(in, k, "0"); }
    api->set_param(in, "b1_type", "3");          /* BLK_FX */
    api->set_param(in, "in_level", "0.5");
    api->set_param(in, "out_level", "0.85");

    char buf[2048];
    api->get_param(in, "fx_list", buf, sizeof(buf));
    printf("pedals: %s\n\n", buf);

    /* HOW MANY PEDALS THERE ARE IS ASKED, NOT WRITTEN DOWN HERE. It was a
     * literal 16, so the ten added in 0.5.0 were never run by this test at
     * all - and a test that silently stops covering what it is for is worse
     * than no test, because the PASS is read as covering everything. */
    char names[64][32]; int fx_count = 0;
    for (const char *p = buf; *p && fx_count < 64; p++) {
        if (*p != '"') continue;
        const char *e = ++p;
        while (*e && *e != '"') e++;
        int len = (int)(e - p); if (len > 31) len = 31;
        memcpy(names[fx_count], p, len); names[fx_count][len] = 0;
        fx_count++; p = e;
    }
    if (fx_count < 16) { printf("FAIL fx_list parsed %d pedals\n", fx_count); return 1; }

    printf("%-3s %-12s %8s %8s %8s %6s %7s\n",
           "id", "pedal", "out pk", "out rms", "tail pk", "nan", "us/blk");

    int fails = 0;
    for (int id = 0; id < fx_count; id++) {
        char v[8]; snprintf(v, sizeof(v), "%d", id);
        api->set_param(in, "b1_fx", v);
        for (int k = 1; k <= 5; k++) {
            char kk[12]; snprintf(kk, sizeof(kk), "b1_p%d", k);
            api->set_param(in, kk, "0.6");
        }

        int16_t a[128 * 2];
        double pk = 0, sq = 0; long nsamp = 0, nan = 0;
        struct timespec t0, t1;
        clock_gettime(CLOCK_MONOTONIC, &t0);
        const int BLOCKS = 200;                     /* ~0.6 s */
        for (int blk = 0; blk < BLOCKS; blk++) {
            for (int i = 0; i < 128; i++) {
                /* A plucked note: two partials under an exponential decay. */
                double t = (blk * 128.0 + i) / 44100.0;
                double env = exp(-t * 2.5);
                double v2 = 0.5 * env * (sin(2 * M_PI * 196.0 * t)
                                       + 0.4 * sin(2 * M_PI * 392.0 * t));
                a[i * 2] = a[i * 2 + 1] = (int16_t)(v2 * 20000);
            }
            api->process_block(in, a, 128);
            for (int i = 0; i < 128; i++) {
                double s = a[i * 2] / 32768.0;
                if (s != s) nan++;
                double m = s < 0 ? -s : s;
                if (m > pk) pk = m;
                sq += s * s; nsamp++;
            }
        }
        clock_gettime(CLOCK_MONOTONIC, &t1);
        double us = ((t1.tv_sec - t0.tv_sec) * 1e6 + (t1.tv_nsec - t0.tv_nsec) / 1e3) / BLOCKS;

        /* Then silence. A delay and a reverb are SUPPOSED to have a tail;
         * what must not happen is a tail that does not shrink. So measure
         * it early and late and compare the two, rather than against a
         * constant that a working delay fails. */
        double tail_a = 0, tail = 0;
        for (int blk = 0; blk < 900; blk++) {
            memset(a, 0, sizeof(a));
            api->process_block(in, a, 128);
            for (int i = 0; i < 128; i++) {
                double m = fabs(a[i * 2] / 32768.0);
                if (blk >= 100 && blk < 200 && m > tail_a) tail_a = m;
                if (blk >= 800 && m > tail) tail = m;
            }
        }

        double rms = sqrt(sq / (nsamp ? nsamp : 1));
        printf("%-3d %-12s %8.4f %8.4f %8.4f %6ld %7.1f",
               id, names[id], pk, rms, tail, nan, us);

        const char *why = NULL;
        if (nan) why = "NaN";
        else if (pk < 0.001) why = "silent";
        else if (pk > 0.999) why = "full-scale";
        else if (tail > 0.02 && tail > tail_a * 0.5) why = "tail never decays";
        if (why) { printf("   <-- %s", why); fails++; }
        printf("\n");
    }

    /* ---- THE DOUBLER MUST NOT BE A CHORUS ----------------------------
     *
     * It was one: both ran on the same pitch-shifting delay, whose read
     * head wraps through a crossfade window at a fixed rate, and a fixed
     * rate is what a chorus IS. Reported from the device in those words.
     *
     * What separates them is measurable, so measure it: hold a sine and
     * look at how much the OUTPUT LEVEL swings. A chorus sweeps its comb
     * periodically, so its level does; a double-track sits still and only
     * its timing drifts. The assertion is a COMPARISON rather than a
     * threshold - the absolute number depends on the test signal, but "the
     * doubler moves less than the chorus" is the property that was false.
     */
    {
        double swing[2] = {0, 0};
        const int BLK = 400;
        for (int which = 0; which < 2; which++) {
            api->set_param(in, "b1_fx", which ? "14" : "8");   /* Doubler : Chorus */
            for (int k = 1; k <= 5; k++) {
                char kk[12]; snprintf(kk, sizeof(kk), "b1_p%d", k);
                api->set_param(in, kk, "0.5");
            }
            api->set_param(in, "b1_p3", "1.0");                /* full wet */
            static double env[400];
            int16_t a[128 * 2];
            for (int blk = 0; blk < BLK; blk++) {
                for (int i = 0; i < 128; i++) {
                    double t = (blk * 128.0 + i) / 44100.0;
                    a[i * 2] = a[i * 2 + 1] = (int16_t)(0.35 * sin(2 * M_PI * 200.0 * t) * 20000);
                }
                api->process_block(in, a, 128);
                double sq2 = 0;
                for (int i = 0; i < 128; i++) { double v = a[i * 2] / 32768.0; sq2 += v * v; }
                env[blk] = sqrt(sq2 / 128);
            }
            double mean = 0;
            for (int i = 200; i < BLK; i++) mean += env[i];
            mean /= (BLK - 200);
            double best = 0;
            for (double hz = 0.3; hz < 8.0; hz += 0.05) {
                double re = 0, im = 0;
                for (int i = 200; i < BLK; i++) {
                    double t = i * 128.0 / 44100.0;
                    re += (env[i] - mean) * cos(2 * M_PI * hz * t);
                    im += (env[i] - mean) * sin(2 * M_PI * hz * t);
                }
                double m = sqrt(re * re + im * im) / (BLK - 200);
                if (m > best) best = m;
            }
            swing[which] = (mean > 1e-9) ? (best / mean) : 0;
        }
        printf("\nlevel swing: chorus %.1f%%  doubler %.1f%%\n",
               swing[0] * 100, swing[1] * 100);
        if (!(swing[1] < swing[0])) {
            printf("   <-- the doubler moves as much as the chorus does\n");
            fails++;
        }
    }

    api->destroy_instance(in);
    printf("\n%s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
