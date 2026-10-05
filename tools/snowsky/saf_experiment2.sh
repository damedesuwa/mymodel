#!/data/data/com.termux/files/usr/bin/bash
# Step 0 추가 실험 — 작품 순서용 "패드로 빈자리 채우기" 가 실제 SD 에서 통하는지 확인.
# Asmr/__saf_test__ 안에서만 쓰고, 끝나면 그 폴더만 삭제한다.
# 사용(승인 후에만): bash saf_experiment2.sh 2>&1 | tee ~/saf_experiment2.log
set -euo pipefail
TESTNAME=__saf_test__
ROOT=$(termux-saf-dirs | jq -r '.[] | select(.name=="Asmr") | .uri')
[ -n "$ROOT" ] && [ "$(printf '%s\n' "$ROOT" | wc -l)" -eq 1 ] || { echo "Asmr URI 특정 불가"; exit 1; }
if termux-saf-ls "$ROOT" | jq -e --arg n "$TESTNAME" '.[]|select(.name==$n)' >/dev/null; then
  echo "$TESTNAME 이미 존재 — 중단"; exit 1
fi
T=""
cleanup() {
  [ -n "$T" ] || return 0
  if [ "$(termux-saf-stat "$T" | jq -r .name)" = "$TESTNAME" ]; then
    termux-saf-rm "$T" && echo "정리: $TESTNAME 삭제 완료"
    termux-saf-ls "$ROOT" | jq -e --arg n "$TESTNAME" '.[]|select(.name==$n)' >/dev/null \
      && echo "경고: 아직 남아 있음" || echo "확인: 루트에 $TESTNAME 없음"
  else echo "경고: 이름 불일치 — 삭제하지 않음 ($T)"; fi
}
trap cleanup EXIT
names() { termux-saf-ls "$T" | jq -r '[.[].name] | join("  |  ")'; }
declare -A U
L=(
  "01：長い日本語の作品名テストその一です"
  "02：長い日本語の作品名テストその二です"
  "03：長い日本語の作品名テストその三です"
  "04：長い日本語の作品名テストその四です"
  "05：長い日本語の作品名テストその五です"
)
T=$(termux-saf-mkdir "$ROOT" "$TESTNAME")
echo "== 1. 긴 이름 폴더 5개 생성"
for n in "${L[@]}"; do U[$n]=$(termux-saf-mkdir "$T" "$n"); done
names
echo; echo "== 2. 02, 04 삭제 후 패드 없이 긴 이름 폴더 X 생성 (빈자리로 들어가는지)"
termux-saf-rm "${U[${L[1]}]}"; termux-saf-rm "${U[${L[3]}]}"
X=$(termux-saf-mkdir "$T" "X：長い日本語の作品名テスト追加ですよ")
names
echo; echo "== 3. X 삭제 → 패드(~P0001…)로 빈자리를 채우고, 패드가 맨 뒤에 생기면 멈춤"
termux-saf-rm "$X"
i=0; PADS=()
while :; do
  i=$((i + 1)); p=$(printf '~P%04d' "$i")
  PADS+=("$(termux-saf-create -t application/octet-stream "$T" "$p")")
  last=$(termux-saf-ls "$T" | jq -r '.[-1].name')
  echo "  패드 $p → 맨 뒤: $last"
  [ "$last" = "$p" ] && break
  [ "$i" -ge 50 ] && { echo "  패드 50개 초과 — 중단"; break; }
done
names
echo; echo "== 4. 긴 이름 폴더 Y 생성 → 맨 뒤여야 함"
termux-saf-mkdir "$T" "Y：長い日本語の作品名テスト追加ですよ" >/dev/null
last=$(termux-saf-ls "$T" | jq -r '.[-1].name'); echo "  맨 뒤: $last"
[[ $last == Y：* ]] && echo "  결과: 성공 (맨 뒤)" || echo "  결과: 실패 (맨 뒤 아님)"
echo; echo "== 5. 패드 삭제 후 순서 (01 03 05 Y 여야 함)"
for u in "${PADS[@]}"; do termux-saf-rm "$u"; done
names
