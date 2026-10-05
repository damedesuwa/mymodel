#!/data/data/com.termux/files/usr/bin/bash
# snowsky_reorder.sh 의 실행 로그를 읽어, "성공" 처리된 작품마다
#   계획(처리 시작 줄의 파일·하위 폴더 수) == 실제(완료 줄의 파일·폴더 수)
# 인지 확인한다. SD 에는 접근하지 않는다.
# 사용: bash snowsky_audit.sh [로그 파일들…]   (기본: ~/snowsky_logs/*_apply.log *_test.log)
set -u
if [ $# -eq 0 ]; then set -- "$HOME"/snowsky_logs/*_apply.log "$HOME"/snowsky_logs/*_test.log; fi
ok=0 bad=0 name="" ef="" ed=""
for f in "$@"; do
  [ -f "$f" ] || continue
  while IFS= read -r line; do
    case $line in
      *"=== ["*"] 처리 시작 ("*)
        name=${line#*=== [}; name=${name%%] 처리 시작 (*}
        ef=$(grep -oE '파일 [0-9]+개, 하위 폴더 [0-9]+개' <<<"$line" | grep -oE '[0-9]+' | sed -n 1p)
        ed=$(grep -oE '파일 [0-9]+개, 하위 폴더 [0-9]+개' <<<"$line" | grep -oE '[0-9]+' | sed -n 2p) ;;
      *"  갱신된 계획: 파일 "*)   # 스캔 이후 작품이 바뀌어 다시 읽은 경우 계획 수를 갱신
        ef=$(grep -oE '파일 [0-9]+개' <<<"$line" | grep -oE '[0-9]+')
        ed=$(grep -oE '하위 폴더 [0-9]+개' <<<"$line" | grep -oE '[0-9]+') ;;
      *"  완료: 파일 "*" 개 + 폴더 "*)
        af=$(grep -oE '파일 [0-9]+ 개' <<<"$line" | grep -oE '[0-9]+')
        ad=$(grep -oE '폴더 [0-9]+ 개' <<<"$line" | grep -oE '[0-9]+')
        if [ -n "$name" ] && [ "$af" = "$ef" ] && [ "$ad" = "$ed" ]; then ok=$((ok + 1))
        else bad=$((bad + 1)); echo "불일치: [$name] 계획 파일 ${ef:-?}개/폴더 ${ed:-?}개 → 실제 파일 ${af}개/폴더 ${ad}개  ($(basename "$f"))"; fi
        name="" ;;
    esac
  done < "$f"
done
echo "점검 완료: 일치 $ok 개, 불일치 $bad 개"
