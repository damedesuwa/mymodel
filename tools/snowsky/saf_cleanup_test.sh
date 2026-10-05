#!/data/data/com.termux/files/usr/bin/bash
# Asmr/__saf_test__ 진단 + 정리. 안의 항목이 실험이 만든 것(긴 이름 테스트 폴더, ~P 패드)뿐일 때만 지운다.
# 사용: bash saf_cleanup_test.sh 2>&1 | tee ~/saf_cleanup_test.log
set -u
TESTNAME=__saf_test__
try() {  # 읽기 전용 SAF 호출을 최대 4번 시도, 매번 원시 출력/종료코드 표시
  local i out rc
  for i in 1 2 3 4; do
    out=$("$@" 2>&1); rc=$?
    if [ -n "$out" ] && jq -e . >/dev/null 2>&1 <<<"$out"; then printf '%s' "$out"; return 0; fi
    echo "  [시도 $i] $1 rc=$rc 출력=$(printf '%q' "${out:0:200}")" >&2
    sleep $((i * 2))
  done
  return 1
}
ROOT=$(try termux-saf-dirs | jq -r '.[] | select(.name=="Asmr") | .uri') || { echo "dirs 실패"; exit 1; }
echo "ROOT=$ROOT"
RL=$(try termux-saf-ls "$ROOT") || { echo "루트 ls 실패"; exit 1; }
echo "루트 항목 수: $(jq length <<<"$RL")"
T=$(jq -r --arg n "$TESTNAME" '[.[] | select(.name == $n)] | if length == 1 then .[0].uri else empty end' <<<"$RL")
[ -n "$T" ] || { echo "$TESTNAME 없음 — 정리할 것 없음"; exit 0; }
echo "$TESTNAME = $T"
TL=$(try termux-saf-ls "$T") || { echo "$TESTNAME ls 실패 — 지우지 않음"; exit 1; }
echo "내용:"; jq -r '.[] | "  [\(.type)] \(.name)  \(.length // "-")"' <<<"$TL"
if jq -e 'all(.[]; (.name | test("作品名テスト")) or (.name | test("^~P[0-9]{4}$")))' <<<"$TL" >/dev/null; then
  termux-saf-rm "$T"; echo "rm rc=$?"
  sleep 1
  RL=$(try termux-saf-ls "$ROOT") && { jq -e --arg n "$TESTNAME" 'any(.[]; .name == $n)' <<<"$RL" >/dev/null \
    && echo "경고: 아직 남아 있음" || echo "확인: $TESTNAME 삭제됨"; }
else
  echo "실험이 만들지 않은 항목이 있음 — 지우지 않음. 위 내용을 알려 주세요."
fi
