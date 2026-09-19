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

for name in ('chain_params', 'ui_hierarchy', 'state'):
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

# Every key the hierarchy names, resolved through the child template, must
# have metadata - otherwise the grid invents a float 0..1 knob and writes
# 0.058750 into an enum.
mj = json.load(open('src/c/module.json'))
levels = mj['capabilities']['ui_hierarchy']['levels']
for lname, lvl in levels.items():
    tmpl = lvl.get('child_key_template')
    n = lvl.get('child_count', 0)
    base = lvl.get('child_index_base', 0)
    names = [p['key'] for p in lvl.get('params', []) if 'key' in p]
    for k in names:
        wanted = ([tmpl.replace('{index}', str(i + base)).replace('{key}', k)
                   for i in range(n)] if tmpl else [k])
        for wk in wanted:
            if wk not in keys:
                print("FAIL %s: level '%s' names %s, chain_params has no metadata for it"
                      % ('ui_hierarchy', lname, wk))
                fail = 1

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
