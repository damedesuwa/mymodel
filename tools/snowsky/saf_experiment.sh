#!/data/data/com.termux/files/usr/bin/bash
# Step 0 쓰기 실험 — Asmr/__saf_test__ 안에서만 쓰고, 끝나면 그 폴더만 삭제한다.
# 사용(승인 후에만): bash saf_experiment.sh 2>&1 | tee ~/saf_experiment.log
set -euo pipefail
command -v jq >/dev/null || { echo "jq 없음"; exit 1; }
TESTNAME=__saf_test__
ROOT=$(termux-saf-dirs | jq -r '.[] | select(.name=="Asmr") | .uri')
[ -n "$ROOT" ] && [ "$(printf '%s\n' "$ROOT" | wc -l)" -eq 1 ] || { echo "Asmr URI 특정 불가"; exit 1; }
if termux-saf-ls "$ROOT" | jq -e --arg n "$TESTNAME" '.[]|select(.name==$n)' >/dev/null; then
  echo "$TESTNAME 이미 존재 — 중단 (수동 확인 필요)"; exit 1
fi

ms() { echo $(( ($(date +%s%N) - $1) / 1000000 )); }
LOC="$TMPDIR/saf_exp.$$"; mkdir -p "$LOC"
T=""
cleanup() {
  rm -rf "$LOC"
  [ -n "$T" ] || return 0
  # 반환된 URI의 실제 이름이 __saf_test__ 일 때만 삭제
  if [ "$(termux-saf-stat "$T" | jq -r .name)" = "$TESTNAME" ]; then
    termux-saf-rm "$T" && echo "정리: $TESTNAME 삭제 완료" || echo "정리 실패 rc=$?"
    termux-saf-ls "$ROOT" | jq -e --arg n "$TESTNAME" '.[]|select(.name==$n)' >/dev/null \
      && echo "경고: $TESTNAME 가 아직 남아 있음" || echo "확인: 루트에 $TESTNAME 없음"
  else
    echo "경고: 테스트 폴더 이름 불일치 — 삭제하지 않음 ($T)"
  fi
}
trap cleanup EXIT

lsnames() { termux-saf-ls "$T" | jq -r '.[] | "  \(.name)  \(.length // "-")"'; }
mk() { local s; s=$(date +%s%N); local u; u=$(termux-saf-create -t audio/mpeg "$T" "$1"); echo "  create '$1' $(ms $s) ms -> $u" >&2; printf '%s' "$u"; }

echo "== A. mkdir"
s=$(date +%s%N); T=$(termux-saf-mkdir "$ROOT" "$TESTNAME"); echo "mkdir $(ms $s) ms -> $T"
termux-saf-stat "$T"

echo; echo "== B. 바이너리 무결성 (3 MB 랜덤, 전각 콜론 + 일본어 이름)"
head -c 3000000 /dev/urandom > "$LOC/src.bin"
SRC=$(sha256sum < "$LOC/src.bin" | cut -d' ' -f1)
U=$(mk "01：テスト.mp3")
s=$(date +%s%N); termux-saf-write "$U" < "$LOC/src.bin"; echo "  write 3MB $(ms $s) ms"
s=$(date +%s%N); DST=$(termux-saf-read "$U" | sha256sum | cut -d' ' -f1); echo "  read+sha 3MB $(ms $s) ms"
echo "  src=$SRC"; echo "  dst=$DST"; [ "$SRC" = "$DST" ] && echo "  결과: 일치" || echo "  결과: 불일치!"
termux-saf-stat "$U"

echo; echo "== C. ls 순서 = 생성 순서인가? (z, a, m 순으로 생성)"
for n in z.mp3 a.mp3 m.mp3; do u=$(mk "$n"); printf 'x' | termux-saf-write "$u"; [ "$n" = a.mp3 ] && UA=$u; done
lsnames

echo; echo "== D. a.mp3 삭제 후 긴 이름 1개 + 짧은 이름 1개 생성 — 빈자리 재사용 여부"
termux-saf-rm "$UA"; echo "  rm a.mp3 rc=$?"
for n in "02：長い日本語のファイル名のテストです.mp3" "b.mp3"; do u=$(mk "$n"); printf 'y' | termux-saf-write "$u"; done
lsnames

echo; echo "== E. 같은 이름(z.mp3) 다시 create"
u=$(mk z.mp3); echo "  실제 이름: $(termux-saf-stat "$u" | jq -r .name)"
lsnames
