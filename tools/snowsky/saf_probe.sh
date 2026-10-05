#!/data/data/com.termux/files/usr/bin/bash
# Step 0 — SAF 명령 조사 (읽기 전용). SD 카드에 아무것도 쓰지 않는다.
# 사용: bash saf_probe.sh 2>&1 | tee ~/saf_probe.log
set -u
ROOT_URI='content://com.android.externalstorage.documents/tree/0000-0000%3AAsmr'

echo "== 1. 설치된 termux-saf-* 명령"
ls -l "$PREFIX"/bin/termux-saf-* 2>&1

echo
echo "== 2. 각 명령의 소스(실행하지 않고 내용만 출력 — 옵션/usage 확인용)"
for f in "$PREFIX"/bin/termux-saf-*; do
  echo "----- $f"
  if file "$f" 2>/dev/null | grep -qi text; then
    cat "$f"
  else
    echo "(바이너리 또는 비텍스트 — 내용 생략)"
  fi
done

echo
echo "== 3. -h 출력 (usage만; managedir/create 류는 UI·쓰기 가능성이 있어 제외)"
for c in ls stat read dirs; do
  b="$PREFIX/bin/termux-saf-$c"
  [ -x "$b" ] || continue
  echo "----- termux-saf-$c -h"
  timeout 15 "$b" -h 2>&1 | head -n 40
done

echo
echo "== 4. 루트 읽기 (termux-saf-ls)"
timeout 60 termux-saf-ls "$ROOT_URI" 2>&1 | head -c 4000
echo
if [ -x "$PREFIX/bin/termux-saf-stat" ]; then
  echo "== 5. 루트 stat"
  timeout 30 termux-saf-stat "$ROOT_URI" 2>&1
fi
if [ -x "$PREFIX/bin/termux-saf-dirs" ]; then
  echo "== 6. 권한이 부여된 디렉터리 목록"
  timeout 30 termux-saf-dirs 2>&1
fi
echo
echo "== 7. 환경"
echo "PREFIX=$PREFIX TMPDIR=${TMPDIR:-}"
df -h "${TMPDIR:-$HOME}" 2>&1
command -v sha256sum jq 2>&1
