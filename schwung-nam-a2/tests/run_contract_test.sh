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
# The same seven the plugin makes and build.sh stages. Listed once here;
# the check below is what keeps this in step with the other two lists.
for d in amps od dist fuzz boost cabs reverbs; do
    mkdir -p "$FIX/$d"
    [ -d "src/$d" ] && cp "src/$d"/* "$FIX/$d/" 2>/dev/null || true
done

echo "--- running ---"
/tmp/a2c_dump_contract "$FIX" > /tmp/a2c_contract.txt

# The pedal table as the BINARY serves it, for tests/test_ui_chain.mjs to
# drive the screen against. Generated rather than committed by hand: a
# hand-written copy would let the UI test pass while disagreeing with the
# plugin, which is the failure the served spec exists to prevent.
python3 - <<'GENPY'
import re, json, os
txt = open('/tmp/a2c_contract.txt').read()
m = re.search(r'===fx_specs===\n(.*?)(?=\n===|\Z)', txt, re.S)
if m:
    raw = m.group(1).strip()
    json.loads(raw)
    os.makedirs('tests/fixtures', exist_ok=True)
    open('tests/fixtures/fx_specs.json', 'w').write(raw + '\n')
    print("ok   fx_specs fixture written (%d bytes)" % len(raw))
GENPY

FIXDIR="$FIX" python3 - <<'PY'

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

# WHAT chain_params IS FOR, AND WHAT IT IS NOT.
#
# It used to be checked as "every key the UI drives must be declared", and
# that was the wrong rule in a way that only showed up at thirty-two blocks:
# the UI does not read chain_params at all - it reads fx_specs and the
# picker lists - and the host's consumers of it are the LFO target picker,
# the chain line and the screen reader. Declaring the asset pickers meant
# shipping their whole option list per block, up to 4 KB each, which at 16
# blocks could already pass 100 KB and be truncated mid-array.
#
# So the rule is now stated the way it actually works: the MODULABLE keys
# are declared for every block on both rows, and the pickers deliberately
# are not.
BLOCK_COLS = 16         # NUM_COLS in nam_a2c_plugin.cpp
DECLARED    = ('on', 'cpu', 'p1', 'p2', 'p3', 'p4', 'p5')
WITHHELD    = ('type', 'model', 'quality', 'cab', 'ir', 'fx')
for row in ('b', 't'):
    for b in range(1, BLOCK_COLS + 1):
        for k in DECLARED:
            wk = '%s%d_%s' % (row, b, k)
            if wk not in keys:
                print("FAIL chain_params: no metadata for %s" % wk); fail = 1
for k in WITHHELD:
    wk = 'b1_%s' % k
    if wk in keys:
        print("FAIL chain_params: %s is declared; its option list is "
              "kilobytes a block and nothing reads it from here" % wk)
        fail = 1
print("ok   chain_params: %d modulable keys a block, pickers withheld"
      % len(DECLARED))

# THE HOST'S CAP, WHICH TRUNCATES IN SILENCE. MAX_CHAIN_PARAMS is 256 in
# src/modules/chain/dsp/chain_internal.h, and parse_chain_params_array_json
# stops at max_params and returns the short count - no error anywhere, and
# every block past the cut falls through to an invented float 0..1 knob.
# The C side has a static_assert; this is the same fact measured on what
# the binary actually emitted.
if len(cp) > 256:
    print("FAIL chain_params: %d entries, past the host's 256 - the tail is "
          "dropped silently" % len(cp)); fail = 1
elif len(cp) > 248:
    print("FAIL chain_params: %d entries, within 8 of the 256 cap - one more "
          "per-block key is 32 more and does not fit" % len(cp)); fail = 1
else:
    print("ok   chain_params: %d entries, under the host's 256 cap" % len(cp))

# The pedal list the UI's tree groups. Its INDEX is the wire value, so a
# reorder silently re-points every saved board.
fxl = section('fx_list')
if fxl in (None, '(unserved)'):
    print("FAIL fx_list: not served, so the tree has no names"); fail = 1
else:
    # WHAT IS PINNED IS THE COVER, NOT THE ORDER.
    #
    # The list was an append-only wire format and the first sixteen names
    # were pinned in place, because a saved board stores the NUMBER. 0.6.0
    # replaced the generic pedals with modelled ones and renumbered, which
    # that pin exists to catch - so the reason it is allowed here has to be
    # written down rather than just overridden: every release of this
    # module ships under a NEW module id (an in-place .so overwrite gets
    # ETXTBSY and silently does not land), and a new id is a new component
    # with no state to carry, so there is no board on any device reading
    # these numbers. The pin returns below as a cover check, which is the
    # invariant that survives a renumber.
    try:
        names = json.loads(fxl)
        if len(names) != len(set(names)):
            dupes = sorted({n for n in names if names.count(n) > 1})
            print("FAIL fx_list: two pedals share a name: %s" % dupes); fail = 1
        else:
            print("ok   fx_list: %d pedals, all distinct" % len(names))

        # THE UI'S CATEGORIES MUST COVER IT. The plugin serves a flat list
        # and ui_chain.js groups it by hand; a pedal missing from the
        # categories is a pedal that ships, works, and is unreachable, with
        # nothing to say so. A duplicate is two rows that are one pedal.
        ui = open('src/c/ui_chain.js').read()
        m = re.search(r'const CATS = \[(.*?)\n\];', ui, re.S)
        if not m:
            print("FAIL fx cats: CATS not found in ui_chain.js"); fail = 1
        else:
            # `tail:` as well as `items:` - a tail entry is reachable, it
            # just sits after the captures, and a check that only knew one
            # of the two keys would call a shipped pedal unreachable.
            ids = [int(x)
                   for grp in re.findall(r'(?:items|tail):\s*\[([^\]]*)\]',
                                         m.group(1))
                   for x in grp.split(',') if x.strip()]
            if sorted(ids) != list(range(len(names))):
                missing = sorted(set(range(len(names))) - set(ids))
                dupes = sorted({i for i in ids if ids.count(i) > 1})
                print("FAIL fx cats: missing %s, duplicated %s"
                      % ([names[i] for i in missing], [names[i] for i in dupes]))
                fail = 1
            else:
                print("ok   fx cats: every pedal reachable, exactly once")

        # AND THE SPEC TABLE MUST DESCRIBE EVERY ONE OF THEM, with the same
        # names. Two lists built from one C table cannot disagree - but the
        # emitters are two functions and this is what says so.
        specs = json.loads(section('fx_specs'))
        if [p['n'] for p in specs] != names:
            print("FAIL fx_specs: names disagree with fx_list"); fail = 1
        else:
            bad = []
            for sp in specs:
                if len(sp['k']) != 5: bad.append(sp['n'] + ': not 5 slots')
                for kk in sp['k']:
                    if not kk: continue
                    if kk['c'] == 1 and (kk['lo'] <= 0 or kk['hi'] <= 0):
                        bad.append(sp['n'] + '/' + kk['n'] + ': exp curve through zero')
                    if kk['c'] == 2 and len(str(kk.get('o','')).split('|')) < int(kk['hi']):
                        bad.append(sp['n'] + '/' + kk['n'] + ': fewer options than positions')
                if sp['aw'] >= 0 and not sp['k'][sp['aw']]:
                    bad.append(sp['n'] + ': alt_when points at a knob it does not have')
            if bad:
                print("FAIL fx_specs: " + "; ".join(bad[:4])); fail = 1
            else:
                print("ok   fx_specs: %d pedals described, curves sane" % len(specs))
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

# The bundled files must reach the picker, or a fresh install offers
# nothing. Read from the LISTS the UI actually loads: this used to take
# b1_model's enum options out of chain_params, which was a second route to
# the same names and is no longer declared there - the pickers are the
# module's own screen reading model_list, and that is the path to check.
mods = json.loads(section('model_list'))
cabs = json.loads(section('cab_list'))
if not mods or not cabs:
    print("FAIL: bundled models/cabs did not reach the pickers "
          "(%d model(s), %d cab(s))" % (len(mods), len(cabs))); fail = 1
else:
    print("ok   pickers: %d model(s), %d cab(s)" % (len(mods), len(cabs)))

# A fresh cab sits where the capture put it, with no mic character added.
rb = {}
for line in open("/tmp/a2c_contract.txt"):
    if line.startswith("b4_p"):
        k, _, v = line.partition(" ")
        rb[k.strip()] = v.strip()
want = {"b4_p1": 0.0, "b4_p2": 0.5, "b4_p3": 0.0, "b4_p4": 0.0}
for k, v in want.items():
    got = rb.get(k)
    if got is None or abs(float(got) - v) > 1e-4:
        print("FAIL %s: a fresh cab must read %.1f, got %s" % (k, v, got)); fail = 1
if not fail:
    print("ok   a fresh cab's mic is at the capture, character off")

# THE FOLDER NAME IS THE CATEGORY NAME, and three lists have to say the
# same seven words: the plugin's table (which creates and scans them),
# module.json (which is what the web manager renders), and the UI's
# categories. A folder on one side only is either uploadable into and
# never read, or scanned and never offered - and neither says anything on
# the device.
import os
mj = json.load(open("src/c/module.json"))
declared = set(f["path"] for f in mj["assets"].get("folders", []))
FIXDIR = os.environ["FIXDIR"]
made = set(d for d in os.listdir(FIXDIR)
           if os.path.isdir(os.path.join(FIXDIR, d)))
if made != declared:
    print("FAIL: the module creates %s, module.json declares %s"
          % (sorted(made), sorted(declared))); fail = 1

ui = open("src/c/ui_chain.js").read()
ui_dirs = set(re.findall(r"folder: '([a-z]+)'", ui))
if ui_dirs != declared:
    print("FAIL: ui_chain.js points at %s, the module makes %s"
          % (sorted(ui_dirs), sorted(declared))); fail = 1
if not fail:
    print("ok   asset folders: %d, named the same in all three" % len(declared))

# Served per model, parallel to the names, so the UI can slice the list.
cats = section("model_dirs")
names = json.loads(section("model_list"))
if cats is None:
    print("FAIL: model_cats is not served"); fail = 1
else:
    arr = json.loads(cats)
    if len(arr) != len(names):
        print("FAIL: model_dirs has %d entries for %d models"
              % (len(arr), len(names))); fail = 1
    else:
        bad = [x for x in arr if x not in declared]
        if bad:
            print("FAIL: model_dirs names %s, which is no declared folder"
                  % sorted(set(bad))); fail = 1
        else:
            print("ok   model_dirs: %d, one per model, all declared" % len(arr))

print("FAILED" if fail else "PASS")
sys.exit(1 if fail else 0)
PY
