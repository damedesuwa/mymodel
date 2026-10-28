#!/usr/bin/env bash
# SWAP A CAB WHILE THE CALLBACK IS CONVOLVING, UNDER ThreadSanitizer.
#
# This exists because of a bug no other test in the suite could see. The IR
# loader ran on the worker thread and published with three plain stores -
#
#     float *old_ir = b->ir, *old_hist = b->history;
#     b->ir = new_ir;  b->len = want;  b->history = new_hist;
#     free(old_ir);    free(old_hist);
#
# - while the SPI callback was reading exactly those three to convolve, and
# then freed what it had replaced. A torn read gives the new taps with the
# old length, and `history` is sized `len-1 + FRAMES_PER_BLOCK`, so that is
# an out-of-bounds WRITE; the free is a use-after-free on the audio thread.
# It shipped for months as "the sound jumps when you change that block".
#
# Every other test swaps and THEN measures, so none of them has both threads
# in flight at once. TSan does, and it found a second instance of the same
# fault the same run (mic_reset_state, also worker-side, also read by the
# callback) that reading the code had missed.
set -e
cd "$(dirname "$0")/.."

if ! echo 'int main(){}' | g++ -fsanitize=thread -x c++ - -o /dev/null 2>/dev/null; then
    echo "skip: this toolchain has no ThreadSanitizer"
    exit 0
fi

echo "--- building with -fsanitize=thread ---"
g++ -std=c++20 -O1 -g -w -fsanitize=thread -o /tmp/a2c_test_race \
    tests/test_race.cpp src/dsp/nam_a2c_plugin.cpp \
    -Isrc/dsp -Itests/stub -lpthread -lm

FIX=$(mktemp -d)
trap 'rm -rf "$FIX"' EXIT
for d in amps od dist fuzz boost outboard cabs reverbs; do mkdir -p "$FIX/$d"; done
# A REAL IR FILE, or ir_block_load returns before it touches anything and
# the race cannot happen. The first run of this was clean for exactly that
# reason and it read as the fix working.
cp "src/cabs/TF MESA ST 4X12A V30 57.wav" "$FIX/cabs/" 2>/dev/null || {
    echo "skip: no bundled cab IR to load"; exit 0; }
cp "$FIX/cabs/TF MESA ST 4X12A V30 57.wav" "$FIX/cabs/second.wav"

echo "--- running ---"
OUT=$(/tmp/a2c_test_race "$FIX" 2>&1 || true)
echo "$OUT" | grep -E "race driver finished" || { echo "FAIL: driver did not finish"; exit 1; }

# THE TWO THAT ARE DELIBERATE, each named with why. Anything else is a
# finding: a suppression list that does not say why is how a real race gets
# filed under "known noise".
#
#   worker_thread      the superseded-load check reads `index`, which RT
#                      writes. A plain aligned int; the worst case is one
#                      load run or skipped, and the ring stays
#                      single-producer. Documented at the call site.
#   v2_destroy_instance the stop flags are `volatile int`, which is not a
#                      synchronisation primitive, so TSan flags the join
#                      handshake at teardown. Shutdown only.
#   diag_thread        peak meters read for a log line, zeroed every 2 s.
#   v2_process_block   the other half of those same meters.
BAD=$(echo "$OUT" | grep "^SUMMARY: ThreadSanitizer" | sort -u \
      | grep -vE "in (worker_thread|v2_destroy_instance|diag_thread|v2_process_block)$" \
      || true)
if [ -n "$BAD" ]; then
    echo "FAIL: a data race on the audio path:"
    echo "$BAD"
    exit 1
fi
echo "ok   no data race on the audio path"
echo "PASS"
