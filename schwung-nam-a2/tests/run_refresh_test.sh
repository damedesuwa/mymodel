#!/usr/bin/env bash
# The library refresh: that it happens, and that it happens OFF the audio
# callback.
#
# Two halves, because neither covers the other. The functional half drops a
# file into a fixture while process_block is running and checks the picker
# and the per-block rebind. The source half pins the thing that cannot be
# observed from outside: every entry point of this plugin IS the SPI
# callback, so a directory read reachable from one of them is a realtime
# violation whether or not any test notices the timing.
set -e
cd "$(dirname "$0")/.."

echo "--- source: no directory read from any entry point ---"
python3 - <<'PY'
import re, sys, io
src = io.open('src/dsp/nam_a2c_plugin.cpp', encoding='utf-8').read()

def strip_comments(t):
    # A CHECK THAT READS COMMENTS FINDS ITS OWN PROSE. The first version of
    # this failed on the comment that explains the fix - "opendir/readdir on
    # the SPI callback" - which is exactly the sentence a future reader needs
    # to keep.
    out, i, n = [], 0, len(t)
    while i < n:
        if t.startswith('/*', i):
            j = t.find('*/', i + 2)
            i = n if j < 0 else j + 2
        elif t.startswith('//', i):
            j = t.find(chr(10), i)
            i = n if j < 0 else j
        else:
            out.append(t[i]); i += 1
    return ''.join(out)

def body_of(sig):
    i = src.index(sig)
    j = src.index('{', i); d = 0
    while True:
        if src[j] == '{': d += 1
        elif src[j] == '}':
            d -= 1
            if d == 0: return src[i:j+1]
        j += 1

bad = 0
# The three the chain host calls on the SPI callback. create_instance is
# deliberately NOT here: it reads the card once, before anything can ask
# for a list, and the user is waiting on the load either way.
for sig in ('static void v2_set_param(void *instance, const char *key, const char *val) {',
            'static int v2_get_param(void *instance, const char *key, char *buf, int buf_len) {',
            'static void v2_process_block(void *instance, int16_t *audio_inout, int frames) {'):
    b = strip_comments(body_of(sig))
    name = sig.split('(')[0].split()[-1]
    for call in ('scan_lists_into', 'scan_directory', 'opendir', 'readdir',
                 'stat(', 'assets_dir_stamp', 'refresh_lists', 'fopen'):
        if call in b:
            print("FAIL %s reaches %s" % (name, call)); bad += 1
    print("ok   %s is clean" % name)

# And the publish has to be a store, not a copy into the live buffer.
if 'lists_active.store' not in src:
    print("FAIL nothing publishes a list buffer"); bad += 1
else:
    print("ok   the flip is an atomic store")
sys.exit(1 if bad else 0)
PY

echo "--- functional: a file lands while audio runs ---"
g++ -std=c++20 -O1 -w -o /tmp/a2c_test_refresh \
    tests/test_refresh.cpp src/dsp/nam_a2c_plugin.cpp \
    -Isrc/dsp -Itests/stub -lpthread -lm

FIX=$(mktemp -d)
trap 'rm -rf "$FIX"' EXIT
for d in amps od dist fuzz boost outboard cabs reverbs; do mkdir -p "$FIX/$d"; done
printf '{}' > "$FIX/amps/M2.nam"

/tmp/a2c_test_refresh "$FIX"
