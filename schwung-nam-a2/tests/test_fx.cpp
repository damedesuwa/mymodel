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

    /* WHERE EACH KNOB SHOULD SIT FOR A FAIR TEST, asked of the plugin.
     *
     * Everything used to go to 0.6, which was fine while every pedal's
     * third knob was a 0-100 "Level". With real pedals it is not: a 1176's
     * Output is 0..+24 dB and 0.6 of that is +14 dB, so the harness was
     * calling four pedals broken for doing exactly what their makeup knob
     * says on the tin. A knob spanning dB through zero belongs at zero;
     * one that only adds (a Boost's 0..20 dB) belongs at the bottom;
     * anything else at 0.6, which is a normal setting rather than a
     * neutral one. The ranges come from fx_specs, so this cannot drift
     * away from what the DSP reads. */
    static char specs[24576];
    api->get_param(in, "fx_specs", specs, sizeof(specs));
    static float neutral[64][5];
    for (int i = 0; i < 64; i++) for (int k = 0; k < 5; k++) neutral[i][k] = 0.6f;
    {
        const char *p = specs; int pedal = -1;
        while (*p) {
            if (!strncmp(p, "\"k\":[", 5)) {
                pedal++; p += 5;
                for (int k = 0; k < 5 && *p; k++) {
                    while (*p == ' ' || *p == ',') p++;
                    if (!strncmp(p, "null", 4)) { p += 4; continue; }
                    const char *end = strchr(p, '}');
                    if (!end) break;
                    char unit[8] = "";
                    const char *u = strstr(p, "\"u\":\"");
                    if (u && u < end) {
                        u += 5; int n2 = 0;
                        while (*u && *u != '"' && n2 < 7) unit[n2++] = *u++;
                        unit[n2] = 0;
                    }
                    float lo = 0, hi = 1;
                    const char *l = strstr(p, "\"lo\":"), *h = strstr(p, "\"hi\":");
                    if (l && l < end) lo = (float)atof(l + 5);
                    if (h && h < end) hi = (float)atof(h + 5);
                    if (pedal >= 0 && pedal < 64 && !strcmp(unit, "dB")) {
                        neutral[pedal][k] = (lo < 0 && hi > 0)
                            ? (0.0f - lo) / (hi - lo)     /* 0 dB */
                            : (lo >= 0 ? 0.0f : 0.6f);    /* makeup-only: none */
                    }
                    p = end + 1;
                }
                continue;
            }
            p++;
        }
    }

    printf("%-3s %-12s %8s %8s %8s %6s %7s\n",
           "id", "pedal", "out pk", "out rms", "tail pk", "nan", "us/blk");

    int fails = 0;
    for (int id = 0; id < fx_count; id++) {
        char v[8]; snprintf(v, sizeof(v), "%d", id);
        api->set_param(in, "b1_fx", v);
        for (int k = 1; k <= 5; k++) {
            char kk[12], vv[16];
            snprintf(kk, sizeof(kk), "b1_p%d", k);
            snprintf(vv, sizeof(vv), "%.4f", neutral[id][k - 1]);
            api->set_param(in, kk, vv);
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
            api->set_param(in, "b1_fx", which ? "36" : "23");  /* Doubler : Chorus */
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

    /* ---- THE DOUBLER IS THE ONE STEREO PEDAL -------------------------
     *
     * Two mono attempts at it were both reported as unnatural, and the
     * second report was right: two copies summed to mono ARE a comb
     * filter on one signal, and no tuning makes a comb filter sound like
     * a room. Width is the fix, so width is what is asserted - at Width 0
     * the output must be mono (the collapse has to keep working, it is a
     * real requirement) and at Width 100 the two channels must differ.
     */
    {
        api->set_param(in, "b1_fx", "36");
        api->set_param(in, "b1_p1", "0.5");   /* Delay  */
        api->set_param(in, "b1_p2", "0.4");   /* Detune */
        api->set_param(in, "b1_p4", "0.8");   /* Level  */
        double diff[2] = {0, 0};
        for (int which = 0; which < 2; which++) {
            api->set_param(in, "b1_p3", which ? "1.0" : "0.0");   /* Width */
            int16_t a[128 * 2];
            double d = 0, e = 0;
            for (int blk = 0; blk < 300; blk++) {
                for (int i = 0; i < 128; i++) {
                    double t = (blk * 128.0 + i) / 44100.0;
                    double env = exp(-fmod(t, 0.7) * 3.0);
                    a[i * 2] = a[i * 2 + 1] =
                        (int16_t)(0.35 * env * sin(2 * M_PI * 220.0 * t) * 20000);
                }
                api->process_block(in, a, 128);
                if (blk < 40) continue;                 /* let the line fill */
                for (int i = 0; i < 128; i++) {
                    double l = a[i * 2] / 32768.0, r = a[i * 2 + 1] / 32768.0;
                    d += (l - r) * (l - r);
                    e += (l + r) * (l + r) * 0.25;
                }
            }
            diff[which] = (e > 1e-12) ? sqrt(d / e) : 0;
        }
        printf("doubler L-R: width 0 -> %.4f, width 100 -> %.4f\n", diff[0], diff[1]);
        if (diff[0] > 1e-6) {
            printf("   <-- Width 0 does not collapse to mono\n"); fails++;
        }
        if (diff[1] < 0.15) {
            printf("   <-- Width 100 produces no stereo difference\n"); fails++;
        }
    }

    /* ---- THE SPLIT ---------------------------------------------------
     *
     * Two lanes that never rejoin, so the thing to prove is that they are
     * genuinely SEPARATE: different pedals on each side must produce
     * different audio on each side, and a lane that nothing is panned to
     * must be silent. A split that quietly ran one signal twice would
     * pass every smoke test and sound almost right.
     */
    {
        /* BOTH ROWS CLEARED. The fork is derived from the top row, so a
         * stale block up there from an earlier case is a fork this case
         * never asked for - and the lanes would be right for the wrong
         * reason. */
        for (int b = 1; b <= 8; b++) {
            char k[16];
            snprintf(k, 16, "b%d_type", b); api->set_param(in, k, "0");
            snprintf(k, 16, "t%d_type", b); api->set_param(in, k, "0");
        }
        api->set_param(in, "b1_type", "3");
        api->set_param(in, "b1_fx", "19");        /* EQ, flat - the shared head */
        /* The fork is HERE: column 2 of the top row is the first thing on
         * the branch, so nothing has to say "split at 2". */
        api->set_param(in, "t2_type", "3");
        api->set_param(in, "t2_fx", "12");        /* Boost, on the top rail */
        api->set_param(in, "t2_p1", "0.6");
        api->set_param(in, "b2_type", "3");
        api->set_param(in, "b2_fx", "17");        /* Star Gate, on the bottom */
        api->set_param(in, "b2_p1", "1.0");       /* wide open, so it passes */
        api->set_param(in, "pan_a", "-1.0");
        api->set_param(in, "pan_b", "1.0");

        int16_t a[128 * 2];
        double dl = 0, dr = 0;
        for (int blk = 0; blk < 300; blk++) {
            for (int i = 0; i < 128; i++) {
                double t = (blk * 128.0 + i) / 44100.0;
                a[i * 2] = a[i * 2 + 1] =
                    (int16_t)(0.30 * sin(2 * M_PI * 220.0 * t) * 20000);
            }
            api->process_block(in, a, 128);
            if (blk < 50) continue;
            for (int i = 0; i < 128; i++) {
                dl += (a[i * 2] / 32768.0) * (a[i * 2] / 32768.0);
                dr += (a[i * 2 + 1] / 32768.0) * (a[i * 2 + 1] / 32768.0);
            }
        }
        printf("\nsplit: hard-panned lanes  L %.4f  R %.4f  ratio %.2f\n",
               sqrt(dl / 32000), sqrt(dr / 32000), sqrt(dl / (dr + 1e-12)));
        if (!(dl > 1e-9 && dr > 1e-9)) {
            printf("   <-- a lane is silent\n"); fails++;
        }
        /* A Boost on one side and a gate on the other cannot come out the
         * same; if they do, both lanes are carrying one signal. */
        if (fabs(sqrt(dl / (dr + 1e-12)) - 1.0) < 0.05) {
            printf("   <-- the two lanes are the same signal\n"); fails++;
        }

        /* Both hard left: the right channel must be silent. That is the
         * check that catches a pan that is decorative. */
        api->set_param(in, "pan_b", "-1.0");
        double r2 = 0;
        for (int blk = 0; blk < 200; blk++) {
            for (int i = 0; i < 128; i++) {
                double t = (blk * 128.0 + i) / 44100.0;
                a[i * 2] = a[i * 2 + 1] =
                    (int16_t)(0.30 * sin(2 * M_PI * 220.0 * t) * 20000);
            }
            api->process_block(in, a, 128);
            if (blk < 50) continue;
            for (int i = 0; i < 128; i++)
                r2 += (a[i * 2 + 1] / 32768.0) * (a[i * 2 + 1] / 32768.0);
        }
        printf("split: both lanes panned hard left -> right rms %.6f\n",
               sqrt(r2 / 19200));
        if (sqrt(r2 / 19200) > 0.002) {
            printf("   <-- the pan does not reach the output\n"); fails++;
        }

        /* EIGHT COLUMNS ON EACH SIDE, WHICH IS THE WHOLE POINT.
         *
         * The split used to be carved out of one row of eight, so forking
         * at column 1 left SEVEN slots to share. Fork at column 1 here
         * and load column EIGHT of the top row: if that slot is reached
         * at all, each lane still has its full eight.
         *
         * Proved by CUTTING one lane from its last column - an EQ with
         * every band at -12 dB - and asking whether the channel it is
         * panned to dropped. A slot that is not in the path cannot do
         * that. A cut rather than a boost because a boost would run into
         * the output clamp, and a clamped channel is loud for a reason
         * that has nothing to do with routing. */
        api->set_param(in, "pan_a", "-1.0");
        api->set_param(in, "pan_b", "1.0");
        api->set_param(in, "t1_type", "3");
        api->set_param(in, "t1_fx", "19");        /* EQ, flat: forks at col 1 */
        api->set_param(in, "t8_type", "3");
        api->set_param(in, "t8_fx", "19");        /* EQ ... */
        api->set_param(in, "t8_p1", "0.0");       /* ... all three bands */
        api->set_param(in, "t8_p2", "0.0");       /*     at -12 dB       */
        api->set_param(in, "t8_p3", "0.0");
        api->set_param(in, "t2_type", "0");
        double el = 0, er = 0;
        for (int blk = 0; blk < 300; blk++) {
            for (int i = 0; i < 128; i++) {
                double t = (blk * 128.0 + i) / 44100.0;
                a[i * 2] = a[i * 2 + 1] =
                    (int16_t)(0.30 * sin(2 * M_PI * 220.0 * t) * 20000);
            }
            api->process_block(in, a, 128);
            if (blk < 100) continue;
            for (int i = 0; i < 128; i++) {
                el += (a[i * 2] / 32768.0) * (a[i * 2] / 32768.0);
                er += (a[i * 2 + 1] / 32768.0) * (a[i * 2 + 1] / 32768.0);
            }
        }
        printf("split: column 8 of the branch  L %.5f  R %.5f\n",
               sqrt(el / 25600), sqrt(er / 25600));
        if (!(sqrt(er / 25600) > 0.01)) {
            printf("   <-- the main lane went quiet too\n"); fails++;
        }
        if (!(sqrt(el / 25600) < sqrt(er / 25600) * 0.6)) {
            printf("   <-- the branch's 8th column is not in the path\n"); fails++;
        }


        /* AND THEY COME BACK TOGETHER WHERE THE BRANCH ENDS.
         *
         * Two lanes hard apart, then ONE block in the tail. If the merge
         * is real, that block is heard on BOTH channels - which is the
         * whole point of a shared reverb after two amps. If the lanes
         * were still separate it could only reach the one it sits on,
         * and a test that only looked at the total would pass either
         * way. */
        for (int b = 1; b <= 8; b++) {
            char k[16];
            snprintf(k, 16, "b%d_type", b); api->set_param(in, k, "0");
            snprintf(k, 16, "t%d_type", b); api->set_param(in, k, "0");
        }
        api->set_param(in, "pan_a", "-1.0");      /* branch hard left   */
        api->set_param(in, "pan_b", "1.0");       /* main hard right    */
        api->set_param(in, "t2_type", "3");
        api->set_param(in, "t2_fx", "19");        /* EQ flat: forks at col 2 */
        /* THE MERGE IS A SETTING NOW, and its default is the output - so
         * without this line the tail would still be two lanes and the
         * block below would only reach the one it sits on. */
        api->set_param(in, "merge", "4");
        /* One measurement, twice, so the two runs cannot differ in
         * anything but the block being switched. */
        double ml = 0, mr = 0, bl = 0, br = 0;
        auto measure = [&](double *L, double *R) {
            double l = 0, r = 0;
            for (int blk = 0; blk < 300; blk++) {
                for (int i = 0; i < 128; i++) {
                    double t = (blk * 128.0 + i) / 44100.0;
                    a[i * 2] = a[i * 2 + 1] =
                        (int16_t)(0.30 * sin(2 * M_PI * 220.0 * t) * 20000);
                }
                api->process_block(in, a, 128);
                if (blk < 100) continue;
                for (int i = 0; i < 128; i++) {
                    l += (a[i * 2] / 32768.0) * (a[i * 2] / 32768.0);
                    r += (a[i * 2 + 1] / 32768.0) * (a[i * 2 + 1] / 32768.0);
                }
            }
            *L = sqrt(l / 25600); *R = sqrt(r / 25600);
        };
        api->set_param(in, "b6_type", "0");
        measure(&bl, &br);
        api->set_param(in, "b6_type", "3");
        api->set_param(in, "b6_fx", "19");        /* EQ ...          */
        api->set_param(in, "b6_p1", "0.0");       /* ... all bands   */
        api->set_param(in, "b6_p2", "0.0");       /*     at -12 dB   */
        api->set_param(in, "b6_p3", "0.0");
        measure(&ml, &mr);
        printf("\nmerge: tail empty          L %.5f  R %.5f\n", bl, br);
        printf("merge: -12 dB in the tail  L %.5f  R %.5f\n", ml, mr);
        if (!(ml < bl * 0.8)) {
            printf("   <-- the tail does not reach the LEFT lane\n"); fails++;
        }
        if (!(mr < br * 0.8)) {
            printf("   <-- the tail does not reach the RIGHT lane\n"); fails++;
        }

        api->set_param(in, "merge", "0");
        for (int b = 1; b <= 8; b++) {
            char k[16]; snprintf(k, 16, "t%d_type", b); api->set_param(in, k, "0");
        }
    }

    api->destroy_instance(in);
    printf("\n%s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
