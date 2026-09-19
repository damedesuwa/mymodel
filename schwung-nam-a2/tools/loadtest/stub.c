/*
 * Nam A2 load-path stub - NOT the real module.
 *
 * Ships under the same id (nam-a2) and the same .so name, so it drops into
 * the slot the real module already occupies and is selected the same way.
 * It has no dependencies beyond libc: no C++, no NeuralAudio, no static
 * libstdc++, ~20 KB instead of 4.8 MB.
 *
 * Its whole job is to answer one question. The real module's diagnostics
 * produced no line at all on device, which means its code never ran - but
 * that could be dlopen failing on the binary itself (size, the statically
 * linked C++ runtime, a symbol the device cannot resolve) or the host never
 * attempting the load at all. This stub can only fail the second way.
 *
 *   [NAMA2-STUB] lines appear  -> the host loads and runs modules fine;
 *                                 the fault is specific to the real binary
 *   still nothing               -> the load is never attempted, and the
 *                                 fault is upstream of the plugin entirely
 *
 * Writes straight to debug.log because the device has no way to create the
 * host's debug_log_on flag (no SSH, no file browser in its Manager build).
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>
#include <stdarg.h>

#include "plugin_api_v1.h"

#define AUDIO_FX_API_VERSION_2 2

typedef struct audio_fx_api_v2 {
    uint32_t api_version;
    void* (*create_instance)(const char *module_dir, const char *config_json);
    void (*destroy_instance)(void *instance);
    void (*process_block)(void *instance, int16_t *audio_inout, int frames);
    void (*set_param)(void *instance, const char *key, const char *val);
    int (*get_param)(void *instance, const char *key, char *buf, int buf_len);
    void (*on_midi)(void *instance, const uint8_t *msg, int len, int source);
} audio_fx_api_v2_t;

static void diag(const char *fmt, ...) {
    FILE *f = fopen("/data/UserData/schwung/debug.log", "a");
    if (!f) return;
    time_t t = time(NULL);
    struct tm tmv;
    localtime_r(&t, &tmv);
    fprintf(f, "%02d:%02d:%02d [NAMA2-STUB] ", tmv.tm_hour, tmv.tm_min, tmv.tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

/* Bring the host's own logging up too, so the next report carries its
 * messages as well as ours. */
static void enable_host_log(void) {
    if (access("/data/UserData/schwung/debug_log_on", F_OK) == 0) return;
    FILE *f = fopen("/data/UserData/schwung/debug_log_on", "w");
    if (f) { fputc('1', f); fclose(f); }
}

/* A constructor runs at dlopen time, before the host calls anything - so a
 * line from here separates "the library loaded" from "the host called us". */
__attribute__((constructor))
static void stub_ctor(void) {
    enable_host_log();
    diag("shared object loaded (constructor ran)");
}

typedef struct { int dummy; } stub_instance_t;

static void *v2_create_instance(const char *module_dir, const char *config_json) {
    (void)config_json;
    diag("create_instance, module_dir=%s", module_dir ? module_dir : "(null)");
    stub_instance_t *inst = calloc(1, sizeof(stub_instance_t));
    diag("create_instance returning %p", (void *)inst);
    return inst;
}

static void v2_destroy_instance(void *instance) {
    diag("destroy_instance");
    free(instance);
}

static void v2_process_block(void *instance, int16_t *audio_inout, int frames) {
    (void)instance; (void)audio_inout;
    static int first = 1;
    if (first) { first = 0; diag("first process_block (frames=%d)", frames); }
    /* pass audio through untouched */
}

static void v2_set_param(void *instance, const char *key, const char *val) {
    (void)instance;
    diag("set_param %s=%s", key ? key : "(null)", val ? val : "(null)");
}

static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {
    (void)instance;
    if (!key || !buf) return -1;

    if (strcmp(key, "state") == 0) return snprintf(buf, buf_len, "{}");
    if (strcmp(key, "ui_hierarchy") == 0) {
        return snprintf(buf, buf_len,
            "{\"modes\":null,\"levels\":{\"root\":{\"label\":\"Nam A2 STUB\","
            "\"children\":null,\"knobs\":[],\"params\":[]}}}");
    }
    return -1;
}

static audio_fx_api_v2_t g_api;

audio_fx_api_v2_t *move_audio_fx_init_v2(const host_api_v1_t *host) {
    diag("move_audio_fx_init_v2 called by host (host=%p)", (const void *)host);

    memset(&g_api, 0, sizeof(g_api));
    g_api.api_version      = AUDIO_FX_API_VERSION_2;
    g_api.create_instance  = v2_create_instance;
    g_api.destroy_instance = v2_destroy_instance;
    g_api.process_block    = v2_process_block;
    g_api.set_param        = v2_set_param;
    g_api.get_param        = v2_get_param;
    g_api.on_midi          = NULL;

    if (host && host->log) host->log("Nam A2 STUB: initialized");
    return &g_api;
}
