#!/usr/bin/env bash
# asmr_cover_update.sh — 새 작품을 SD 카드에 넣은 뒤 이것 하나만 실행하면 된다.
#
#   1) SD 카드 전체를 새로 스캔하고, 커버가 없는 MP3만 골라 매칭을 확인 (DRY RUN)
#   2) 결과를 작품 단위로 짧게 요약
#   3) 'y' 로 답하면 그 대상에만 240x240 Front Cover 삽입
#
# 규칙(asmr_cover_embed.sh 의 --work-cover --other-images):
#   - 작품 = Asmr 바로 아래 폴더, 그 안의 모든 하위 폴더 포함. 다른 작품 이미지는 절대 안 씀
#   - 작품 안 이미지(JPG/PNG/WEBP…)가 1장 -> 그 작품의 커버 없는 MP3 전부에 적용
#   - 이미지가 여러 장 -> 적용하지 않고 후보만 보여 줌 (한 장만 남기고 다시 실행)
#   - 이미 커버가 있는 MP3, 원본 이미지, 음원 데이터는 건드리지 않음
#
# 사용:  bash ~/bin/asmr_cover_update.sh            (추가 인자는 본 스크립트로 전달, 예: --work RJ01234567)
set -euo pipefail

MAIN="$HOME/bin/asmr_cover_embed.sh"
URL="https://raw.githubusercontent.com/damedesuwa/mymodel/ccr-248d2e90-8vttzu/tools/termux-cover-embed/asmr_cover_embed.sh"
STATE="${ASMR_COVER_HOME:-$HOME/.asmr_cover}"
RULES=(--work-cover --other-images)

if [ ! -f "$MAIN" ]; then
    echo "본 스크립트가 없어 내려받습니다: $MAIN"
    mkdir -p "$(dirname "$MAIN")"
    curl -fsSL -o "$MAIN" "$URL"
    chmod +x "$MAIN"
fi

if command -v termux-wake-lock >/dev/null 2>&1; then
    termux-wake-lock || true
    trap 'termux-wake-unlock >/dev/null 2>&1 || true' EXIT
fi

if [ -d "$STATE/pending" ] && [ -n "$(ls -A "$STATE/pending" 2>/dev/null)" ]; then
    echo "지난번 작업이 중간에 끊긴 흔적이 있어 원본부터 되돌립니다."
    bash "$MAIN" --restore-pending
fi

latest() { ls -t "$STATE/reports/$1"_*."$2" 2>/dev/null | head -1; }

echo "[1/3] SD 카드를 스캔하고 확인하는 중입니다. 작품 수에 따라 수십 분 걸릴 수 있습니다…"
if ! bash "$MAIN" "${RULES[@]}" "$@" > /dev/null; then
    echo "스캔 중 오류가 났습니다. 리포트: $(latest report txt)"
    exit 1
fi
REPORT="$(latest report txt)"
PLAN="$(latest plan tsv)"

echo "[2/3] 결과"
set +e
python3 - "$REPORT" "$PLAN" <<'PY'
import sys
from collections import OrderedDict, defaultdict
report, plan = sys.argv[1], sys.argv[2]

by_work = OrderedDict()
counts = defaultdict(int)
for line in open(plan, encoding="utf-8").read().splitlines()[1:]:
    c = line.split("\t")
    status, work = c[0], c[1]
    counts[status] += 1
    by_work.setdefault(work, defaultdict(int))[status] += 1

cands = defaultdict(list)
work = None
for line in open(report, encoding="utf-8").read().splitlines():
    if line.startswith("[") and "] " in line and " MP3 " in line:
        work = line[1:line.rindex("]   MP3")]
    elif line.strip().startswith("후보 ") and work:
        path = line.split(": ", 1)[1]
        short = path.split(work + "/", 1)[-1]
        if short not in cands[work]:
            cands[work].append(short)

def works_with(status):
    return [(w, s[status]) for w, s in by_work.items() if s.get(status)]

todo = works_with("EMBED")
print("  이미 커버 있음      : %d곡" % counts["SKIP_EXISTING"])
print("  새로 커버 넣을 곡   : %d곡 (작품 %d개)" % (counts["EMBED"], len(todo)))
for w, n in todo:
    print("      - %s  (%d곡)" % (w, n))
none = works_with("NO_IMAGE")
if none:
    print("  이미지가 없는 작품  : %d개 — 작품 폴더에 표지 이미지를 넣고 다시 실행" % len(none))
    for w, n in none:
        print("      - %s  (%d곡)" % (w, n))
amb = works_with("AMBIGUOUS")
if amb:
    print("  이미지가 여러 장    : %d개 — 커버 한 장만 남기고 다시 실행" % len(amb))
    for w, n in amb:
        print("      - %s  (%d곡)" % (w, n))
        for c in cands.get(w, []):
            print("          · " + c)
if counts["READ_FAIL"]:
    print("  읽기 실패           : %d곡 (리포트에서 'READ FAIL' 검색)" % counts["READ_FAIL"])
sys.exit(0 if counts["EMBED"] else 3)
PY
rc=$?
set -e
if [ "$rc" -eq 3 ]; then
    echo "새로 넣을 곡이 없습니다. 끝."
    exit 0
elif [ "$rc" -ne 0 ]; then
    echo "결과를 요약하지 못했습니다. 리포트: $REPORT"
    exit 1
fi

printf "위 곡들에 커버를 넣을까요? (y/n) "
read -r ans || ans=""
case "$ans" in
    y|Y|yes|YES|Yes|ㅛ|예|네) ;;
    *) echo "적용하지 않았습니다."; exit 0 ;;
esac

echo "[3/3] 적용 중…"
bash "$MAIN" --reuse-scan "${RULES[@]}" --apply --yes "$@" > /dev/null || true
FINAL="$(latest report txt)"
grep -E '자가 시험|중단|치명적|적용 성공|적용 실패' "$FINAL" | sed 's/^ *//'
if grep -q 'APPLY FAIL' "$FINAL"; then
    echo "실패한 곡은 원본 그대로입니다. 리포트: $FINAL"
fi
