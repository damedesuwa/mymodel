/* Instantiate the plugin and print what it serves, so the JSON can be parsed
 * by something other than the device. */
#include <cstdio>
#include <cstdlib>
#include <cstring>

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

static void host_log(const char *m) { fprintf(stderr, "[log] %s\n", m); }

int main(int argc, char **argv) {
    host_api_v1_t host;
    memset(&host, 0, sizeof(host));
    host.log = host_log;

    audio_fx_api_v2_t *api = move_audio_fx_init_v2(&host);
    void *inst = api->create_instance(argc > 1 ? argv[1] : ".", "{}");
    if (!inst) { fprintf(stderr, "create_instance failed\n"); return 1; }

    static char buf[262144];
    const char *keys[] = { "chain_params", "ui_hierarchy", "state" };
    for (unsigned i = 0; i < sizeof(keys) / sizeof(keys[0]); i++) {
        int n = api->get_param(inst, keys[i], buf, sizeof(buf));
        printf("===%s===\n", keys[i]);
        if (n < 0) printf("(unserved)\n"); else printf("%s\n", buf);
    }

    /* A block put into each type, then read back - the round trip the grid
     * makes, with the writes the grid would make. */
    api->set_param(inst, "b3_type", "3");
    api->set_param(inst, "b3_drive", "0.75");
    api->set_param(inst, "b3_on", "1");
    const char *rb[] = { "b3_type", "b3_drive", "b3_on", "b1_type", "cpu",
                         "name", "is_loading", "sel_block" };
    printf("===readback===\n");
    for (unsigned i = 0; i < sizeof(rb) / sizeof(rb[0]); i++) {
        int n = api->get_param(inst, rb[i], buf, sizeof(buf));
        printf("%-12s %s\n", rb[i], n < 0 ? "(unserved)" : buf);
    }

    /* Run audio so the block costs and the meters are exercised. */
    int16_t audio[128 * 2];
    for (int i = 0; i < 128 * 2; i++) audio[i] = (int16_t)(8000 * ((i % 32) - 16) / 16);
    for (int k = 0; k < 400; k++) api->process_block(inst, audio, 128);
    int n = api->get_param(inst, "cpu", buf, sizeof(buf));
    printf("cpu after 400 blocks: %s\n", n < 0 ? "(unserved)" : buf);

    api->destroy_instance(inst);
    return 0;
}
