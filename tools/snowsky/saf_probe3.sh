#!/data/data/com.termux/files/usr/bin/bash
# Step 0-3 — 읽기 전용. Asmr 루트와 첫 작품 폴더를 ls 해서 형식/순서/속도를 본다.
# 사용: bash saf_probe3.sh 2>&1 | tee ~/saf_probe3.log
set -u
command -v jq >/dev/null || { echo "jq 없음 — pkg install jq"; exit 1; }
ROOT=$(termux-saf-dirs | jq -r '.[] | select(.name=="Asmr") | .uri')
[ -n "$ROOT" ] && [ "$(printf '%s\n' "$ROOT" | wc -l)" -eq 1 ] || { echo "Asmr 권한 URI를 하나로 특정할 수 없음"; exit 1; }
echo "ROOT=$ROOT"

ms() { echo $(( ($(date +%s%N) - $1) / 1000000 )); }

s=$(date +%s%N); RL=$(termux-saf-ls "$ROOT"); echo "루트 ls: $(ms $s) ms"
echo "항목 수: $(jq length <<<"$RL")  (폴더 $(jq '[.[]|select(.type=="vnd.android.document/directory")]|length' <<<"$RL"))"
echo "루트의 폴더 아닌 항목:"; jq -r '.[]|select(.type!="vnd.android.document/directory")|"  \(.name)"' <<<"$RL"
echo "루트 첫 5개 (ls 출력 순서 그대로):"; jq -c '.[:5][]' <<<"$RL"

W=$(jq -r '[.[]|select(.type=="vnd.android.document/directory")][0].uri // empty' <<<"$RL")
if [ -n "$W" ]; then
  echo; echo "== 첫 작품: $(jq -r '[.[]|select(.type=="vnd.android.document/directory")][0].name' <<<"$RL")"
  s=$(date +%s%N); WL=$(termux-saf-ls "$W"); echo "작품 ls: $(ms $s) ms"
  echo "ls 출력 순서 그대로 (이 순서가 SD 디렉터리 엔트리 순서일 가능성이 높음):"
  jq -r '.[] | "  [\(.type)] \(.name)  \(.length // "-") B"' <<<"$WL"
  F=$(jq -r '[.[]|select(.type!="vnd.android.document/directory")][0].uri // empty' <<<"$WL")
  [ -n "$F" ] && { echo "stat 예:"; termux-saf-stat "$F"; }
fi

echo; echo "== 전각 숫자 변환"
printf '０１：テスト.mp3\n１０．b.mp3\n' | sed 'y/０１２３４５６７８９/0123456789/'
echo; df -h "$TMPDIR" | tail -n 1
