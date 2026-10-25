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
#include <cstdlib>
#include <string>
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
        api->set_param(in, "merge", "3");   /* parallel to column 3 */
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


    /* ------------------------------------------------------------------
     * MIC PLACEMENT ON THE CAB.
     *
     * An IR is one mic in one place. What this adds is the DIFFERENCE
     * between there and where you want it, so each control is checked for
     * the thing it claims rather than for "the sound changed".
     * ------------------------------------------------------------------ */
    {
        for (int b = 1; b <= 8; b++) {
            char k[16];
            snprintf(k, 16, "b%d_type", b); api->set_param(in, k, "0");
            snprintf(k, 16, "t%d_type", b); api->set_param(in, k, "0");
        }
        api->set_param(in, "merge", "0");
        api->set_param(in, "pan_a", "0.0");
        api->set_param(in, "pan_b", "0.0");
        api->set_param(in, "b1_type", "2");            /* BLK_CAB */
        for (int i = 0; i < 60; i++) { struct timespec t = {0, 20000000L}; nanosleep(&t, NULL); }

        char nm[256] = {0};
        api->get_param(in, "b1_cab", nm, sizeof(nm));
        int16_t a[128 * 2];

        /* An IMPULSE, so the delay is a position in the file and not an
         * inference from a correlation. */
        auto peak_at = [&](const char *p2) {
            api->set_param(in, "b1_p2", p2);
            for (int blk = 0; blk < 20; blk++) {        /* settle */
                memset(a, 0, sizeof(a));
                api->process_block(in, a, 128);
            }
            int best = -1; double bv = 0;
            for (int blk = 0; blk < 4; blk++) {
                memset(a, 0, sizeof(a));
                if (blk == 0) { a[0] = a[1] = 20000; }
                api->process_block(in, a, 128);
                for (int i = 0; i < 128; i++) {
                    double v = fabs(a[i * 2] / 32768.0);
                    if (v > bv) { bv = v; best = blk * 128 + i; }
                }
            }
            return best;
        };
        /* RMS AT ONE FREQUENCY, which is what a shelf is specified in.
         *
         * The first version of this measured "brightness" as the mean
         * absolute first difference of a sawtooth, and read a -12 dB
         * shelf at 3 kHz as a 4% change - because the cab itself has
         * almost no energy up there and the metric was dominated by the
         * fundamental. A sine at the frequency under test cannot be
         * fooled that way, and the number it gives is the shelf. */
        auto rms_at = [&](double hz, const char *key, const char *val) {
            api->set_param(in, key, val);
            double e = 0;
            for (int blk = 0; blk < 200; blk++) {
                for (int i = 0; i < 128; i++) {
                    double t = (blk * 128.0 + i) / 44100.0;
                    a[i * 2] = a[i * 2 + 1] =
                        (int16_t)(0.45 * sin(2 * M_PI * hz * t) * 20000);
                }
                api->process_block(in, a, 128);
                if (blk < 80) continue;
                for (int i = 0; i < 128; i++) {
                    double v = a[i * 2] / 32768.0;
                    e += v * v;
                }
            }
            return sqrt(e / (120 * 128));
        };

        const int p_near = peak_at("0.5");              /* as captured */
        const int p_far  = peak_at("1.0");              /* +40 cm      */
        api->set_param(in, "b1_p2", "0.5");
        printf("\nmic: impulse peak  as-captured %d  +40cm %d  (expect +%d)\n",
               p_near, p_far, (int)(40.0 / 34300.0 * 44100.0));
        if (p_far - p_near < 45 || p_far - p_near > 57) {
            printf("   <-- time of flight is not 40 cm\n"); fails++;
        }

        /* -12 dB of shelf at the cone edge: under half the amplitude,
         * and the low end must NOT follow it down. */
        const double hi_cap  = rms_at(4500, "b1_p1", "0.0");
        const double hi_edge = rms_at(4500, "b1_p1", "1.0");
        const double lo_cap  = rms_at(200,  "b1_p1", "0.0");
        const double lo_edge = rms_at(200,  "b1_p1", "1.0");
        api->set_param(in, "b1_p1", "0.0");
        printf("mic: pos 4.5k  cap %.5f -> edge %.5f (%.2fx)   200Hz %.2fx\n",
               hi_cap, hi_edge, hi_edge / (hi_cap + 1e-12),
               lo_edge / (lo_cap + 1e-12));
        if (!(hi_edge < hi_cap * 0.45)) {
            printf("   <-- walking out to the edge did not lose the top\n"); fails++;
        }
        if (!(lo_edge > lo_cap * 0.95)) {
            printf("   <-- it took the low end with it, which is not a move\n"); fails++;
        }

        /* Proximity is a LOW-end effect, and the knob is centred on the
         * capture so going BELOW centre is what raises it. */
        const double pr_mid   = rms_at(90, "b1_p2", "0.5");
        const double pr_close = rms_at(90, "b1_p2", "0.0");   /* -40 cm */
        api->set_param(in, "b1_p2", "0.5");
        printf("mic: prox 90Hz as-captured %.5f -> close %.5f (%.2fx)\n",
               pr_mid, pr_close, pr_close / (pr_mid + 1e-12));
        if (!(pr_close > pr_mid * 1.8)) {
            printf("   <-- closing in did not raise the bass\n"); fails++;
        }

        const double c_rib = rms_at(7000, "b1_p3", "0.75");   /* Ribbon    */
        const double c_con = rms_at(7000, "b1_p3", "1.0");    /* Condenser */
        api->set_param(in, "b1_p3", "0.0");
        printf("mic: char 7k   ribbon %.5f  condenser %.5f (%.2fx)\n",
               c_rib, c_con, c_con / (c_rib + 1e-12));
        if (!(c_rib < c_con * 0.6)) {
            printf("   <-- a ribbon is not darker than a condenser\n"); fails++;
        }

        /* PHASE IS THE WHOLE CHAIN'S PROOF. If it is a sample-exact
         * negation then nothing else moved between the two runs - which
         * is also what says the DEFAULTS are a no-op rather than a filter
         * that happens to be mild. */
        api->set_param(in, "b1_p1", "0.0");
        api->set_param(in, "b1_p2", "0.5");
        api->set_param(in, "b1_p3", "0.0");
        auto capture = [&](const char *ph, int16_t *out) {
            api->set_param(in, "b1_p4", ph);
            for (int blk = 0; blk < 120; blk++) {
                for (int i = 0; i < 128; i++) {
                    double t = (blk * 128.0 + i) / 44100.0;
                    a[i * 2] = a[i * 2 + 1] =
                        (int16_t)(0.30 * sin(2 * M_PI * 220.0 * t) * 20000);
                }
                api->process_block(in, a, 128);
            }
            memcpy(out, a, sizeof(int16_t) * 256);
        };
        int16_t norm[256], inv[256];
        capture("0.0", norm);
        capture("1.0", inv);
        api->set_param(in, "b1_p4", "0.0");
        int bad = 0, live = 0;
        for (int i = 0; i < 256; i++) {
            if (abs(norm[i]) > 200) live++;
            if (abs(norm[i] + inv[i]) > 2) bad++;
        }
        printf("mic: phase         %d live samples, %d not a clean negation\n",
               live, bad);
        if (live < 100 || bad > 0) {
            printf("   <-- Inv is not the exact negation of Norm\n"); fails++;
        }
    }


    /* ------------------------------------------------------------------
     * REVERB IR.
     *
     * The same convolution as a cab with the dry kept and a tap count you
     * choose. Each knob is checked for the thing it claims: a reverb that
     * merely "sounds different" would pass on any of them.
     * ------------------------------------------------------------------ */
    {
        char list[1024] = {0};
        api->get_param(in, "ir_list", list, sizeof(list));
        if (!strstr(list, "(none)")) {
            for (int b = 1; b <= 8; b++) {
                char k[16];
                snprintf(k, 16, "b%d_type", b); api->set_param(in, k, "0");
                snprintf(k, 16, "t%d_type", b); api->set_param(in, k, "0");
            }
            api->set_param(in, "merge", "0");
            api->set_param(in, "pan_a", "0.0");
            api->set_param(in, "pan_b", "0.0");
            api->set_param(in, "b1_type", "4");           /* BLK_IR */
            for (int i = 0; i < 80; i++) { struct timespec t = {0, 20000000L}; nanosleep(&t, NULL); }

            int16_t a[128 * 2];
            /* THE TAIL IS THE WHOLE POINT, so it is measured as energy
             * AFTER the input stops - which is the one thing a filter
             * cannot fake. */
            auto tail = [&](const char *key, const char *val) {
                if (key) api->set_param(in, key, val);
                for (int i = 0; i < 60; i++) { struct timespec t = {0, 10000000L}; nanosleep(&t, NULL); }
                for (int blk = 0; blk < 40; blk++) {      /* flush */
                    memset(a, 0, sizeof(a));
                    api->process_block(in, a, 128);
                }
                for (int blk = 0; blk < 8; blk++) {       /* a short burst */
                    for (int i = 0; i < 128; i++) {
                        double t = (blk * 128.0 + i) / 44100.0;
                        a[i * 2] = a[i * 2 + 1] =
                            (int16_t)(0.4 * sin(2 * M_PI * 440.0 * t) * 20000);
                    }
                    api->process_block(in, a, 128);
                }
                double e = 0;
                for (int blk = 0; blk < 60; blk++) {      /* silence after */
                    memset(a, 0, sizeof(a));
                    api->process_block(in, a, 128);
                    for (int i = 0; i < 128; i++) {
                        double v = a[i * 2] / 32768.0;
                        e += v * v;
                    }
                }
                return sqrt(e / (60 * 128));
            };

            const double dry  = tail("b1_p1", "0.0");     /* Mix 0 */
            const double wet  = tail("b1_p1", "0.6");
            printf("\nir: tail after the note stops  mix 0 %.6f  mix 60%% %.6f\n",
                   dry, wet);
            if (!(dry < 0.0005)) {
                printf("   <-- Mix 0 is not transparent\n"); fails++;
            }
            if (!(wet > dry * 20 && wet > 0.002)) {
                printf("   <-- there is no tail\n"); fails++;
            }

            /* LEN IS TAPS, so a longer one rings for longer. */
            const double t_short = tail("b1_p2", "0.0");   /* 46 ms  */
            const double t_long  = tail("b1_p2", "1.0");   /* 372 ms */
            printf("ir: 46ms %.6f -> 372ms %.6f (%.2fx)\n",
                   t_short, t_long, t_long / (t_short + 1e-12));
            if (!(t_long > t_short * 1.5)) {
                printf("   <-- Len does not change how long it rings\n"); fails++;
            }

            /* LOW CUT TAKES THE BOTTOM OFF THE TAIL AND NOT THE DRY. */
            api->set_param(in, "b1_p2", "0.5");
            const double lc_off = tail("b1_p4", "0.0");
            const double lc_on  = tail("b1_p4", "1.0");    /* 400 Hz */
            api->set_param(in, "b1_p4", "0.0");
            printf("ir: locut off %.6f -> 400Hz %.6f (%.2fx)\n",
                   lc_off, lc_on, lc_on / (lc_off + 1e-12));
            if (!(lc_on < lc_off * 0.8)) {
                printf("   <-- the low cut does not reach the tail\n"); fails++;
            }
        } else {
            printf("\nir: no irs/ in the fixture, reverb unchecked\n");
        }
    }


    /* ------------------------------------------------------------------
     * THE TONE STACK.
     *
     * We already have a three band EQ, so this is only worth having if it
     * behaves like the CIRCUIT rather than like an EQ with amp labels on
     * it. Four claims, four measurements - and each is at the frequency
     * the claim is about, because a broadband "it got darker" would pass
     * for any of them.
     * ------------------------------------------------------------------ */
    {
        for (int b = 1; b <= 8; b++) {
            char k[16];
            snprintf(k, 16, "b%d_type", b); api->set_param(in, k, "0");
            snprintf(k, 16, "t%d_type", b); api->set_param(in, k, "0");
        }
        api->set_param(in, "merge", "0");
        api->set_param(in, "pan_a", "0.0");
        api->set_param(in, "pan_b", "0.0");
        api->set_param(in, "b1_type", "3");
        api->set_param(in, "b1_fx", "50");            /* Tone Stack */
        int16_t a[128 * 2];

        auto rms_at = [&](double hz, const char *amp, const char *bass,
                          const char *mid, const char *treb, const char *pres) {
            api->set_param(in, "b1_p1", amp);
            api->set_param(in, "b1_p2", bass);
            api->set_param(in, "b1_p3", mid);
            api->set_param(in, "b1_p4", treb);
            api->set_param(in, "b1_p5", pres);
            double e = 0;
            for (int blk = 0; blk < 160; blk++) {
                for (int i = 0; i < 128; i++) {
                    double t = (blk * 128.0 + i) / 44100.0;
                    a[i * 2] = a[i * 2 + 1] =
                        (int16_t)(0.35 * sin(2 * M_PI * hz * t) * 20000);
                }
                api->process_block(in, a, 128);
                if (blk < 60) continue;
                for (int i = 0; i < 128; i++) {
                    double v = a[i * 2] / 32768.0;
                    e += v * v;
                }
            }
            return 20.0 * log10(sqrt(e / (100 * 128)) + 1e-12);
        };
        const char *MAR = "0.3333";                   /* Marshall */
        const double noon_lo  = rms_at( 100, MAR, "0.5", "0.5", "0.5", "0.0");
        const double noon_mid = rms_at( 500, MAR, "0.5", "0.5", "0.5", "0.0");
        const double noon_hi  = rms_at(4000, MAR, "0.5", "0.5", "0.5", "0.0");

        /* 1. MID AT ZERO IS A NOTCH, not flat - and only at the mid. */
        const double cut_mid = rms_at( 500, MAR, "0.5", "0.0", "0.5", "0.0");
        const double cut_lo  = rms_at( 100, MAR, "0.5", "0.0", "0.5", "0.0");
        const double cut_hi  = rms_at(4000, MAR, "0.5", "0.0", "0.5", "0.0");
        printf("\nstack: mid 5->0   %+.1f dB at 500Hz  (100Hz %+.1f, 4k %+.1f)\n",
               cut_mid - noon_mid, cut_lo - noon_lo, cut_hi - noon_hi);
        if (!(cut_mid - noon_mid < -8.0)) {
            printf("   <-- Mid at 0 is not a notch, it is an EQ at 0 dB\n"); fails++;
        }
        if (fabs(cut_hi - noon_hi) > 3.0) {
            printf("   <-- and it took the top with it\n"); fails++;
        }

        /* 2. THE CONTROLS ARE AT THEIR OWN FREQUENCIES. */
        const double b0 = rms_at(100, MAR, "0.0", "0.5", "0.5", "0.0");
        const double b1 = rms_at(100, MAR, "1.0", "0.5", "0.5", "0.0");
        const double bh = rms_at(4000, MAR, "1.0", "0.5", "0.5", "0.0")
                        - rms_at(4000, MAR, "0.0", "0.5", "0.5", "0.0");
        const double t0 = rms_at(4000, MAR, "0.5", "0.5", "0.0", "0.0");
        const double t1 = rms_at(4000, MAR, "0.5", "0.5", "1.0", "0.0");
        printf("stack: bass 0->1  %+.1f dB at 100Hz (4k %+.1f)   "
               "treb 0->1 %+.1f dB at 4k\n", b1 - b0, bh, t1 - t0);
        if (!(b1 - b0 > 8.0) || fabs(bh) > 2.0) {
            printf("   <-- Bass is not a bass control\n"); fails++;
        }
        if (!(t1 - t0 > 12.0)) {
            printf("   <-- Treble is not a treble control\n"); fails++;
        }

        /* 3. RAISING BASS FILLS THE MID SCOOP. In the circuit the two pots
         * share a leg; this is most of why a stack is not an EQ. */
        const double fill = rms_at(500, MAR, "1.0", "0.0", "0.5", "0.0")
                          - rms_at(500, MAR, "0.0", "0.0", "0.5", "0.0");
        printf("stack: bass fills the scoop %+.1f dB at 500Hz\n", fill);
        if (!(fill > 2.0)) {
            printf("   <-- the controls do not interact, so it is an EQ\n"); fails++;
        }

        /* 4. THE SCOOP SITS SOMEWHERE DIFFERENT PER AMP, which is most of
         * what tells a Fender from a Marshall. Measured where the FENDER
         * notch is: the Fender must cut harder there than the Modern,
         * whose scoop is higher up. */
        const double fen = rms_at(400, "0.0", "0.5", "0.0", "0.5", "0.0")
                         - rms_at(400, "0.0", "0.5", "0.5", "0.5", "0.0");
        const double mdn = rms_at(400, "1.0", "0.5", "0.0", "0.5", "0.0")
                         - rms_at(400, "1.0", "0.5", "0.5", "0.5", "0.0");
        printf("stack: notch at 400Hz  Fender %+.1f dB  Modern %+.1f dB\n",
               fen, mdn);
        if (!(fen < mdn - 1.5)) {
            printf("   <-- the four amps are the same filter\n"); fails++;
        }

        /* 5. PRESENCE IS ONE SIDED AND LIVES UP TOP. */
        const double p_hi = rms_at(4000, MAR, "0.5", "0.5", "0.5", "1.0") - noon_hi;
        const double p_md = rms_at( 500, MAR, "0.5", "0.5", "0.5", "1.0") - noon_mid;
        printf("stack: presence 0->1  %+.1f dB at 4k (500Hz %+.1f)\n", p_hi, p_md);
        if (!(p_hi > 3.0) || fabs(p_md) > 1.5) {
            printf("   <-- Presence is not a presence control\n"); fails++;
        }

        /* 6. NOON IS UNITY. The stack is passive and loses 10-20 dB; the
         * makeup is what the amp does after it, so a board does not drop
         * in level the moment this block is switched in. */
        api->set_param(in, "b1_on", "1");             /* bypass */
        const double byp = rms_at(500, MAR, "0.5", "0.5", "0.5", "0.0");
        api->set_param(in, "b1_on", "0");
        const double on  = rms_at(500, MAR, "0.5", "0.5", "0.5", "0.0");
        printf("stack: at noon  bypassed %+.1f dB  in %+.1f dB\n", byp, on);
        if (fabs(on - byp) > 2.0) {
            printf("   <-- switching it in at noon changes the level\n"); fails++;
        }
    }


    /* ------------------------------------------------------------------
     * THE POWER STAGE'S MEMORY.
     *
     * A .nam is a static function, so everything here is something it
     * CANNOT have rather than something it does badly. Each claim is
     * measured on the axis it is about: sag over TIME, bias over LEVEL.
     * ------------------------------------------------------------------ */
    {
        for (int b = 1; b <= 8; b++) {
            char k[16];
            snprintf(k, 16, "b%d_type", b); api->set_param(in, k, "0");
            snprintf(k, 16, "t%d_type", b); api->set_param(in, k, "0");
        }
        api->set_param(in, "merge", "0");
        api->set_param(in, "pan_a", "0.0");
        api->set_param(in, "pan_b", "0.0");
        api->set_param(in, "b1_type", "3");
        api->set_param(in, "b1_fx", "51");            /* Power Amp */
        int16_t a[128 * 2];

        /* RMS of one window of a held note, `skip` blocks after it starts.
         * Sag is a shape in time, so the only honest measurement of it is
         * two windows of the SAME note. */
        auto note = [&](double amp, const char *sag, const char *bloom,
                        const char *bias, int skip, int win) {
            api->set_param(in, "b1_p1", sag);
            api->set_param(in, "b1_p2", bloom);
            api->set_param(in, "b1_p3", bias);
            for (int blk = 0; blk < 300; blk++) {     /* let it recover */
                memset(a, 0, sizeof(a));
                api->process_block(in, a, 128);
            }
            double e = 0; int kept = 0;
            for (int blk = 0; blk < skip + win; blk++) {
                for (int i = 0; i < 128; i++) {
                    double t = (blk * 128.0 + i) / 44100.0;
                    a[i * 2] = a[i * 2 + 1] =
                        (int16_t)(amp * sin(2 * M_PI * 220.0 * t) * 20000);
                }
                api->process_block(in, a, 128);
                if (blk < skip) continue;
                for (int i = 0; i < 128; i++) {
                    double v = a[i * 2] / 32768.0;
                    e += v * v;
                }
                kept++;
            }
            return 20.0 * log10(sqrt(e / (kept * 128)) + 1e-12);
        };

        /* 1. A HELD LOUD NOTE DROOPS. Two windows of ONE note: its first
         * 17 ms against 200 ms in.
         *
         * From the ONSET, not from 30 ms in. The rail takes ~45 ms to
         * fall, so a window starting at 30 ms has already lost half the
         * droop it is there to measure - which read as the sag being
         * half the size it is. */
        const double on_0  = note(0.6, "0.0", "0.5", "0.0", 0, 6);
        const double lt_0  = note(0.6, "0.0", "0.5", "0.0", 70, 8);
        const double on_1  = note(0.6, "1.0", "0.5", "0.0", 0, 6);
        const double lt_1  = note(0.6, "1.0", "0.5", "0.0", 70, 8);
        printf("\npa: held note  sag 0 %+.2f dB over 200ms,  sag 10 %+.2f dB\n",
               lt_0 - on_0, lt_1 - on_1);
        if (fabs(lt_0 - on_0) > 0.5) {
            printf("   <-- it droops with Sag at zero\n"); fails++;
        }
        if (!(lt_1 - on_1 < -2.5)) {
            printf("   <-- a held note does not sag\n"); fails++;
        }

        /* 2. IT IS LEVEL DEPENDENT. A quiet note must barely move, or
         * this is a compressor wearing an amp's name. */
        const double q_on = note(0.05, "1.0", "0.5", "0.0", 0, 6);
        const double q_lt = note(0.05, "1.0", "0.5", "0.0", 70, 8);
        printf("pa: quiet note sag 10 %+.2f dB over the same 200ms\n",
               q_lt - q_on);
        if (!(fabs(q_lt - q_on) < fabs(lt_1 - on_1) * 0.4)) {
            printf("   <-- it ducks quiet playing as hard as loud\n"); fails++;
        }

        /* 3. BLOOM IS THE RECOVERY, so a long one is still down later. */
        const double fast = note(0.6, "1.0", "0.0", "0.0", 160, 8);
        const double slow = note(0.6, "1.0", "1.0", "0.0", 160, 8);
        printf("pa: at 460ms   bloom 50ms %+.2f dB  bloom 600ms %+.2f dB\n",
               fast, slow);
        if (!(slow < fast - 0.3)) {
            printf("   <-- Bloom does not change the recovery\n"); fails++;
        }

        /* 4. BIAS ADDS HARMONICS ON A LOUD NOTE AND NOT ON A QUIET ONE.
         * Measured as the energy that is NOT at the fundamental. */
        auto thd = [&](double amp, const char *bias) {
            api->set_param(in, "b1_p1", "0.0");
            api->set_param(in, "b1_p3", bias);
            for (int blk = 0; blk < 200; blk++) {
                memset(a, 0, sizeof(a));
                api->process_block(in, a, 128);
            }
            double re = 0, im = 0, tot = 0; long cnt = 0;
            for (int blk = 0; blk < 120; blk++) {
                for (int i = 0; i < 128; i++) {
                    double t = (blk * 128.0 + i) / 44100.0;
                    a[i * 2] = a[i * 2 + 1] =
                        (int16_t)(amp * sin(2 * M_PI * 110.0 * t) * 20000);
                }
                api->process_block(in, a, 128);
                if (blk < 40) continue;
                for (int i = 0; i < 128; i++) {
                    double t = (blk * 128.0 + i) / 44100.0;
                    double v = a[i * 2] / 32768.0;
                    re += v * cos(2 * M_PI * 110.0 * t);
                    im += v * sin(2 * M_PI * 110.0 * t);
                    tot += v * v; cnt++;
                }
            }
            double fund = 2.0 * sqrt(re * re + im * im) / cnt;
            double rms  = sqrt(tot / cnt);
            double h    = rms * rms - fund * fund / 2.0;
            return 100.0 * sqrt(h > 0 ? h : 0) / (rms + 1e-12);
        };
        const double d_loud_0 = thd(0.6,  "0.0"), d_loud_1 = thd(0.6,  "1.0");
        const double d_soft_1 = thd(0.05, "1.0");
        printf("pa: non-fundamental  loud bias 0 %.1f%% -> 10 %.1f%%   "
               "quiet bias 10 %.1f%%\n", d_loud_0, d_loud_1, d_soft_1);
        if (!(d_loud_1 > d_loud_0 + 3.0)) {
            printf("   <-- Bias adds nothing on a hard note\n"); fails++;
        }
        if (!(d_soft_1 < d_loud_1 * 0.5)) {
            printf("   <-- Bias spits on quiet playing too\n"); fails++;
        }
        api->set_param(in, "b1_p1", "0.0");
        api->set_param(in, "b1_p3", "0.0");
    }

    /* ------------------------------------------------------------------
     * SPEAKER DAMPING, on the cab.
     * ------------------------------------------------------------------ */
    {
        char list[1024] = {0};
        api->get_param(in, "cab_list", list, sizeof(list));
        if (!strstr(list, "(none)")) {
            for (int b = 1; b <= 8; b++) {
                char k[16];
                snprintf(k, 16, "b%d_type", b); api->set_param(in, k, "0");
            }
            api->set_param(in, "b1_type", "2");       /* BLK_CAB */
            for (int i = 0; i < 60; i++) { struct timespec t = {0, 20000000L}; nanosleep(&t, NULL); }
            int16_t a[128 * 2];
            auto at = [&](double hz, const char *damp) {
                api->set_param(in, "b1_p5", damp);
                double e = 0;
                for (int blk = 0; blk < 180; blk++) {
                    for (int i = 0; i < 128; i++) {
                        double t = (blk * 128.0 + i) / 44100.0;
                        a[i * 2] = a[i * 2 + 1] =
                            (int16_t)(0.4 * sin(2 * M_PI * hz * t) * 20000);
                    }
                    api->process_block(in, a, 128);
                    if (blk < 80) continue;
                    for (int i = 0; i < 128; i++) {
                        double v = a[i * 2] / 32768.0;
                        e += v * v;
                    }
                }
                return 20.0 * log10(sqrt(e / (100 * 128)) + 1e-12);
            };
            const double res  = at(95,   "1.0") - at(95,   "0.0");
            const double mid  = at(500,  "1.0") - at(500,  "0.0");
            const double top  = at(5000, "1.0") - at(5000, "0.0");
            api->set_param(in, "b1_p5", "0.0");
            printf("\ndamp: 0->10  %+.1f dB at the 95Hz resonance, "
                   "%+.1f at 500Hz, %+.1f at 5k\n", res, mid, top);
            if (!(res > 3.0)) {
                printf("   <-- the cabinet resonance does not come up\n"); fails++;
            }
            if (fabs(mid) > 1.5) {
                printf("   <-- it is a volume knob, not an impedance curve\n");
                fails++;
            }
            if (!(top > 1.0)) {
                printf("   <-- the voice coil rise is missing\n"); fails++;
            }
        }
    }

    /* ---------------------------------------------------- Tuning (52) */
    {
        for (int b = 1; b <= 8; b++) {
            char k[16];
            snprintf(k, 16, "b%d_type", b); api->set_param(in, k, "0");
            snprintf(k, 16, "t%d_type", b); api->set_param(in, k, "0");
        }
        api->set_param(in, "merge", "0");
        api->set_param(in, "pan_a", "0.0");
        api->set_param(in, "pan_b", "0.0");
        api->set_param(in, "b1_type", "3");

        /* THE DEFAULT IS E, and it is read before anything is written. An
         * enum of seven tunings is created at 0.5 like every other knob,
         * and 0.5 of seven is index 3 = C# standard - so without
         * fx_block_apply_defaults a Tuning block retunes the guitar three
         * semitones just by being dropped on the board. */
        api->set_param(in, "b1_fx", "52");
        char dflt[32] = {0};
        api->get_param(in, "b1_p1", dflt, sizeof(dflt));
        printf("\ntuning: default p1 = %s (E standard is 0.0)\n", dflt);
        if (atof(dflt) > 0.0005) {
            printf("   <-- a new Tuning block arrives already shifted\n");
            fails++;
        }

        /* 0.87 s of output. The window has to span at least one full wrap
         * of the 50 ms grain for every tuning offered: the fastest is C
         * standard, which wraps every 0.24 s, and a window shorter than
         * that leaves the modulation sidebands asymmetric. */
        static double rec[38400];
        int rec_n = 0;
        int16_t a[128 * 2];

        auto run = [&](const char *tune, const char *fine, const char *mix,
                       double fin) {
            api->set_param(in, "b1_p1", tune);
            api->set_param(in, "b1_p2", fine);
            api->set_param(in, "b1_p3", mix);
            rec_n = 0;
            double ph = 0.0;
            const double inc = 2.0 * M_PI * fin / 44100.0;
            for (int blk = 0; blk < 400; blk++) {
                for (int i = 0; i < 128; i++) {
                    const double v = 0.5 * sin(ph);
                    ph += inc;
                    a[i * 2] = a[i * 2 + 1] = (int16_t)(v * 32767.0);
                }
                api->process_block(in, a, 128);
                /* The first 100 blocks go in the bin: the read pointer
                 * starts 12 ms behind the write and the fade-in is 30 ms,
                 * so what comes out before that is the onset. */
                if (blk >= 100)
                    for (int i = 0; i < 128; i++)
                        rec[rec_n++] = a[i * 2] / 32768.0;
            }
        };
        /* One DFT bin, in dB. */
        auto bin = [&](double f) {
            double re = 0.0, im = 0.0;
            const double w = 2.0 * M_PI * f / 44100.0;
            for (int i = 0; i < rec_n; i++) {
                re += rec[i] * cos(w * i);
                im += rec[i] * sin(w * i);
            }
            return sqrt(re * re + im * im) / rec_n;
        };
        /* WHERE THE NOTE IS, as the energy CENTROID of the band.
         *
         * Asking bin(expected) vs bin(original) passed a shifter running
         * the wrong DIRECTION - the wrong answer sat nearer the expected
         * bin than the original did. Scanning for the PEAK then reported
         * every large shift off by exactly one grain-wrap sideband,
         * because in a short window a sideband can out-rank the carrier.
         * The sidebands are symmetric about the carrier, so the centroid
         * is immune to both mistakes. */
        auto centroid = [&](double lo, double hi) {
            double num = 0.0, den = 0.0;
            for (double f = lo; f <= hi; f += 0.5) {
                const double amp = bin(f);
                const double pw = amp * amp;
                num += pw * f;
                den += pw;
            }
            return den > 0.0 ? num / den : 0.0;
        };

        struct { const char *p1; const char *name; double semi; } cases[] = {
            { "0.0000", "E ", 0.0 }, { "0.1667", "Eb", -1.0 },
            { "0.3333", "D ", -2.0 }, { "0.5000", "C#", -3.0 },
            { "0.6667", "C ", -4.0 }, { "0.8333", "F ", +1.0 },
            { "1.0000", "F#", +2.0 },
        };
        printf("tuning: A3 220Hz in, Mix 100, centroid of 150-300Hz --\n");
        for (int c = 0; c < 7; c++) {
            run(cases[c].p1, "0.5", "1.0", 220.0);
            const double want = 220.0 * pow(2.0, cases[c].semi / 12.0);
            const double got = centroid(150.0, 300.0);
            const double cents = 1200.0 * log2(got / want);
            printf("   %s  want %6.2f Hz   got %6.2f Hz   %+5.0f cents\n",
                   cases[c].name, want, got, cents);
            if (fabs(cents) > 20.0) {
                printf("      <-- not this tuning\n"); fails++;
            }
        }

        /* Fine is CENTS: -50 at Eb lands a quarter-tone below it, which is
         * twice the gate above, so it cannot pass by accident. */
        run("0.1667", "0.0", "1.0", 220.0);
        {
            const double want = 220.0 * pow(2.0, (-1.0 - 0.5) / 12.0);
            const double got = centroid(150.0, 300.0);
            const double cents = 1200.0 * log2(got / want);
            printf("tuning: Eb with Fine -50  want %.2f Hz  got %.2f Hz "
                   "(%+.0f cents)\n", want, got, cents);
            if (fabs(cents) > 20.0) {
                printf("   <-- Fine does not move the pitch\n"); fails++;
            }
        }

        /* THERE IS NO MIX. A retune is not a blend: below 100 the old
         * tuning is still sounding underneath, which is a detune wearing a
         * tuning's name - and Detune is a separate pedal for exactly that.
         * p3 is a HOLE at its own index (the wire key is the index, so it
         * cannot be compacted away), and the DSP must not read it. Driven
         * to both ends, because a knob that has merely been hidden from
         * the screen still moves the audio. */
        /* Measured as a PITCH, not as a level. A first version compared
         * the energy at 196 Hz between the two ends of p3 and read a 24%
         * difference as p3 still working - it is the grain wrap, which
         * modulates the amplitude depending on where in its cycle the
         * window happens to land. The centroid does not care: if any dry
         * survived, it would pull toward 220. */
        run("0.3333", "0.5", "0.0", 220.0);
        const double hole_lo = centroid(150.0, 300.0);
        run("0.3333", "0.5", "1.0", 220.0);
        const double hole_hi = centroid(150.0, 300.0);
        printf("tuning: p3 at 0 -> %.2f Hz   at 1 -> %.2f Hz  (want 196.00 "
               "both)\n", hole_lo, hole_hi);
        for (int e = 0; e < 2; e++) {
            const double got = e ? hole_hi : hole_lo;
            if (fabs(1200.0 * log2(got / 196.00)) > 20.0) {
                printf("   <-- p3 at %d still mixes the old pitch back in\n", e);
                fails++;
            }
        }

        /* E STANDARD IS A BYPASS, measured against the block being OFF
         * rather than against the raw input - in_level and out_level are
         * applied either way, so comparing to the input would fail on a
         * perfect bypass and tells you nothing about this pedal. */
        static double off_rec[12800];
        int off_n = 0;
        auto record_into = [&](double *dst, int *n, int limit) {
            *n = 0;
            double ph = 0.0;
            for (int blk = 0; blk < 200; blk++) {
                for (int i = 0; i < 128; i++) {
                    const double v = 0.5 * sin(ph);
                    ph += 2.0 * M_PI * 220.0 / 44100.0;
                    a[i * 2] = a[i * 2 + 1] = (int16_t)(v * 32767.0);
                }
                api->process_block(in, a, 128);
                if (blk >= 100)
                    for (int i = 0; i < 128 && *n < limit; i++)
                        dst[(*n)++] = a[i * 2];
            }
        };
        api->set_param(in, "b1_type", "0");
        record_into(off_rec, &off_n, 12800);
        api->set_param(in, "b1_type", "3");
        api->set_param(in, "b1_fx", "52");
        api->set_param(in, "b1_p1", "0.0");
        api->set_param(in, "b1_p3", "1.0");
        rec_n = 0;
        record_into(rec, &rec_n, 12800);
        int diff = 0;
        for (int i = 0; i < off_n && i < rec_n; i++)
            if (off_rec[i] != rec[i]) diff++;
        printf("tuning: E standard vs block OFF  %d of %d samples differ\n",
               diff, off_n);
        if (diff != 0) {
            printf("   <-- E standard is not a bypass\n"); fails++;
        }

        /* And the line KEPT RUNNING through that bypass, so leaving E does
         * not replay the 60 ms that were in it when the knob last passed.
         * (fx_block_reset zeroes the line, but only on a PEDAL change.) */
        api->set_param(in, "b1_p1", "0.3333");
        double worst = 0.0, ph2 = 0.0;
        for (int i = 0; i < 128; i++) {
            const double v = 0.5 * sin(ph2);
            ph2 += 2.0 * M_PI * 220.0 / 44100.0;
            a[i * 2] = a[i * 2 + 1] = (int16_t)(v * 32767.0);
        }
        api->process_block(in, a, 128);
        for (int i = 0; i < 128; i++) {
            const double d = fabs(a[i * 2] / 32768.0);
            if (d > worst) worst = d;
        }
        printf("tuning: first block after leaving E  peak %.3f\n", worst);
        if (!(worst > 0.05)) {
            printf("   <-- the line was empty: E returned without priming\n");
            fails++;
        }
        api->set_param(in, "b1_type", "0");
    }

    /* ----------------------------------------------------------- tuner */
    /* A SINE WOULD PROVE NOTHING. The failure a tuner has is the OCTAVE
     * ERROR: a plucked low E has a second harmonic that is often louder
     * than its fundamental, so a detector taking the biggest correlation
     * peak answers E3 for an E2 with full confidence and a centred needle.
     * So every case below is a harmonic stack with the fundamental
     * DELIBERATELY WEAKER than its partials, which is the shape that
     * breaks the naive version. */
    {
        for (int b = 1; b <= 16; b++) {
            char k[16];
            snprintf(k, 16, "b%d_type", b); api->set_param(in, k, "0");
            snprintf(k, 16, "t%d_type", b); api->set_param(in, k, "0");
        }
        api->set_param(in, "merge", "0");
        int16_t a[128 * 2];
        char q[256];

        /* Plays `f0` for `blocks` blocks with the given partial weights and
         * returns what the tuner says. The result is READ every block,
         * because the read is what arms it. */
        auto hear = [&](double f0, const double *amp, int nh, int blocks) {
            double ph = 0.0;
            for (int blk = 0; blk < blocks; blk++) {
                for (int i = 0; i < 128; i++) {
                    double v = 0.0;
                    for (int h = 1; h <= nh; h++) v += amp[h - 1] * sin(ph * h);
                    ph += 2.0 * M_PI * f0 / 44100.0;
                    const double c = v * 0.22;
                    const int16_t sv = (int16_t)((c > 0.98 ? 0.98 :
                                                  c < -0.98 ? -0.98 : c) * 32767.0);
                    a[i * 2] = a[i * 2 + 1] = sv;
                }
                api->process_block(in, a, 128);
                q[0] = 0;
                api->get_param(in, "tuner", q, sizeof(q));
            }
            return std::string(q);
        };
        auto field = [](const std::string &j, const char *key) {
            const std::string k = std::string("\"") + key + "\":";
            const size_t at = j.find(k);
            if (at == std::string::npos) return 0.0;
            return atof(j.c_str() + at + k.size());
        };
        auto name = [](const std::string &j) {
            const size_t at = j.find("\"note\":\"");
            if (at == std::string::npos) return std::string("-");
            const size_t b2 = at + 8, e = j.find('"', b2);
            return j.substr(b2, e - b2);
        };

        /* The fundamental at a THIRD of the second harmonic. */
        const double weak[6] = { 0.33, 1.00, 0.55, 0.40, 0.22, 0.14 };
        /* THE LOWEST STRING ANYONE WILL POINT AT IT, not a 6-string's E2.
         * The first three were out of range entirely while a comment in
         * the source claimed 37.8 Hz was below a 5-string bass's low B -
         * B0 is 30.87, so it was above it, and the 7-string, 8-string and
         * 5-string bass all read nothing. */
        /* CHROMATIC, so the cases are the TUNINGS people use and the
         * pitches they land on, not six standard strings.
         *
         * THE OCTAVES HERE WERE WRONG and the detector was right: a
         * 7-string's low B is B1 61.74 and an 8-string's low F# is F#1
         * 46.25 - both always inside the range - while 30.87 and 23.12,
         * written here as B1 and F#1, are B0 and F#0, a 5- and 6-string
         * BASS. The test reported "named the wrong note" for two rows
         * where the only wrong name was in the expectation. */
        struct { double hz; const char *note; int oct; } open6[] = {
            {  23.12, "F#", 0 },      /* 6-string bass F#0           */
            {  30.87, "B",  0 },      /* 5-string bass B0            */
            {  41.20, "E",  1 },      /* 4-string bass / 8str drop E */
            {  46.25, "F#", 1 },      /* 8-string low F#             */
            {  55.00, "A",  1 },      /* drop A                      */
            {  61.74, "B",  1 },      /* 7-string low B / drop B     */
            {  65.41, "C",  2 },      /* drop C                      */
            {  73.42, "D",  2 },      /* drop D                      */
            {  82.41, "E", 2 }, { 110.00, "A", 2 }, { 146.83, "D", 3 },
            { 196.00, "G", 3 }, { 246.94, "B", 3 }, { 329.63, "E", 4 },
        };
        const int NOPEN = (int)(sizeof(open6) / sizeof(open6[0]));
        printf("\ntuner: drop and extended-range lows, fundamental 10 dB "
               "UNDER the 2nd\n");
        for (int c = 0; c < NOPEN; c++) {
            /* 220 blocks, not 90: the coarse window is 186 ms and the
             * cycle is 26 frames, so a shorter run measures a ring that
             * is still filling - which answers confident nonsense and
             * would have read as the detector being wrong. */
            const std::string j = hear(open6[c].hz, weak, 6, 260);
            const double hz = field(j, "hz");
            const double cents = field(j, "cents");
            const double conf = field(j, "conf");
            const std::string nm = name(j);
            printf("   %6.2f Hz -> %6.2f Hz  %-3s%d  %+5.1f c  conf %.2f\n",
                   open6[c].hz, hz, nm.c_str(), (int)field(j, "oct"),
                   cents, conf);
            if (hz <= 0.0) { printf("      <-- no note found\n"); fails++; continue; }
            /* THE OCTAVE IS THE TEST. A ratio near 2 or 0.5 is the failure
             * this whole two-stage design exists to avoid, and it would
             * otherwise pass a cents check with flying colours. */
            const double ratio = hz / open6[c].hz;
            if (ratio > 1.5 || ratio < 0.67) {
                printf("      <-- OCTAVE ERROR (ratio %.3f)\n", ratio); fails++;
                continue;
            }
            if (nm != open6[c].note || (int)field(j, "oct") != open6[c].oct) {
                printf("      <-- named the wrong note\n"); fails++;
            }
            /* Two stages and a parabolic vertex are here to buy this. One
             * decimated sample is 21 cents at the top of the range, so a
             * gate this tight is what says the fine stage ran.
             *
             * THREE cents, which is above the measured spread rather than
             * on it. Across these fourteen pitches the worst is 2.1 (F#1,
             * where the true period falls between two samples and the
             * parabolic vertex is a sample out), and a gate at 2.0 sat
             * exactly there - a test that fails on the detector working
             * as well as it does. It is not slack for its own sake: the
             * same gate at 5 missed the fine window being too short at
             * the bottom, which moved the SAME tone between -3.0 and
             * +4.8 cents.
             *
             * Not chased further on purpose. 2 cents is inaudible on a
             * guitar - its own fret-to-fret intonation is several times
             * that - and the thing this gate is defending is the fine
             * stage running at all, against the 21 cents a single
             * decimated sample would give at the top of the range. */
            if (fabs(1200.0 * log2(hz / open6[c].hz)) > 3.0) {
                printf("      <-- off by more than 3 cents\n"); fails++;
            }
        }

        /* A STRING THAT IS OUT must read out, with the right sign. */
        printf("tuner: an A2 pulled 30 cents flat and 30 sharp\n");
        for (int sgn = -1; sgn <= 1; sgn += 2) {
            const double want = 30.0 * sgn;
            const double f = 110.0 * pow(2.0, want / 1200.0);
            const std::string j = hear(f, weak, 6, 260);
            const double cents = field(j, "cents");
            printf("   %+3.0f c -> %+5.1f c  (%s)\n", want, cents,
                   name(j).c_str());
            if (fabs(cents - want) > 3.0) {
                printf("      <-- the needle disagrees with the input\n");
                fails++;
            }
        }

        /* ALL TWELVE, which is what chromatic means and what the rows
         * above do not cover: the tunings people use land on eight of the
         * pitch classes, so a naming bug in the other four would ship.
         * One octave, every semitone. */
        printf("tuner: all twelve pitch classes in one octave\n");
        {
            static const char *PC[12] = { "C", "C#", "D", "D#", "E", "F",
                                          "F#", "G", "G#", "A", "A#", "B" };
            int bad = 0;
            for (int k = 0; k < 12; k++) {
                /* C3..B3 = MIDI 48..59, all comfortably inside the range. */
                const double f = 440.0 * pow(2.0, (48 + k - 69) / 12.0);
                const std::string j = hear(f, weak, 6, 260);
                const std::string nm = name(j);
                const double cents = field(j, "cents");
                if (nm != PC[k] || (int)field(j, "oct") != 3 ||
                    fabs(cents) > 3.0) {
                    printf("   %-2s at %7.2f Hz -> %s%d %+.1f c  <-- wrong\n",
                           PC[k], f, nm.c_str(), (int)field(j, "oct"), cents);
                    bad++;
                }
            }
            if (bad) fails++;
            else printf("   all 12 named correctly, within 3 cents\n");
        }

        /* SILENCE IS NOT A NOTE. The gate has to hold, or the needle
         * wanders while nothing is playing and the screen looks broken. */
        for (int blk = 0; blk < 120; blk++) {
            memset(a, 0, sizeof(a));
            api->process_block(in, a, 128);
            q[0] = 0;
            api->get_param(in, "tuner", q, sizeof(q));
        }
        printf("tuner: silence -> %s\n", q);
        if (field(std::string(q), "hz") != 0.0) {
            printf("   <-- a note was reported with no input\n"); fails++;
        }

        /* IT COSTS NOTHING WHEN NOBODY IS LOOKING. The arm is the read, so
         * a run of blocks with no read must leave the result where it was
         * rather than keep analysing. */
        const std::string before = hear(110.0, weak, 6, 260);
        double ph = 0.0;
        for (int blk = 0; blk < 300; blk++) {       /* no get_param */
            for (int i = 0; i < 128; i++) {
                const double v = 0.3 * sin(ph);
                ph += 2.0 * M_PI * 440.0 / 44100.0;
                a[i * 2] = a[i * 2 + 1] = (int16_t)(v * 32767.0);
            }
            api->process_block(in, a, 128);
        }
        q[0] = 0;
        api->get_param(in, "tuner", q, sizeof(q));
        printf("tuner: 300 unread blocks of 440 Hz -> %.2f Hz "
               "(was %.2f, still A2's)\n", field(std::string(q), "hz"),
               field(before, "hz"));
        if (fabs(field(std::string(q), "hz") - field(before, "hz")) > 1.0) {
            printf("   <-- it kept analysing with nothing reading it\n");
            fails++;
        }
    }

    api->destroy_instance(in);
    printf("\n%s\n", fails ? "FAILED" : "PASS");
    return fails ? 1 : 0;
}
