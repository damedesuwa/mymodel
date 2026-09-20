#!/usr/bin/env bash
# Compile the plugin for the BUILD HOST against a stub NeuralAudio, run it,
# and parse what it serves.
#
# It exists because of a specific failure this module shipped four times:
# chain_params and ui_hierarchy are built by C code at runtime, and a
# malformed one is not a build error. chain_host quietly rejects it and falls
# back to module.json, which drops `access` and `live` on the way through -
# so the CPU readout came back as an ordinary knob sitting at its default,
# with nothing logged anywhere. A trailing comma did that.
#
# Nothing here needs the device, the cross-compiler or a model.
set -e
cd "$(dirname "$0")/.."

echo "--- compiling host harness ---"
g++ -std=c++20 -O1 -w -o /tmp/a2c_dump_contract \
    tests/dump_contract.cpp src/dsp/nam_a2c_plugin.cpp \
    -Isrc/dsp -Itests/stub -lpthread -lm

FIX=$(mktemp -d)
trap 'rm -rf "$FIX"' EXIT
mkdir -p "$FIX/models" "$FIX/cabs"
[ -d src/models ] && cp src/models/* "$FIX/models/" 2>/dev/null || true
[ -d src/cabs ]   && cp src/cabs/*   "$FIX/cabs/"   2>/dev/null || true

echo "--- running ---"
/tmp/a2c_dump_contract "$FIX" > /tmp/a2c_contract.txt

python3 - <<'PY'
import json, re, sys
txt = open('/tmp/a2c_contract.txt').read()
fail = 0

def section(name):
    m = re.search(r'===%s===\n(.*?)(?=\n===|\Z)' % name, txt, re.S)
    return m.group(1).strip() if m else None

for name in ('chain_params', 'state'):
    raw = section(name)
    if raw is None:
        print("FAIL %s: not served" % name); fail = 1; continue
    try:
        obj = json.loads(raw)
    except Exception as e:
        print("FAIL %s: invalid JSON - %s" % (name, e))
        print("     ...%s..." % raw[max(0, getattr(e, 'pos', 0) - 60):getattr(e, 'pos', 0) + 60])
        fail = 1
        continue
    print("ok   %s (%d bytes)" % (name, len(raw)))

cp = json.loads(section('chain_params'))
keys = {p['key'] for p in cp}

# NO HIERARCHY, ON PURPOSE - and that has to stay true.
#
# enterComponentEdit asks for a ui_hierarchy first and, if it gets one,
# opens the host grid and RETURNS, so loadModuleUi never runs and the pads
# and the module's own screen both silently do not exist. That is exactly
# how 0.2.0 shipped. Both sources have to stay quiet: module.json (which
# fills the FX cache) and the plugin (which the empty cache falls through
# to).
mj = json.load(open('src/c/module.json'))
if 'ui_hierarchy' in mj.get('capabilities', {}):
    print("FAIL module.json: declares ui_hierarchy, so ui_chain.js will never load")
    fail = 1
if section('ui_hierarchy') not in (None, '(unserved)'):
    print("FAIL plugin: serves ui_hierarchy, so ui_chain.js will never load")
    fail = 1
print("ok   no hierarchy (pads reachable)")

# Every key the UI drives must have metadata, or the chain line and the LFO
# picker invent a float 0..1 knob for it.
for b in range(1, 9):
    for k in ('type', 'on', 'model', 'quality', 'cab', 'fx', 'cpu',
              'p1', 'p2', 'p3', 'p4', 'p5'):
        wk = 'b%d_%s' % (b, k)
        if wk not in keys:
            print("FAIL chain_params: no metadata for %s" % wk); fail = 1

# The pedal list the UI's tree groups. Its INDEX is the wire value, so a
# reorder silently re-points every saved board.
fxl = section('fx_list')
if fxl in (None, '(unserved)'):
    print("FAIL fx_list: not served, so the tree has no names"); fail = 1
else:
    # The FIRST SIXTEEN are pinned by NAME AND POSITION, because a board
    # saved before 0.5.0 stores the number: reordering them re-points every
    # pedal on every saved board, silently. Anything after them is appended
    # and only has to exist.
    ORIGINAL = ['Overdrive', 'Distortion', 'Fuzz', 'Boost',
                'Compressor', 'Gate', 'EQ', 'Auto Wah',
                'Chorus', 'Phaser', 'Tremolo',
                'Delay', 'Slapback', 'Reverb', 'Doubler', 'Detune']
    try:
        names = json.loads(fxl)
        if names[:16] != ORIGINAL:
            print("FAIL fx_list: the first 16 moved - every saved board "
                  "re-points. got %s" % names[:16]); fail = 1
        elif len(names) < 16:
            print("FAIL fx_list: %d entries" % len(names)); fail = 1
        else:
            print("ok   fx_list: %d pedals, the original 16 pinned in place"
                  % len(names))

        # AND THE UI'S TREE MUST COVER IT. The plugin serves a flat list and
        # ui_chain.js groups it by hand; a pedal missing from the tree is a
        # pedal that ships, works, and is unreachable, with nothing to say
        # so. A duplicate is two rows that are the same pedal.
        tree = open('src/c/ui_chain.js').read()
        m = re.search(r'const FX_TREE = \[(.*?)\n\];', tree, re.S)
        if not m:
            print("FAIL fx tree: FX_TREE not found in ui_chain.js"); fail = 1
        else:
            ids = [int(x) for x in re.findall(r'items:\s*\[([^\]]*)\]', m.group(1))
                             for x in x.split(',') if x.strip()]
            if sorted(ids) != list(range(len(names))):
                missing = sorted(set(range(len(names))) - set(ids))
                dupes = sorted({i for i in ids if ids.count(i) > 1})
                print("FAIL fx tree: missing %s, duplicated %s"
                      % ([names[i] for i in missing], [names[i] for i in dupes]))
                fail = 1
            else:
                print("ok   fx tree: every pedal reachable, exactly once")
    except Exception as e:
        print("FAIL fx_list: %s" % e); fail = 1

# The UI reads these to NAME what is loaded; an index is not an answer.
for k in ('model_list', 'cab_list', 'fx_list'):
    raw = section(k)
    if raw is None or raw == '(unserved)':
        print("FAIL %s: not served, so the screen can only show an index" % k)
        fail = 1
    else:
        try:
            json.loads(raw)
        except Exception as e:
            print("FAIL %s: invalid JSON - %s" % (k, e)); fail = 1
print("ok   picker lists served to the UI")

# The two flags that only survive when the PLUGIN's string is used verbatim.
for k in ('cpu', 'b1_cpu'):
    p = next((x for x in cp if x['key'] == k), None)
    if not p or p.get('access') != 'read' or p.get('live') is not True:
        print("FAIL %s: must carry access=read and live=true" % k); fail = 1
print("ok   read-only live meters declared")

# The bundled files must reach the picker, or a fresh install offers nothing.
mods = next(x for x in cp if x['key'] == 'b1_model')['options']
cabs = next(x for x in cp if x['key'] == 'b1_cab')['options']
if mods == ['(none)'] or cabs == ['(none)']:
    print("FAIL: bundled models/cabs did not reach the pickers"); fail = 1
else:
    print("ok   pickers: %d model(s), %d cab(s)" % (len(mods), len(cabs)))

print("FAILED" if fail else "PASS")
sys.exit(1 if fail else 0)
PY
