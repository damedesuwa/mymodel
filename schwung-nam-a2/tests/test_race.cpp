/* SWAP CABS WHILE PROCESSING, under ThreadSanitizer.
 *
 * The IR loader ran on the worker and wrote b->ir / b->len / b->history
 * with three plain stores, then freed what it replaced - while the SPI
 * callback was convolving over exactly those. No test in the suite could
 * see it: they all swap and then measure, never both at once. TSan can.
 */
#include <cstdio>
#include <cstring>
#include <cmath>
#include <cstdlib>
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
extern "C" audio_fx_api_v2_t* move_audio_fx_init_v2(const host_api_v1_t *);
int main(int argc, char **argv) {
    auto api = move_audio_fx_init_v2(NULL);
    void *in = api->create_instance(argv[1], NULL);
    api->set_param(in, "in_level", "0.5");
    api->set_param(in, "out_level", "0.85");
    api->set_param(in, "b1_type", "2");            /* Cab */
    int16_t a[256];
    double ph = 0.0;
    /* Keep the callback busy and keep asking for a reload. The Len knob is
     * what the Reverb list walks; `cab` is what the Cab list walks. */
    for (int round = 0; round < 400; round++) {
        api->set_param(in, "b1_cab", "0");
        for (int b = 0; b < 6; b++) {
            for (int i = 0; i < 128; i++) {
                const double v = 0.2 * sin(ph);
                ph += 2.0 * M_PI * 220.0 / 44100.0;
                a[i*2] = a[i*2+1] = (int16_t)(v * 32767.0);
            }
            api->process_block(in, a, 128);
        }
        /* -1 then back makes the next write a CHANGE, so every round
         * actually queues a load rather than being deduped away. */
        api->set_param(in, "b1_type", "0");
        api->set_param(in, "b1_type", "2");
    }
    api->destroy_instance(in);
    printf("race driver finished\n");
    return 0;
}
