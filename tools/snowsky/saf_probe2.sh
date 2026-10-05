#!/data/data/com.termux/files/usr/bin/bash
# Step 0-2 — 추가 조사 (읽기 전용). SD 카드에 아무것도 쓰지 않는다.
# 전제: termux-saf-managedir 로 Asmr 폴더 접근 권한을 이미 부여했고, jq 가 설치되어 있다.
# 사용: bash saf_probe2.sh 2>&1 | tee ~/saf_probe2.log
set -u

echo "== 1. 쓰기 계열 명령 소스 (cat 만 함, 실행하지 않음)"
for c in create mkdir write rm managedir; do
  echo "----- termux-saf-$c"
  cat "$PREFIX/bin/termux-saf-$c"
done

echo
echo "== 2. 도구"
command -v jq sha256sum stat sed awk sort 2>&1
locale 2>/dev/null | head -n 3

echo
echo "== 3. termux-saf-dirs (권한 부여된 폴더 — 여기서 나온 URI만 사용)"
DIRS=$(timeout 30 termux-saf-dirs 2>&1); echo "$DIRS"
command -v jq >/dev/null || { echo "jq 없음 — 'pkg install jq' 후 다시 실행"; exit 0; }

ROOT=$(printf '%s' "$DIRS" | jq -r '.[0].uri // empty')
[ -n "$ROOT" ] || { echo "권한 부여된 폴더 없음 — termux-saf-managedir 먼저"; exit 0; }
echo "ROOT=$ROOT"

echo
echo "== 4. 루트 ls (앞 30개 항목)"
RL=$(timeout 60 termux-saf-ls "$ROOT"); echo "$RL" | jq -c '.[:30][]'
echo "항목 수: $(echo "$RL" | jq length)"

W=$(echo "$RL" | jq -r '[.[]|select(.type=="vnd.android.document/directory")][0].uri // empty')
if [ -n "$W" ]; then
  echo
  echo "== 5. 첫 작품 폴더 ls (반환된 URI 사용)"
  echo "$RL" | jq -r '[.[]|select(.type=="vnd.android.document/directory")][0].name'
  t0=$(date +%s%N)
  timeout 60 termux-saf-ls "$W" | jq -c '.[]'
  t1=$(date +%s%N); echo "ls 소요: $(( (t1-t0)/1000000 )) ms"
fi

echo
echo "== 6. 전각 숫자 변환 확인"
printf '０１：テスト.mp3\n' | sed 'y/０１２３４５６７８９/0123456789/'
