#!/usr/bin/env bash
# snowsky_imgcheck.sh — Asmr 의 작품 폴더마다 이미지 파일이 정확히 1개인지 검사한다.
# 읽기 전용: termux-saf-dirs / termux-saf-ls 만 쓴다 (SD 를 바꾸지 않는다).
#
#   bash snowsky_imgcheck.sh            작품 폴더 바로 아래의 이미지 개수만 검사
#   bash snowsky_imgcheck.sh --deep     하위 폴더도 검사 (mp3 가 들어 있는 폴더마다)
#   bash snowsky_imgcheck.sh --folder 이름   이름에 해당 문자열이 들어간 작품만 검사
#
# 결과: 화면 + ~/snowsky_logs/imgcheck_<시각>.log
set -u
exec < /dev/null
ROOT_NAME=Asmr
DIR_MIME="vnd.android.document/directory"
IMG_RE='\.(jpe?g|png|gif|webp|bmp|heic)$'
DEEP=0 ONLY=""
while [ $# -gt 0 ]; do
  case "$1" in
    --deep) DEEP=1 ;;
    --folder) shift; ONLY=${1:-} ;;
    -h|--help) sed -n 2,10p "$0"; exit 0 ;;
    *) echo "알 수 없는 옵션: $1" >&2; exit 2 ;;
  esac
  shift
done

for c in jq termux-saf-dirs termux-saf-ls; do
  command -v "$c" >/dev/null 2>&1 || { echo "필요한 명령이 없음: $c" >&2; exit 1; }
done

LOGDIR=$HOME/snowsky_logs; mkdir -p "$LOGDIR"
LOG=$LOGDIR/imgcheck_$(date +%Y%m%d_%H%M%S).log
log() { printf '%s %s\n' "$(date +%T)" "$*" | tee -a "$LOG"; }

# 빈 응답이 가끔 오므로 유효한 JSON 배열이 올 때까지 다시 시도한다.
ls_json() {
  local out i
  for i in 1 2 3 4; do
    out=$(termux-saf-ls "$1" 2>/dev/null)
    if jq -e 'type == "array"' >/dev/null 2>&1 <<<"$out"; then printf '%s' "$out"; return 0; fi
    sleep "$i"
  done
  return 1
}

DIRS_OUT=""
for i in 1 2 3 4; do
  DIRS_OUT=$(termux-saf-dirs 2>/dev/null)
  jq -e 'type == "array"' >/dev/null 2>&1 <<<"$DIRS_OUT" && break
  sleep "$i"
done
ROOT=$(jq -r --arg n "$ROOT_NAME" '[.[] | select(.name == $n)] | if length == 1 then .[0].uri else empty end' <<<"$DIRS_OUT" 2>/dev/null)
[ -n "$ROOT" ] || { log "termux-saf-dirs 에서 '$ROOT_NAME' 를 하나로 찾을 수 없음 (termux-saf-managedir 로 권한 부여)"; exit 1; }

TOP=$(ls_json "$ROOT") || { log "Asmr 목록을 읽지 못함"; exit 1; }
WORKS=$(jq -r --arg d "$DIR_MIME" --arg o "$ONLY" \
  '.[] | select(.type == $d and (.name | startswith("~P") | not) and ($o == "" or (.name | contains($o)))) | [.name, .uri] | @tsv' <<<"$TOP")
NW=$(printf '%s\n' "$WORKS" | grep -c . || true)
[ "$NW" -gt 0 ] || { log "검사할 작품 폴더가 없음"; exit 1; }
log "작품 폴더 $NW 개 검사 시작 (모드: $([ "$DEEP" = 1 ] && echo '하위 폴더 포함' || echo '작품 폴더 바로 아래만'))"

ok=0 bad=0 n=0 err=0
# 폴더 하나를 검사한다: $1 표시 이름, $2 URI. 이미지 개수가 1 이 아니면 보고한다.
check_dir() {
  local label=$1 uri=$2 L cnt names mp3
  L=$(ls_json "$uri") || { log "읽기 실패: $label"; err=$((err + 1)); return 1; }
  cnt=$(jq --arg re "$IMG_RE" '[.[] | select(.type != "'"$DIR_MIME"'" and (.name | ascii_downcase | test($re)))] | length' <<<"$L")
  mp3=$(jq '[.[] | select(.type != "'"$DIR_MIME"'" and (.name | ascii_downcase | endswith(".mp3")))] | length' <<<"$L")
  if [ "$cnt" = 1 ]; then ok=$((ok + 1)); return 0; fi
  names=$(jq -r --arg re "$IMG_RE" '[.[] | select(.type != "'"$DIR_MIME"'" and (.name | ascii_downcase | test($re))) | .name] | join(", ")' <<<"$L")
  bad=$((bad + 1))
  log "이미지 ${cnt}개 (mp3 ${mp3}개): $label${names:+  → $names}"
  return 0
}

while IFS=$'\t' read -r -u 3 name uri; do
  [ -n "$name" ] || continue
  n=$((n + 1))
  check_dir "$name" "$uri"
  if [ "$DEEP" = 1 ]; then
    # 하위 폴더를 폭 우선으로 따라 내려가며, mp3 가 들어 있는 폴더만 검사한다.
    q=$(mktemp); printf '%s\t%s\n' "$name" "$uri" > "$q"
    while [ -s "$q" ]; do
      nx=$(mktemp)
      while IFS=$'\t' read -r -u 4 p u; do
        L=$(ls_json "$u") || { log "읽기 실패: $p"; err=$((err + 1)); continue; }
        jq -r --arg p "$p" --arg d "$DIR_MIME" '.[] | select(.type == $d) | [$p + "/" + .name, .uri] | @tsv' <<<"$L" >> "$nx"
      done 4< "$q"
      # 방금 찾은 하위 폴더 각각을 검사 (mp3 가 있을 때만 의미가 있으므로 mp3 0개 + 이미지 0개는 건너뜀)
      while IFS=$'\t' read -r -u 4 p u; do
        L=$(ls_json "$u") || { log "읽기 실패: $p"; err=$((err + 1)); continue; }
        if [ "$(jq '[.[] | select(.type != "'"$DIR_MIME"'" and (.name | ascii_downcase | endswith(".mp3")))] | length' <<<"$L")" -gt 0 ]; then
          check_dir "$p" "$u"
        fi
      done 4< "$nx"
      mv "$nx" "$q"
    done
    rm -f "$q"
  fi
  [ $((n % 25)) -eq 0 ] && printf '  … %d/%d\n' "$n" "$NW"
done 3< <(printf '%s\n' "$WORKS")

log "검사 완료: 폴더 $((ok + bad)) 개 중 이미지 1개 ${ok} 개, 아닌 곳 ${bad} 개, 읽기 실패 ${err} 개"
[ "$err" -eq 0 ] || log "읽기 실패가 있으면 결과가 완전하지 않으니 다시 실행하세요"
[ "$bad" -eq 0 ] && [ "$err" -eq 0 ]
