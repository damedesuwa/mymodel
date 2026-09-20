#!/usr/bin/env bash
# Re-publish a built module under a DIFFERENT id.
#
# Not a feature - a way past an install that cannot land.
#
# A module's .so is mmap'd by MoveOriginal for as long as it sits in a slot,
# so busybox tar's in-place overwrite gets ETXTBSY on that one file while
# every other file in the tarball extracts fine. The install reports success
# and replaces everything except the code, and the half that updated hides
# the half that did not. It has happened twice on this branch: nam-a2 sat on
# an ed5399c .so through four releases, and nam-a2c sat on `blocks-1` through
# three.
#
# The chain host builds an FX path as "<id>/<id>.so" (chain_host.c:357), so
# the id is the only thing that moves the file. A new id installs into a
# directory nothing has open and therefore cannot be blocked.
#
# The proper fix is to uninstall before installing; this is for when that has
# already been tried and the log still shows the old build.
#
#   ./scripts/reid.sh nam-a2c nam-a2d "Nam A2d" A2D
set -e
cd "$(dirname "$0")/.."

SRC_ID="$1"; NEW_ID="$2"; NEW_NAME="$3"; NEW_ABBREV="$4"
[ -n "$NEW_ABBREV" ] || { echo "usage: reid.sh <src-id> <new-id> <name> <abbrev>"; exit 1; }
[ -d "dist/$SRC_ID" ] || { echo "dist/$SRC_ID not built"; exit 1; }

rm -rf "dist/$NEW_ID"
cp -r "dist/$SRC_ID" "dist/$NEW_ID"
mv "dist/$NEW_ID/$SRC_ID.so" "dist/$NEW_ID/$NEW_ID.so"

python3 - "$NEW_ID" "$NEW_NAME" "$NEW_ABBREV" <<'PY'
import json, sys
nid, name, abbrev = sys.argv[1:4]
p = 'dist/%s/module.json' % nid
d = json.load(open(p))
d['id'] = nid
d['name'] = name
d['abbrev'] = abbrev
d['dsp'] = nid + '.so'
json.dump(d, open(p, 'w'), indent=2, ensure_ascii=False)
open(p, 'a').write('\n')
PY

( cd dist && rm -f "$NEW_ID-module.tar.gz" && tar -czf "$NEW_ID-module.tar.gz" "$NEW_ID/" )
echo "dist/$NEW_ID-module.tar.gz  ($NEW_NAME, from $SRC_ID)"
