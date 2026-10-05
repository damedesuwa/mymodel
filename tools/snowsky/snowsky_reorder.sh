#!/data/data/com.termux/files/usr/bin/bash
# snowsky_reorder.sh — FiiO SnowSky ECHO NANO 용 MP3 "생성 순서" 재정렬 (Termux + SAF)
#
# SnowSky 는 파일을 SD 카드 디렉터리 엔트리 순서(복사 순서)로 보여준다.
# 이 스크립트는 Asmr/<작품>/ 과 그 하위 폴더 전부를 트랙 번호 순서대로 다시 생성한다.
#
# 각 폴더 안의 생성 순서: MP3(트랙 번호 숫자순, 번호 없는 것은 이름순으로 뒤)
#                       → 나머지 파일(cover.jpg 등, 원래 순서) → 하위 폴더(같은 번호 규칙)
#
# 작품 순서(Asmr 바로 아래): 같은 규칙(앞 번호 숫자순, 그다음 이름순)으로 정렬한다.
#   이미 맞는 앞부분(내부 순서도 정상)만 그대로 두고, 나머지 작품을 정렬 순서대로 다시 생성해서
#   루트의 맨 뒤에 붙인다. 지운 폴더 자리(빈 엔트리)에 새 폴더가 들어가는 것을 막기 위해
#   새 폴더를 만들기 전에 아주 짧은 이름의 빈 파일(~P0001…, "패드")로 앞쪽 빈자리를 채우고,
#   새 폴더가 실제로 맨 뒤에 생겼는지 ls 로 확인한다. 패드는 실행이 끝나면 모두 삭제한다.
#
# 방식 A: 작품(최상위 폴더) 하나씩
#   1) 작품 안 모든 폴더 재확인(ls) → 2) 모든 파일을 $TMPDIR 로 읽어 sha256/크기 검증(백업)
#   3) 원본 작품 폴더 삭제 → 4) 같은 이름으로 새 폴더 트리 생성, 순서대로 create+write
#   5) 모든 폴더를 ls 해서 이름/크기/엔트리 순서 검증 + 전 파일 재읽기 sha256 비교 → 6) 백업 삭제
# 실패하면 즉시 중단. 원본 삭제 이후의 실패에서는 백업을 절대 지우지 않는다(--restore).
#
# 바이트 그대로 복사만 한다. 재인코딩/태그 수정/이름 변경 없음.
# SAF URI 는 termux-saf-dirs / ls / mkdir / create 가 반환한 것만 사용한다.

set -uo pipefail

ROOT_NAME=Asmr
DIR_MIME=vnd.android.document/directory
CACHE_DIR=${SNOWSKY_CACHE:-$HOME/.cache/snowsky}
MANIFEST=$CACHE_DIR/manifest.json
BACKUP_BASE=${TMPDIR:?TMPDIR 가 비어 있음}/snowsky_backup
LOG_DIR=${SNOWSKY_LOGS:-$HOME/snowsky_logs}
# 동시에 도는 프로세스 수를 낮게 유지한다. 안드로이드 12+ 는 앱의 하위 프로세스가 약 32개를 넘으면 전부 SIGKILL 로 종료한다
# ("Process completed (signal 9)"). SAF 호출 하나가 프로세스 5~6개를 만들기 때문에 동시 작업은 보수적으로 둔다.
JOBS=3 IO_JOBS=1 PAD_JOBS=1 NO_PREP=1
GAP=0

MODE=dry YES=0 FOLDER="" LIMIT=0 FORCE=0 RESCAN=0 RESTORE="" TRACKS_ONLY=0 KEEP_PADS=auto
PAD_FMT="~P%05d" PAD_RE='^~P[0-9]{4,6}$' PADN=0 PADS_FILE=""
PRE_DONE=0 PREP_PID="" PREP_B="" NEXT_P="" IGNORE_BACKUPS=0

usage() {
  cat <<'U'
사용법: bash snowsky_reorder.sh [옵션]

  --dry-run           (기본값) 아무것도 변경하지 않고 계획만 출력
  --folder <작품명>    그 작품 하나만 대상 (dry-run 또는 --apply 와 함께)
  --limit <N>         다시 생성할 작품 중 앞에서 N개만 대상 (여러 번 나눠 실행해도 결과는 같음)
  --test <작품명>      그 작품 하나만 실제 처리 (이미 순서가 맞아도 다시 생성)
  --apply             실제 적용. 작품 하나 끝날 때마다 다음으로 갈지 y/N 확인
  --yes               --apply 에서 y/N 을 묻지 않고 끝까지 진행 (오류가 나면 그 자리에서 멈춤)
  --force             이미 트랙 순서대로인 작품도 대상에 포함
  --rescan            저장된 스캔 결과를 버리고 SD 카드를 다시 스캔
  --tracks-only       작품 순서는 맞추지 않고 작품 안의 트랙 순서만 맞춤
  --keep-pads         실행이 끝나도 패드(~P0001… 빈 파일)를 항상 남긴다
  --no-keep-pads      실행이 끝날 때마다 패드를 지운다 (다음 실행이 빈자리를 처음부터 다시 채운다)
                      기본값: 다시 생성할 작품이 남아 있으면 패드를 남기고, 마지막 작품까지 끝나면 지운다
  --cleanup-pads      Asmr 루트의 패드만 모두 지우고 끝냄
  --report            저장된 스캔 결과로 파일명 번호 패턴 리포트를 만든다 (SD 변경 없음)
                      → ~/snowsky_report.txt
  --gap <초>          파일 생성 사이 대기 시간 (기본 0)
  --jobs <N>          스캔 시 병렬 ls 개수 (기본 4, 읽기 전용)
  --io-jobs <N>       백업·검증 때 동시에 읽는 파일 수 (기본 1). 읽기만 동시에 하고 쓰기·생성은 항상 순서대로.
                      올리면 빨라지지만 안드로이드가 Termux 를 강제 종료(signal 9)할 수 있다
  --prep              (--yes 일 때) 다음 작품의 원본 삭제·패드 작업을 현재 작품을 쓰는 동안 미리 함 (작품당 약 13초 단축)
  --fast              빠른 모드 = --prep + --io-jobs 2. 동시에 도는 프로세스가 늘어나므로, 안드로이드 개발자 옵션의
                      "하위 프로세스 제한 해제"(Disable child process restrictions)를 켠 뒤에 쓰세요. 아니면 Termux 가
                      "Process completed (signal 9)" 로 강제 종료될 수 있다
  --restore <백업>     실패로 남은 백업 폴더에서 작품을 다시 생성
  --ignore-backups    복구하지 않은 백업이 남아 있어도 --apply/--test 를 진행 (기본: 먼저 --restore 하라고 알리고 멈춤)
  -h, --help          이 도움말

하위 폴더도 모두 스캔/처리한다. 작품 = Asmr 바로 아래의 폴더.
스캔 결과: ~/.cache/snowsky/manifest.json (한 번 스캔 후 재사용, 처리한 작품은 자동 갱신)
로그:      ~/snowsky_logs/
백업:      $TMPDIR/snowsky_backup/
U
}

while [ $# -gt 0 ]; do
  case $1 in
    --dry-run) MODE=dry ;;
    --apply)   MODE=apply ;;
    --yes)     YES=1 ;;
    --ignore-backups) IGNORE_BACKUPS=1 ;;
    --test)    MODE=test; FOLDER=${2:?--test 에 작품명 필요}; shift ;;
    --folder)  FOLDER=${2:?--folder 에 작품명 필요}; shift ;;
    --limit)   LIMIT=${2:?--limit 에 숫자 필요}; shift ;;
    --force)   FORCE=1 ;;
    --rescan)  RESCAN=1 ;;
    --tracks-only) TRACKS_ONLY=1 ;;
    --keep-pads)   KEEP_PADS=1 ;;
    --no-keep-pads) KEEP_PADS=0 ;;
    --cleanup-pads) MODE=cleanpads ;;
    --report)  MODE=report ;;
    --gap)     GAP=${2:?}; shift ;;
    --jobs)    JOBS=${2:?}; shift ;;
    --io-jobs) IO_JOBS=${2:?}; shift ;;
    --no-prep) NO_PREP=1 ;;
    --prep)    NO_PREP=0 ;;
    --fast)    NO_PREP=0; IO_JOBS=2 ;;
    --restore) MODE=restore; RESTORE=${2:?--restore 에 백업 경로 필요}; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "알 수 없는 옵션: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done
[[ $LIMIT =~ ^[0-9]+$ ]] || { echo "--limit 은 숫자" >&2; exit 2; }
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || { echo "--jobs 는 1 이상" >&2; exit 2; }
[[ $IO_JOBS =~ ^[1-9][0-9]*$ ]] || { echo "--io-jobs 는 1 이상" >&2; exit 2; }
[[ $GAP =~ ^[0-9]+([.][0-9]+)?$ ]] || { echo "--gap 은 초 단위 숫자" >&2; exit 2; }
[ "$MODE" = test ] && FORCE=1

for c in jq sha256sum termux-saf-dirs termux-saf-ls termux-saf-read termux-saf-stat \
         termux-saf-mkdir termux-saf-create termux-saf-write termux-saf-rm; do
  command -v "$c" >/dev/null || { echo "필요한 명령 없음: $c" >&2; exit 1; }
done

mkdir -p "$CACHE_DIR" "$LOG_DIR" "$BACKUP_BASE" || exit 1
LOG=$LOG_DIR/$(date +%Y%m%d_%H%M%S)_$MODE.log
exec > >(tee -a "$LOG") 2>&1
# termux-saf-* (termux-api) 는 stdin 을 읽어 갈 때가 있다. 반복문의 목록을 삼키지 않도록 표준 입력을 막고,
# 모든 반복문은 목록을 fd 3 으로 읽는다. (y/n 은 /dev/tty 에서 직접 읽는다)
exec < /dev/null

LOCK=$CACHE_DIR/lock
if ! mkdir "$LOCK" 2>/dev/null; then
  # 강제 종료(signal 9)로 잠금이 남았을 수 있다: 소유 프로세스가 없으면 잠금을 되찾는다
  OLDPID=$(cat "$LOCK/pid" 2>/dev/null)
  if [ -n "$OLDPID" ] && ! kill -0 "$OLDPID" 2>/dev/null; then
    echo "이전 실행이 비정상 종료된 흔적(잠금)을 정리합니다 (pid $OLDPID)"; rm -rf -- "$LOCK"
  elif [ -z "$OLDPID" ] && [ -z "$(pgrep -f snowsky_reorder.sh 2>/dev/null | grep -v "^$$\$")" ]; then
    echo "이전 실행이 비정상 종료된 흔적(잠금)을 정리합니다"; rm -rf -- "$LOCK"
  fi
  mkdir "$LOCK" 2>/dev/null || { echo "다른 실행이 진행 중입니다: $LOCK"; exit 1; }
fi
echo $$ > "$LOCK/pid"

STAGE=idle CUR_WORK="" CUR_BACKUP=""
on_exit() { kill_prefetch; [ "$KEEP_PADS" = 0 ] && cleanup_pads; rm -rf -- "$LOCK" 2>/dev/null; sleep 0.2; }
on_int() {
  echo
  log "중단됨 (단계: $STAGE, 작품: ${CUR_WORK:-없음})"
  explain_state
  exit 130
}
trap on_exit EXIT
trap on_int INT TERM

log()  { printf '%s %s\n' "$(date +%T)" "$*"; }
now()  { date +%s%N; }
ms()   { echo $(( ($(now) - $1) / 1000000 )); }
rate() { awk -v b="$1" -v m="$2" 'BEGIN{ if (m < 1) m = 1; printf "%.1f MB/s", b / 1048576 / (m / 1000) }'; }
hsize(){ awk -v b="$1" 'BEGIN{ if (b>=1048576) printf "%.1f MB", b/1048576; else printf "%.1f KB", b/1024 }'; }
jp()   { if [ "$1" = . ]; then printf '%s' "$2"; else printf '%s/%s' "$1" "$2"; fi; }   # 부모(.=작품 루트) + 이름

explain_state() {
  case $STAGE in
    precheck|backup)
      log "원본은 그대로입니다."
      [ -n "$CUR_BACKUP" ] && rm -rf -- "$CUR_BACKUP" && log "임시 백업 삭제: $CUR_BACKUP" ;;
    delete)
      log "원본 폴더 삭제 중 실패. SD 의 '$CUR_WORK' 상태를 확인하세요."
      log "검증된 백업(유지됨): $CUR_BACKUP"
      log "복구: bash $0 --restore '$CUR_BACKUP'" ;;
    mkdir|write|verify)
      log "원본 작품 폴더는 이미 삭제됨. 검증된 백업(유지됨): $CUR_BACKUP"
      log "복구: bash $0 --restore '$CUR_BACKUP'" ;;
    restore-check)
      log "SD 는 변경하지 않았습니다. 백업은 그대로: $CUR_BACKUP" ;;
  esac
}
fail() {
  log "오류 [$STAGE] ${CUR_WORK:+[$CUR_WORK] }$*"
  explain_state
  exit 1
}

# ---------------------------------------------------------------- SAF helpers
# 읽기 전용 호출은 가끔 빈 출력을 돌려주므로(실측) 최대 4번 재시도한다. 쓰기 계열은 재시도하지 않는다.
saf_ls() {   # stdout: JSON 배열. 실패 시 return 1
  local out i
  for i in 1 2 3 4 5 6; do
    out=$(termux-saf-ls "$1" 2>&1)
    jq -e 'type == "array"' >/dev/null 2>&1 <<<"$out" && { printf '%s' "$out"; return 0; }
    log "ls 재시도 $i: $(printf '%q' "${out:0:120}")" >&2
    sleep $(( i < 3 ? i : 3 ))
  done
  return 1
}
# createDocument/mkdir 가 돌려준 URI 의 마지막 %2F 뒤가 실제로 붙은 이름이다 (같은 이름이 있으면 "z (1).mp3" 가 된다).
# 그 이름을 URI 에서 바로 읽어 stat 호출(약 1 초)을 줄인다. 읽을 수 없으면 빈 값.
uri_name() {
  local t=${1##*%2F}
  [ "$t" != "$1" ] && [ -n "$t" ] || return 1
  printf '%b' "${t//%/\\x}"
}
saf_stat_name() {   # $1 URI  $2 부모 폴더 URI. stdout: 실제 이름 (stat, 안 되면 부모 ls 로 확인). 실패 시 빈 문자열
  local out i
  for i in 1 2 3 4 5 6; do
    out=$(termux-saf-stat "$1" 2>&1)
    jq -e 'type == "object" and has("name")' >/dev/null 2>&1 <<<"$out" && { jq -r .name <<<"$out"; return 0; }
    out=$(termux-saf-ls "$2" 2>&1)
    if jq -e 'type == "array"' >/dev/null 2>&1 <<<"$out"; then
      out=$(jq -r --arg u "$1" '.[] | select(.uri == $u) | .name' <<<"$out")
      [ -n "$out" ] && { printf '%s\n' "$out"; return 0; }
    fi
    log "이름 확인 재시도 $i" >&2
    sleep $(( i < 3 ? i : 3 ))
  done
}
saf_sha() {   # 다시 읽어서 sha256. 기대값($2)과 다르면 최대 3번 다시 읽는다 (읽기 전용)
  local sha i
  for i in 1 2 3; do
    sha=$(termux-saf-read "$1" | sha256sum | cut -d' ' -f1)
    [ "$sha" = "$2" ] && break
    sleep "$i"
  done
  printf '%s' "$sha"
}

for _i in 1 2 3 4; do DIRS_OUT=$(termux-saf-dirs 2>/dev/null); jq -e 'type == "array"' >/dev/null 2>&1 <<<"$DIRS_OUT" && break; sleep "$_i"; done
ROOT=$(jq -r --arg n "$ROOT_NAME" \
        '[.[] | select(.name == $n)] | if length == 1 then .[0].uri else empty end' <<<"$DIRS_OUT" 2>/dev/null)
[ -n "$ROOT" ] || { log "termux-saf-dirs 에서 '$ROOT_NAME' 를 하나로 찾을 수 없음 (termux-saf-managedir 로 권한 부여)"; exit 1; }

# ---------------------------------------------------------------- 스캔 (1회, 하위 폴더까지)
# 폴더 깊이별로 한 단계씩(wave) 병렬 ls. 결과는 폴더 노드 {w, path, uri, entries} 목록.
scan() {
  local t0 rl tmp nw wave k u w p total=0 level=0
  local -a pids lines
  t0=$(now)
  log "스캔 시작: $ROOT_NAME (하위 폴더 포함)"
  rl=$(saf_ls "$ROOT") || { log "루트 ls 실패"; exit 1; }
  tmp=$(mktemp -d "$TMPDIR/snowsky_scan.XXXXXX")
  jq --arg d "$DIR_MIME" '[.[] | select(.type == $d) | {name, uri}]' <<<"$rl" > "$tmp/works.json"
  nw=$(jq length "$tmp/works.json")
  log "  루트 읽기 완료: 항목 $(jq length <<<"$rl")개, 작품 폴더 ${nw}개 ($(ms "$t0") ms). 작품 폴더 안을 읽는 중… (병렬 $JOBS)"
  : > "$tmp/nodes.jsonl"
  jq -r 'to_entries[] | [.key, ".", .value.uri] | @tsv' "$tmp/works.json" > "$tmp/wave.tsv"
  while [ -s "$tmp/wave.tsv" ]; do
    level=$((level + 1))
    mapfile -t lines < "$tmp/wave.tsv"
    pids=()
    # 맨 wait 는 tee 프로세스 치환까지 기다리므로 PID 로만 기다린다.
    for ((k = 0; k < ${#lines[@]}; k++)); do
      IFS=$'\t' read -r w p u <<<"${lines[k]}"
      ( termux-saf-ls "$u" > "$tmp/r$k.json" 2>&1 ) &
      pids[k]=$!
      [ "$k" -ge "$JOBS" ] && wait "${pids[k - JOBS]}"
      [ $(( (k + 1) % 10 )) -eq 0 ] && log "  깊이 $((level)): $((k + 1)) / ${#lines[@]} 폴더 읽는 중…"
    done
    for u in "${pids[@]}"; do wait "$u"; done
    : > "$tmp/next.tsv"
    for ((k = 0; k < ${#lines[@]}; k++)); do
      IFS=$'\t' read -r w p u <<<"${lines[k]}"
      if ! jq -e 'type == "array"' "$tmp/r$k.json" >/dev/null 2>&1; then   # 한 번 순차 재시도
        saf_ls "$u" > "$tmp/r$k.json" || { rm -rf "$tmp"; log "폴더 ls 실패 — 스캔 중단"; exit 1; }
      fi
      jq -c --argjson w "$w" --arg p "$p" --arg u "$u" \
        '{w: $w, path: (if $p == "." then "" else $p end), uri: $u, entries: (to_entries | map(.value + {idx: .key}))}' "$tmp/r$k.json" >> "$tmp/nodes.jsonl"
      jq -r --argjson w "$w" --arg p "$p" --arg d "$DIR_MIME" \
        '.[] | select(.type == $d) | [$w, (if $p == "." then .name else $p + "/" + .name end), .uri] | @tsv' \
        "$tmp/r$k.json" >> "$tmp/next.tsv"
    done
    total=$((total + ${#lines[@]}))
    log "  깊이 $level: 폴더 ${#lines[@]}개 (누적 $total)"
    mv "$tmp/next.tsv" "$tmp/wave.tsv"
  done
  jq -s --slurpfile W "$tmp/works.json" --arg root "$ROOT" --arg at "$(date '+%F %T')" '
      . as $n
      | { root: $root, scanned_at: $at,
          works: [ $W[0] | to_entries[] | .key as $i
                   | { name: .value.name, uri: .value.uri, pos: $i,
                       dirs: [ $n[] | select(.w == $i) | del(.w) ] } ] }
    ' "$tmp/nodes.jsonl" > "$MANIFEST.tmp" && mv "$MANIFEST.tmp" "$MANIFEST" \
    || { rm -rf "$tmp"; log "manifest 저장 실패"; exit 1; }
  rm -rf "$tmp"
  log "스캔 완료: 작품 $nw 개, 폴더 $total 개, SAF ls $((total + 1))회, $(ms "$t0") ms → $MANIFEST"
}

update_manifest() {   # $1 작품명 $2 새 URI $3 폴더 노드 JSONL 파일
  jq --arg n "$1" --arg u "$2" --slurpfile N "$3" '
    { name: $n, uri: $u, dirs: $N, pos: (([.works[].pos] | max // -1) + 1) } as $w
    | .works |= (if any(.[]; .name == $n) then map(if .name == $n then $w else . end) else . + [$w] end)
  ' "$MANIFEST" > "$MANIFEST.tmp" && mv "$MANIFEST.tmp" "$MANIFEST"
}

# ---------------------------------------------------------------- 계획 (로컬 계산만)
COMMON_JQ='
def fw: explode | map(if . >= 65296 and . <= 65305 then . - 65248 else . end) | implode;
def ismp3: .name | ascii_downcase | endswith(".mp3");
def jp($p; $n): if $p == "" then $n else $p + "/" + $n end;
def norm: fw | ascii_downcase | sub("\\.mp3$"; "");
def hasnum: .name | norm | test("[0-9]");   # 확장자 ".mp3" 의 3 은 세지 않는다
# 자연 정렬: 한 글자씩 비교하되 연속된 숫자는 하나의 수로 비교한다 (#2 < #10, 01 < 2, RJ…_3 < RJ…_12).
# 번호가 이름 앞이 아니어도 된다 ("#1.", "Track 01", "【01】", "第1話", "RJ01062161_01" …).
# 숫자 덩어리는 글자 "0" 자리에서 비교되므로 공백·"-"·"." 보다는 뒤, 글자·한자·한글보다는 앞에 온다
# ("QUEST" < "QUEST2", "배구 - …" < "배구 2 …", "track01" < "trackEX1", "02_" < "反転01_").
def nkeyof: [scan("[0-9]+|[^0-9]") | if test("^[0-9]") then [48, tonumber] else [explode[0], 0] end];
def nkey: .name | norm | nkeyof;
def okey: [nkey, .name];
# 폴더(작품·하위 폴더)는 RJ 코드를 빼고 비교한다. RJ 코드의 숫자가 제목의 일부처럼 비교되면
# "SEXTHEEND2RJ…" 가 "SEXTHEENDRJ…"(1편) 보다 앞에 온다.
def nork: gsub("[rv]j[0-9]{6,8}"; "");
# 하위 폴더: RJ 코드만 빼고 자연 정렬
def fkey: [(.name | norm | nork | nkeyof), (.name | norm | nkeyof), .name];
# 작품(Asmr 바로 아래): 【태그】·[태그] 로 시작하는 작품을 먼저, 태그 글자 기준 가나다순.
# 그다음 태그 없는 작품을 가나다순 (앞머리 기호 ♥ 「 - 등은 비교에서 뺀다).
# 예: "[Cb]…", "[Ab]…", "가나다" → "[Ab]…", "[Cb]…", "가나다". 태그가 끝나는 자리는 가장 앞 글자로
# 취급해서 짧은 태그가 먼저 온다: 【저음】 → 【저음 이케보】 → 【저음 이케보 마마 마요정】.
# (비교 기준일 뿐, 이름은 그대로다.)
def wkey: (.name | norm) as $n
  | if ($n | test("^\\s*[【\\[]"))
    then [0, ($n | sub("^\\s*[【\\[]"; "") | gsub("[】\\]]"; "\u0001") | nork | nkeyof), ($n | nkeyof), .name]
    else [1, ($n | sub("^[^\\p{L}\\p{N}]+"; "") | nork | nkeyof), ($n | nkeyof), .name] end;
# 트랙: 공통 앞부분을 뺀 이름이 숫자로 시작하면 그 번호가 먼저 결정하고, 번호가 같으면
# 프롤로그(プロローグ/프롤로그/prologue)가 먼저 온다. 예: "1）禁断…" 와 "01：프롤로그" → 프롤로그 먼저.
def tkey($pre): (.name | norm) as $n
  | ($n | if startswith($pre) then .[($pre | length):] else . end | [match("^[0-9]+")] | first | .string) as $d
  | [ (if $d then [0, ($d | tonumber), (if ($n | test("プロローグ|프롤로그|prologue")) then 0 else 1 end)] else [1] end),
      nkey, .name ];
# 이름이 "." 으로 시작하는 파일(.__CONVERTING__ 같은 임시/숨김 파일)은 맨 뒤
def hidden: .name | startswith(".");
# 같은 폴더 MP3 들의 공통 앞부분(끝의 숫자는 제외)을 뺀 뒤 처음 나오는 숫자 = 화면에 보여 줄 트랙 번호
def lcp: if length == 0 then "" else
           reduce .[1:][] as $s (.[0];
             . as $p | ([($p | length), ($s | length)] | min) as $m
             | ([range(0; $m) | select($p[.:.+1] != $s[.:.+1])] | first // $m) as $i | $p[:$i])
         end | sub("[0-9]+$"; "");
def tracklabel($pre): (.name | norm | gsub("(?<c>[①-⑳])"; (.c | explode[0] - 9311 | tostring) + " ")) as $n
  | (if ($n | startswith($pre)) then $n[($pre | length):] else $n end)
  | ([scan("[0-9]+")] | first) // null;
'
PLAN_JQ=$COMMON_JQ'
def dirplan:
  .path as $p
  | (.entries | sort_by(.idx)) as $e
  | [$e[] | select(.type == $dir)] as $dirs
  | [$e[] | select(.type != $dir and ismp3)] as $mp30
  | ([$mp30[] | select(hidden | not) | .name | norm] | lcp) as $pre
  | [$mp30[] | . + {tok: (if hidden then null else tracklabel($pre) end)}] as $mp3
  | ([$mp3[] | select((hidden | not) and hasnum)] | sort_by(tkey($pre))) as $numd
  | ([$mp3[] | select((hidden | not) and (hasnum | not))] | sort_by(okey)) as $unnum
  | ([$mp3[] | select(hidden)] | sort_by(.name)) as $hid
  | [$e[] | select(.type != $dir and (ismp3 | not))] as $others
  | ($dirs | sort_by(fkey)) as $sdirs
  | ($numd + $unnum + $hid) as $pm
  | { path: $p, uri: .uri,
      mp3ok: ([$mp3[].name] == [$pm[].name]),
      dirok: ([$dirs[].name] == [$sdirs[].name]),
      current: [$mp3[] | .tok // .name],
      curdirs: [$dirs[].name],
      mp3: [$pm[].name], others: [$others[].name], subdirs: [$sdirs[].name],
      files: [($pm + $others)[] | {name, uri, type: (.type // ""), length: (.length // 0), label: jp($p; .tok // .name)}],
      nmp3: ($mp3 | length),
      bad: ([$e[] | select(.name | test("[\\t\\n\\r\\\\]"))] | length > 0),
      warnings: ( [$unnum[] | "숫자 없음 (이름순으로 뒤에 배치): " + jp($p; .name)]
                + [$hid[] | "\".\" 으로 시작하는 임시/숨김 파일로 보임 (맨 뒤에 배치, 그대로 복사): " + jp($p; .name)]
                + [$numd | map(select(.tok != null)) | group_by(.tok | tonumber)[] | select(length > 1)
                   | "같은 번호 \(.[0].tok | tonumber) 여러 개 (시점·버전 차이면 정상 — 번갈아 배치, 짧은 이름 먼저): " + (map(jp($p; .name)) | join(", "))] ) };
.works | sort_by(.name) | map(
  . as $w
  | [$w.dirs[] | dirplan] as $dp
  | ($dp | map({key: .path, value: .}) | from_entries) as $by
  | def ops($p): $by[$p] as $d
      | ($d.files[] | {op: "f", parent: $p} + .),
        ($d.subdirs[] as $s | {op: "d", parent: $p, name: $s, label: (jp($p; $s) + "/")}, ops(jp($p; $s)));
    [ops("")] as $ops
  | ([$dp[] | select(.bad)] | length > 0) as $bad
  | ([$dp[].nmp3] | add // 0) as $nm
  | { name: $w.name, uri: $w.uri, pos: $w.pos,
      dirs: ([ "", ($ops[] | select(.op == "d") | jp(.parent; .name)) ] | map(. as $q | $by[$q])),
      ops: $ops, nmp3: $nm,
      warnings: ((if $bad then ["파일명에 탭/줄바꿈/역슬래시 → 건너뜀"] else [] end) + [$dp[].warnings[]]),
      status: (if $bad then "skip" elif $nm == 0 then "nomp3" elif all($dp[]; .mp3ok and .dirok) then "ok" else "reorder" end),
      nfiles: ([$ops[] | select(.op == "f")] | length),
      ndirs: ([$ops[] | select(.op == "d")] | length),
      bytes: ([$ops[] | select(.op == "f") | .length] | add // 0) }
)'

DETAIL_JQ=$COMMON_JQ'
def st: {"reorder": "재정렬 필요", "ok": "이미 트랙 순서대로임", "skip": "건너뜀", "nomp3": "MP3 없음"}[.status];
.[] | . as $w |
  "[\(.name)]",
  "상태: \(st)  (파일 \(.nfiles)개, 하위 폴더 \(.ndirs)개)",
  ( .dirs[] |
      (if .path == "" then empty else "  [\($w.name)/\(.path)]" end),
      (if .mp3ok then empty else "  현재 MP3 순서: " + (.current | join(" → ")) end),
      (if .dirok then empty else "  현재 하위 폴더 순서: " + (.curdirs | join(" → ")) end),
      (.mp3 | to_entries[] | "  \(.key + 1). \(.value)"),
      "  기타 파일: " + (if (.others | length) == 0 then "없음" else (.others | join(", ")) end),
      (if (.subdirs | length) == 0 then empty else "  하위 폴더: " + (.subdirs | join(", ")) end) ),
  "생성 순서: " + ([.ops[].label] | join(" → ")),
  "경고: " + (if (.warnings | length) == 0 then "없음" else (.warnings | join("\n      ")) end),
  ""'

# ---------------------------------------------------------------- 쓰기 단계 (백업 → SD)
# $1 = 검증된 백업 디렉터리 (meta.json, index.tsv, NNNN.bin, .complete)
# index.tsv: seq kind(f|d) parent(.=작품 루트) name type length sha   (빈 값은 -)
write_phase() {
  local B=$1 rw name nw nm n nf k t0 tw seq kind parent fname ftype flen fsha u L got want uri sha key j bytes=0
  local -A DU=() DLS=()
  name=$(jq -r .work "$B/meta.json")
  n=$(wc -l < "$B/index.tsv")
  nf=$(awk -F'\t' '$2 == "f"' "$B/index.tsv" | wc -l)
  t0=$(now)

  STAGE=mkdir
  make_work_folder "$name"
  nw=$NEW_WORK_URI
  DU[.]=$nw
  start_prep   # (--yes) 다음 작품의 원본 삭제와 빈자리 메우기를 이 작품의 파일을 쓰는 동안 미리 한다

  STAGE=write
  local tw0; tw0=$(now)
  k=0
  while IFS=$'\t' read -r -u 3 seq kind parent fname ftype flen fsha; do
    k=$((k + 1)); tw=$(now)
    [ -n "${DU[$parent]:-}" ] || fail "부모 폴더 URI 없음: $parent"
    key=$(jp "$parent" "$fname")
    if [ "$kind" = d ]; then
      u=$(termux-saf-mkdir "${DU[$parent]}" "$fname" 2>&1)
      [[ $u == content://* ]] || fail "mkdir 실패 ($key): ${u:0:200}"
      nm=$(uri_name "$u") || nm=""
      [ -n "$nm" ] || nm=$(saf_stat_name "$u" "${DU[$parent]}")
      [ "$nm" = "$fname" ] || fail "하위 폴더 이름이 '$nm' 로 생성됨 (원래: '$fname')"
      DU[$key]=$u
      log "  [$k/$n] 폴더 $key/"
    else
      [ "$ftype" = - ] && ftype=application/octet-stream
      u=$(termux-saf-create -t "$ftype" "${DU[$parent]}" "$fname" 2>&1)
      [[ $u == content://* ]] || fail "create 실패 ($key): ${u:0:200}"
      termux-saf-write "$u" < "$B/$(printf %05d "$seq").bin" || fail "write 실패 ($key)"
      bytes=$((bytes + flen))
      log "  [$k/$n] 생성 $key ($(hsize "$flen"), $(ms "$tw") ms)"
      [ "$GAP" != 0 ] && sleep "$GAP"
    fi
  done 3< "$B/index.tsv"
  [ "$k" = "$n" ] || fail "쓰기 단계가 $k/$n 항목에서 끝남 (목록을 끝까지 처리하지 못함)"

  log "  쓰기 완료: $(hsize "$bytes"), $(ms "$tw0") ms ($(rate "$bytes" "$(ms "$tw0")"))"
  STAGE=verify
  local tv0; tv0=$(now)
  : > "$B/final_nodes.jsonl"
  j=0
  for key in "${!DU[@]}"; do
    j=$((j + 1))
    L=$(saf_ls "${DU[$key]}") || fail "검증용 ls 실패 ($key)"
    printf '%s' "$L" > "$B/final_ls_$j.json"
    DLS[$key]=$B/final_ls_$j.json
    got=$(jq -c --arg d "$DIR_MIME" '[.[] | [(if .type == $d then "d" else "f" end), .name, (if .type == $d then 0 else (.length // 0) end)]]' <<<"$L")
    want=$(jq -Rsc --arg k "$key" 'split("\n") | map(select(length > 0) | split("\t") | select(.[2] == $k)
              | [.[1], .[3], (if .[1] == "d" then 0 else (.[5] | tonumber) end)])' < "$B/index.tsv")
    [ "$got" = "$want" ] || fail "엔트리 순서/이름/크기 불일치 ($key)
      기대: $want
      실제: $got"
    jq -c --arg p "$([ "$key" = . ] && echo "" || echo "$key")" --arg u "${DU[$key]}" \
      '{path: $p, uri: $u, entries: (to_entries | map(.value + {idx: .key}))}' <<<"$L" >> "$B/final_nodes.jsonl"
  done
  log "  엔트리 순서 확인 OK (폴더 ${#DU[@]}개)"
  # 다시 읽어 sha256 을 구하는 일은 IO_JOBS 개씩 동시에 한다 (읽기만 하므로 순서와 무관). 비교와 로그는 순서대로.
  local -a VF=() vp=()
  local nv=0 vj
  while IFS=$'\t' read -r -u 3 seq kind parent fname ftype flen fsha; do
    [ "$kind" = f ] || continue
    key=$(jp "$parent" "$fname")
    uri=$(jq -r --arg n "$fname" '.[] | select(.name == $n) | .uri' "${DLS[$parent]}")
    [[ $uri == content://* ]] || fail "검증: URI 없음 ($key)"
    VF+=("$seq"$'\t'"$key"$'\t'"$uri"$'\t'"$fsha")
    if [ "$IO_JOBS" = 1 ]; then printf '%s' "$(saf_sha "$uri" "$fsha")" > "$B/vsha_$seq"
    else
      ( printf '%s' "$(saf_sha "$uri" "$fsha")" > "$B/vsha_$seq" ) &
      vp[nv]=$!
      [ "$nv" -ge "$IO_JOBS" ] && wait "${vp[nv - IO_JOBS]}"
    fi
    nv=$((nv + 1))
  done 3< "$B/index.tsv"
  for vj in "${vp[@]}"; do wait "$vj"; done
  k=0
  for vj in "${VF[@]}"; do
    IFS=$'\t' read -r seq key uri fsha <<<"$vj"
    k=$((k + 1))
    sha=$(cat "$B/vsha_$seq" 2>/dev/null); rm -f "$B/vsha_$seq"
    # SD 쓰기가 잘못 기록됐을 수 있다 → 같은 파일(같은 엔트리, 순서 그대로)에 백업을 다시 써서 최대 2번 재시도
    for rw in 1 2; do
      [ "$sha" = "$fsha" ] && break
      log "  경고: sha256 불일치 ($key) — 백업에서 다시 쓰기 $rw/2 (SD 쓰기 오류일 수 있음)"
      termux-saf-write "$uri" < "$B/$(printf %05d "$seq").bin" || fail "다시 쓰기 실패 ($key)"
      sha=$(saf_sha "$uri" "$fsha")
    done
    [ "$sha" = "$fsha" ] || fail "sha256 불일치 ($key): 원본 $fsha / 새 파일 $sha — 다시 써도 틀림. SD 카드 상태를 확인하세요"
    log "  [$k/$nf] 검증 OK $key"
  done
  [ "$k" = "$nf" ] || fail "검증 단계가 $k/$nf 파일에서 끝남"

  update_manifest "$name" "$nw" "$B/final_nodes.jsonl" || log "경고: manifest 갱신 실패 (다음 실행 시 --rescan 권장)"
  STAGE=done
  rm -rf -- "$B"; CUR_BACKUP=""
  log "  검증 완료: $(ms "$tv0") ms ($(rate "$bytes" "$(ms "$tv0")"))"
  log "  완료: 파일 $nf 개 + 폴더 $(( ${#DU[@]} - 1 )) 개, $(hsize "$bytes"), 쓰기+검증 $(ms "$t0") ms — 백업 삭제함"
}


# ---------------------------------------------------------------- 루트 배치 (작품 순서)
# 패드: 아주 짧은 이름의 빈 파일. 앞쪽 빈 엔트리를 메우는 용도이며 실행이 끝나면 모두 지운다.
# 짧은 이름은 어떤 빈자리에도 들어가므로, 패드가 맨 뒤에 생겼다면 그보다 앞에는 (더 긴 이름이
# 들어갈 수 있는) 빈자리가 없다는 뜻이다.
# 패드를 n 개 만든다 (루트 ls 없음). 패드는 이름만 서로 다르면 되고 순서가 상관없어서 JOBS 개씩 동시에 만든다.
make_pads() {   # $1 = 개수  $2 = 동시에 만들 수 (기본 PAD_JOBS)
  local n=$1 pj=${2:-$PAD_JOBS} k name u bad=0 tmp
  local -a pids=()
  if [ "$pj" -le 1 ]; then   # 하나씩 (추가 프로세스 없음)
    for ((k = 0; k < n; k++)); do
      PADN=$((PADN + 1)); name=$(printf "$PAD_FMT" "$PADN")
      u=$(termux-saf-create -t application/octet-stream "$ROOT" "$name" 2>&1)
      [[ $u == content://* ]] || fail "패드 생성 실패: ${u:0:200}"
      printf '%s\n' "$u" >> "$PADS_FILE"
    done
    return 0
  fi
  tmp=$(mktemp -d "$TMPDIR/snowsky_pads.XXXXXX") || fail "임시 폴더 생성 실패"
  for ((k = 0; k < n; k++)); do
    PADN=$((PADN + 1)); name=$(printf "$PAD_FMT" "$PADN")
    ( termux-saf-create -t application/octet-stream "$ROOT" "$name" > "$tmp/$k" 2>&1 < /dev/null ) &
    pids[k]=$!
    [ "$k" -ge "$pj" ] && wait "${pids[k - pj]}"
  done
  for u in "${pids[@]}"; do wait "$u"; done
  for ((k = 0; k < n; k++)); do   # 성공한 패드는 모두 기록해 둔다 (정리할 때 필요)
    u=$(cat "$tmp/$k")
    if [[ $u == content://* ]]; then printf '%s\n' "$u" >> "$PADS_FILE"; else bad=$((bad + 1)); log "  패드 동시 생성 실패 1건: ${u:0:100}"; fi
  done
  rm -rf -- "$tmp"
  # 동시에 만들다 실패한 것은 새 이름으로 하나씩 다시 만든다. (실패한 쪽이 실제로는 만들어졌더라도
  # 다음 실행의 prepare_pads 가 이름 패턴으로 찾아 지우므로 남지 않는다)
  for ((k = 0; k < bad; k++)); do
    PADN=$((PADN + 1)); name=$(printf "$PAD_FMT" "$PADN")
    u=$(termux-saf-create -t application/octet-stream "$ROOT" "$name" 2>&1)
    [[ $u == content://* ]] || fail "패드 생성 실패: ${u:0:200}"
    printf '%s\n' "$u" >> "$PADS_FILE"
  done
}

# 앞쪽 빈자리를 모두 막는다: 패드를 만들고 루트 ls 로 맨 뒤가 패드인지 확인 (처음이거나 확인에 실패했을 때만 쓴다)
plug_holes() {   # $1 = 처음 한 번에 만들 패드 수
  local total=0 b=${1:-1} last
  [ "$b" -ge 1 ] && [ "$b" -le 64 ] || b=1
  while :; do
    log "  패드 $b 개 만드는 중… (누적 $((total + b)))"
    make_pads "$b" 2; total=$((total + b))
    # 맨 뒤가 패드면 그보다 앞에는 빈자리가 없다 (패드는 어떤 빈자리에도 들어가는 가장 짧은 이름)
    last=$(saf_ls "$ROOT" | jq -r '.[-1].name') || fail "루트 ls 실패"
    [[ $last =~ $PAD_RE ]] && break
    [ "$total" -ge 5000 ] && fail "빈자리 채우기가 끝나지 않음 (패드 $total 개)"
    b=$(( b < 64 ? b * 2 : 64 ))
  done
  log "  루트 빈자리 채움: 패드 $total 개"
  return 0
}

# 이미 빈자리가 막혀 있으면(패드가 남아 있는 상태) 지운 폴더 자리만 메우면 되므로 한 번에 만들고 루트 ls 는 폴더를 만든 뒤 한 번만 한다.
FAST_PLUG=0
# 작품 폴더를 만든다. 작품 순서 모드에서는 루트 맨 뒤에 생겼는지 확인한다. 결과: NEW_WORK_URI
make_work_folder() {
  local name=$1 try nw nm last tp tl hint pre=$PRE_DONE
  PRE_DONE=0
  hint=$(( (${#name} + 12) / 13 + 2 ))   # 긴 이름은 13 글자마다 한 칸 + 1 칸을 쓴다(FAT LFN)
  for try in 1 2 3 4; do
    if [ "$TRACKS_ONLY" != 1 ] && ! { [ "$pre" = 1 ] && [ "$try" = 1 ]; }; then
      tp=$(now)
      if [ "$FAST_PLUG" = 1 ] && [ "$try" = 1 ]; then
        log "  지운 폴더 자리 메우는 중: 패드 $hint 개"
        make_pads "$hint"
      else
        plug_holes "$hint"
      fi
      log "  빈자리 확인: $(ms "$tp") ms"
    fi
    nw=$(termux-saf-mkdir "$ROOT" "$name" 2>&1)
    [[ $nw == content://* ]] || fail "mkdir 실패: ${nw:0:200}"
    nm=$(uri_name "$nw") || nm=""
    [ -n "$nm" ] || nm=$(saf_stat_name "$nw" "$ROOT")
    [ "$nm" = "$name" ] || fail "새 폴더 이름이 '$nm' 로 생성됨 (원래: '$name') — 같은 이름이 남아 있는지 확인"
    if [ "$TRACKS_ONLY" = 1 ]; then break; fi
    tl=$(now)
    last=$(saf_ls "$ROOT" | jq -r '.[-1].name') || fail "루트 ls 실패"
    log "  루트 위치 확인: $(ms "$tl") ms"
    [ "$last" = "$name" ] && { FAST_PLUG=1; break; }
    # 맨 뒤가 아님 → 방금 만든 빈 폴더만 지우고, 빈자리를 처음부터 다시 확인하며 재시도
    [ "$(saf_ls "$nw" | jq length)" = 0 ] || fail "방금 만든 폴더가 비어 있지 않음"
    log "  새 폴더가 루트 맨 뒤가 아님 (맨 뒤: $last) → 빈 폴더 삭제 후 재시도 $try"
    rm_checked "$nw" "$name" "$ROOT" || fail "재시도용 빈 폴더 삭제 실패"
    [ "$try" = 4 ] && fail "새 폴더를 루트 맨 뒤에 만들 수 없음"
  done
  NEW_WORK_URI=$nw
  log "  새 폴더 생성: $name"
}

# 폴더/파일 삭제. termux-saf-rm 은 결과를 종료 코드로 쓰는 셸 스크립트라서, termux-api 가 숫자가 아닌 값(빈 출력)을
# 돌려주면 "exit: Illegal number" 와 함께 rc=2 로 끝난다 — 실제로는 지워졌을 수도 있다.
# 그래서 rc 가 0 이 아니면 부모 폴더 목록으로 실제로 사라졌는지 확인하고, 남아 있으면 다시 지운다.
rm_checked() {   # $1 URI  $2 이름  $3 부모 URI
  local try rc L
  for try in 1 2 3; do
    termux-saf-rm "$1" > /dev/null 2>&1; rc=$?
    [ "$rc" = 0 ] && return 0
    log "  삭제 명령이 rc=$rc 를 돌려줌 → 실제로 지워졌는지 확인합니다 ($try/3)"
    if L=$(saf_ls "$3"); then
      if ! jq -e --arg n "$2" 'any(.[]; .name == $n)' <<<"$L" >/dev/null; then
        log "  폴더가 이미 없음 → 삭제된 것으로 처리"; return 0
      fi
    fi
    sleep $((try * 3))
  done
  return 1
}

# 이전 실행이 남긴 패드(이름 ~P0000 형식, 0 바이트 파일)와 이번 실행의 패드를 지운다.
cleanup_pads() {
  local n=0 u
  [ -n "$PADS_FILE" ] && [ -f "$PADS_FILE" ] || return 0
  while IFS= read -r -u 3 u; do
    [ -n "$u" ] && termux-saf-rm "$u" >/dev/null 2>&1 && n=$((n + 1))
  done 3< "$PADS_FILE"
  rm -f "$PADS_FILE"
  [ "$n" -gt 0 ] && log "패드 $n 개 삭제"
  return 0
}
prepare_pads() {   # 이전 실행이 남긴 패드: --keep-pads 면 그대로 두고(이미 빈자리를 막고 있음), 아니면 삭제
  local L n
  PADS_FILE=$CACHE_DIR/pads.$$; : > "$PADS_FILE"
  L=$(saf_ls "$ROOT") || return 0
  L=$(jq -c --arg re "$PAD_RE" '[.[] | select((.name | test($re)) and (.length // 0) == 0 and .type != "vnd.android.document/directory")]' <<<"$L")
  n=$(jq length <<<"$L")
  PADN=$(jq '[.[].name[2:] | tonumber] | max // 0' <<<"$L")
  [ "$n" -gt 0 ] || return 0
  jq -r '.[].uri' <<<"$L" >> "$PADS_FILE"
  if [ "$KEEP_PADS" != 0 ]; then FAST_PLUG=1; log "이전 실행의 패드 $n 개를 그대로 사용 (앞쪽 빈자리가 이미 막혀 있음)"
  else cleanup_pads; PADS_FILE=$CACHE_DIR/pads.$$; : > "$PADS_FILE"; fi
}

# ---------------------------------------------------------------- 작품 하나 처리 (방식 A)
# 작품 폴더 하나를 (하위 폴더까지) 다시 읽어 스캔과 같은 모양의 노드 JSONL 로 쓴다.
scan_work() {   # $1 작품 URI  $2 출력 JSONL
  local q="$2.q" nx="$2.next" path u L
  : > "$2"; printf '.\t%s\n' "$1" > "$q"
  while [ -s "$q" ]; do
    : > "$nx"
    while IFS=$'\t' read -r -u 3 path u; do
      L=$(saf_ls "$u") || return 1
      jq -c --arg p "$path" --arg u "$u" \
        '{path: (if $p == "." then "" else $p end), uri: $u, entries: (to_entries | map(.value + {idx: .key}))}' <<<"$L" >> "$2"
      jq -r --arg p "$path" --arg d "$DIR_MIME" \
        '.[] | select(.type == $d) | [(if $p == "." then .name else $p + "/" + .name end), .uri] | @tsv' <<<"$L" >> "$nx"
    done 3< "$q"
    mv "$nx" "$q"
  done
  rm -f "$q"
}

# 스캔 이후 작품 폴더가 바뀐 경우(예: 이미지 파일을 추가함): 그 작품만 다시 읽어 계획을 갱신한다.
# 파일이 추가된 것만 받아들인다. 삭제됐거나 크기가 바뀐 파일이 있으면 원본을 건드리기 전에 멈춘다.
refresh_work() {   # P 를 갱신한다 (backup_work 의 지역 변수), $B.nodes.jsonl 에 새 스캔을 남긴다
  local nodes="$B.nodes.jsonl" old new gone added
  log "  폴더 내용이 스캔 이후 바뀜 → 이 작품만 다시 읽습니다"
  scan_work "$(jq -r .uri <<<"$P")" "$nodes" || fail "작품 다시 읽기 실패 (원본 변경 없음)"
  old=$(jq -c --arg n "$name" --arg d "$DIR_MIME" '.works[] | select(.name == $n)
        | [.dirs[] as $x | $x.entries[] | [$x.path, .name, (if .type == $d then "d" else ((.length // 0) | tostring) end)]] | sort' "$MANIFEST")
  new=$(jq -sc --arg d "$DIR_MIME" '[.[] as $x | $x.entries[] | [$x.path, .name, (if .type == $d then "d" else ((.length // 0) | tostring) end)]] | sort' "$nodes")
  gone=$(jq -nc --argjson o "$old" --argjson n "$new" '[$o[] | select(. as $e | ($n | index([$e])) == null)]')
  added=$(jq -nc --argjson o "$old" --argjson n "$new" '[$n[] | select(. as $e | ($o | index([$e])) == null)]')
  [ "$gone" = "[]" ] || fail "스캔 이후 삭제되었거나 크기가 바뀐 항목이 있음: $(jq -r 'map(if .[0] == "" then .[1] else .[0] + "/" + .[1] end) | join(", ")' <<<"$gone" | cut -c1-300) — 직접 확인 후 --rescan (원본 변경 없음)"
  log "  새로 생긴 항목 $(jq length <<<"$added")개: $(jq -r 'map((if .[0] == "" then "" else .[0] + "/" end) + .[1]) | join(", ")' <<<"$added" | cut -c1-300)"
  jq -n --arg n "$name" --arg u "$(jq -r .uri <<<"$P")" --argjson pos "$(jq .pos <<<"$P")" --slurpfile N "$nodes" \
    '{works: [{name: $n, uri: $u, pos: $pos, dirs: $N}]}' > "$B.mini.json" || fail "계획 갱신 실패"
  P=$(jq -c --arg dir "$DIR_MIME" "$PLAN_JQ" "$B.mini.json" | jq -c '.[0]') || fail "계획 갱신 실패"
  [ -n "$P" ] && [ "$P" != null ] || fail "계획 갱신 실패 (원본 변경 없음)"
  rm -f "$B.mini.json"
}
# 갱신된 작품 정보를 매니페스트에 반영한다 (위치 pos 는 유지)
update_manifest_keep() {   # $1 작품명 $2 노드 JSONL
  jq --arg n "$1" --slurpfile N "$2" '.works |= map(if .name == $n then (. as $o | {name: $n, uri: $o.uri, pos: $o.pos, dirs: $N}) else . end)' \
    "$MANIFEST" > "$MANIFEST.tmp$$" && mv "$MANIFEST.tmp$$" "$MANIFEST"
}

# 사전 확인 + 백업. SD 는 읽기만 하므로 다른 작품을 쓰는 동안 서브셸에서 미리 돌릴 수 있다.
backup_work() {   # $1 = plan 객체(JSON)  $2 = 백업 디렉터리
  local P=$1 B=$2 name need avail i kind parent fname furi ftype flen f sha lsha lsz t0 got want u key nd=0 changed=0
  name=$(jq -r .name <<<"$P")
  CUR_WORK=$name CUR_BACKUP=""
  t0=$(now)

  STAGE=precheck
  while IFS=$'\t' read -r -u 3 key u; do   # 작품 안 모든 폴더를 다시 ls 해서 스캔 결과와 비교
    nd=$((nd + 1))
    got=$(saf_ls "$u" | jq -c '[.[] | [.name, .uri, (.length // 0)]] | sort') || fail "폴더 ls 실패 ($name/$key)"
    want=$(jq -c --arg n "$name" --arg k "$key" \
      '($k | if . == "." then "" else . end) as $k | .works[] | select(.name == $n) | .dirs[] | select(.path == $k) | [.entries[] | [.name, .uri, (.length // 0)]] | sort' "$MANIFEST")
    [ "$got" = "$want" ] || { changed=1; break; }
  done 3< <(jq -r '.dirs[] | [(if .path == "" then "." else .path end), .uri] | @tsv' <<<"$P")
  [ "$changed" = 1 ] || [ "$nd" = "$(jq '.dirs | length' <<<"$P")" ] || fail "사전 확인이 폴더 $nd 개에서 끝남 (원본 변경 없음)"
  [ "$changed" = 1 ] && refresh_work
  need=$(( $(jq .bytes <<<"$P") / 1024 + 200 * 1024 ))
  avail=$(df -Pk "$TMPDIR" | awk 'NR == 2 { print $4 }')
  [ "$avail" -gt "$need" ] || fail "\$TMPDIR 여유 공간 부족: 필요 ${need} KB, 여유 ${avail} KB"
  log "  사전 확인 OK (폴더 $nd 개 재확인)"

  STAGE=backup
  mkdir -p "$B" || fail "백업 폴더 생성 실패"
  CUR_BACKUP=$B
  jq -n --arg w "$name" --arg at "$(date '+%F %T')" '{work: $w, created: $at}' > "$B/meta.json"
  if [ "$changed" = 1 ]; then printf '%s\n' "$P" > "$B/plan.json"; mv "$B.nodes.jsonl" "$B/refreshed.jsonl"; fi
  : > "$B/index.tsv"
  # 파일 읽기(SD 읽기 전용)는 IO_JOBS 개씩 동시에 한다. 목록(index.tsv)은 항상 계획 순서대로 만든다.
  local -a OPS=() pids=()
  local nfiles=0 sf
  while IFS=$'\t' read -r -u 3 kind parent fname furi ftype flen; do
    OPS+=("$kind"$'\t'"$parent"$'\t'"$fname"$'\t'"$furi"$'\t'"$ftype"$'\t'"$flen")
  done 3< <(jq -r '.ops[] | [.op, (if .parent == "" then "." else .parent end), .name, (.uri // "-"),
                            (if (.type // "") == "" then "-" else .type end), (.length // 0)] | @tsv' <<<"$P")
  i=0
  for op in "${OPS[@]}"; do
    i=$((i + 1))
    IFS=$'\t' read -r kind parent fname furi ftype flen <<<"$op"
    [ "$kind" = d ] && continue
    f=$B/$(printf %05d "$i").bin
    if [ "$IO_JOBS" = 1 ]; then   # 동시 읽기 없음: 추가 프로세스 없이 그 자리에서 읽는다
      sha=$(set -o pipefail; termux-saf-read "$furi" 2>/dev/null | tee "$f" | sha256sum | cut -d' ' -f1) && printf '%s' "$sha" > "$f.sha"
    else
      ( set -o pipefail; sha=$(termux-saf-read "$furi" 2>/dev/null | tee "$f" | sha256sum | cut -d' ' -f1) && printf '%s' "$sha" > "$f.sha" ) &
      pids[nfiles]=$!
      [ "$nfiles" -ge "$IO_JOBS" ] && wait "${pids[nfiles - IO_JOBS]}"
    fi
    nfiles=$((nfiles + 1))
  done
  for sf in "${pids[@]}"; do wait "$sf"; done
  i=0
  for op in "${OPS[@]}"; do
    i=$((i + 1))
    IFS=$'\t' read -r kind parent fname furi ftype flen <<<"$op"
    if [ "$kind" = d ]; then
      printf '%s\td\t%s\t%s\t-\t0\t-\n' "$i" "$parent" "$fname" >> "$B/index.tsv"
      continue
    fi
    key=$(jp "$parent" "$fname"); f=$B/$(printf %05d "$i").bin
    [ -s "$f.sha" ] || [ "$flen" = 0 ] || fail "읽기 실패 ($key) (원본 변경 없음)"
    sha=$(cat "$f.sha" 2>/dev/null); rm -f "$f.sha"
    lsz=$(stat -c %s "$f")
    [ "$lsz" = "$flen" ] || fail "읽은 크기 불일치 ($key): SD $flen / 로컬 $lsz"
    lsha=$(sha256sum < "$f" | cut -d' ' -f1)
    [ "$lsha" = "$sha" ] || fail "로컬 백업 sha256 불일치 ($key)"
    printf '%s\tf\t%s\t%s\t%s\t%s\t%s\n' "$i" "$parent" "$fname" "$ftype" "$flen" "$sha" >> "$B/index.tsv"
  done
  [ "$i" = "$(jq '.ops | length' <<<"$P")" ] || fail "백업이 $i/$(jq '.ops | length' <<<"$P") 항목에서 끝남 (원본 변경 없음)"
  [ "$(wc -l < "$B/index.tsv")" = "$i" ] || fail "백업 목록 줄 수 불일치 (원본 변경 없음)"
  touch "$B/.complete"
  log "  백업 완료: 항목 $i 개, $(hsize "$(jq .bytes <<<"$P")"), $(ms "$t0") ms ($(rate "$(jq .bytes <<<"$P")" "$(ms "$t0")")) → $B"

}

start_backup() {   # $1 plan  $2 백업 디렉터리 → BG_PID. 출력은 "$2.log" 에 모았다가 finish_backup 이 보여 준다
  ( trap - EXIT INT TERM; backup_work "$1" "$2" ) > "$2.log" 2>&1 &
  BG_PID=$!
}
finish_backup() {  # $1 pid  $2 백업 디렉터리
  local rc=0
  wait "$1" || rc=$?
  [ -f "$2.log" ] && cat "$2.log"
  rm -f "$2.log"
  [ "$rc" = 0 ] && [ -f "$2/.complete" ]
}
# 프로세스와 그 자식들을 모두 종료한다 (백업 작업은 여러 읽기 프로세스를 거느린다)
kill_tree() {
  local c
  for c in $(ps -A -o PID,PPID 2>/dev/null | awk -v p="$1" '$2 == p { print $1 }'); do kill_tree "$c"; done
  kill "$1" 2>/dev/null
  return 0
}
# 다음 작품 미리 읽기(백업) + (--yes 일 때) 미리 지우기. 백업은 SD 를 읽기만 하지만, 미리 지우기가 시작됐다면
# 그 작품의 원본은 이미 없을 수 있으므로 백업을 지우지 않고 복구 명령을 알려 준다.
PF_PID="" PF_B=""
kill_prefetch() {
  if [ -n "$PREP_PID" ]; then kill_tree "$PREP_PID"; wait "$PREP_PID" 2>/dev/null; PREP_PID=""; fi
  [ -n "$PF_PID" ] || return 0
  kill_tree "$PF_PID"; wait "$PF_PID" 2>/dev/null
  if [ -f "$PF_B/.deleting" ]; then
    log "※ 다음 작품의 원본 삭제가 이미 시작됐습니다. 검증된 백업을 유지합니다 → 복구: bash $0 --restore '$PF_B'"
  else
    rm -rf -- "$PF_B" "$PF_B.log" "$PF_B.prep.log" "$PF_B.nodes.jsonl" "$PF_B.mini.json"
    log "미리 읽던 다음 작품의 백업을 취소함 (원본은 그대로)"
  fi
  PF_PID="" PF_B=""
}

# 현재 작품의 폴더를 만든 뒤(맨 뒤에 놓인 뒤) 시작한다: 다음 작품의 백업이 끝나면 그 원본을 지우고 빈자리를 패드로 메워 둔다.
# 현재 작품의 파일을 쓰는 동안 일어나므로, 다음 작품 차례에는 폴더 생성과 위치 확인만 남는다.
start_prep() {
  [ "$YES" = 1 ] && [ "$NO_PREP" = 0 ] && [ -n "$PF_PID" ] && [ -n "$NEXT_P" ] || return 0
  PREP_B=$PF_B
  local nm uri hint pfpid=$PF_PID
  nm=$(jq -r .name <<<"$NEXT_P"); uri=$(jq -r .uri <<<"$NEXT_P"); hint=$(( (${#nm} + 12) / 13 + 2 ))
  (
    trap - EXIT INT TERM
    fail() { log "오류 [사전 작업] $*"; exit 1; }   # 이 서브셸 안에서는 전체를 중단하지 않고 자기만 끝낸다
    while [ ! -f "$PREP_B/.complete" ]; do kill -0 "$pfpid" 2>/dev/null || exit 1; sleep 2; done
    : > "$PREP_B/.deleting"
    log "  [다음 작품 미리 준비] 원본 삭제"
    rm_checked "$uri" "$nm" "$ROOT" || fail "원본 폴더 삭제 실패"
    if [ "$TRACKS_ONLY" != 1 ]; then
      log "  [다음 작품 미리 준비] 빈자리 메우는 중: 패드 $hint 개"
      make_pads "$hint" 1   # 현재 작품 쓰기·다음 작품 읽기와 겹치므로 프로세스 수를 줄이려고 하나씩 만든다
    fi
    printf '%s' "$PADN" > "$PREP_B/.padn"
    : > "$PREP_B/.predeleted"
    log "  [다음 작품 미리 준비] 완료"
  ) > "$PREP_B.prep.log" 2>&1 &
  PREP_PID=$!
}
# 다음 작품 차례가 되면 미리 준비가 끝나길 기다린다. 결과: PRE_DONE = 0 안 함 / 1 완료 / 2 실패
join_prep() {   # $1 = 그 작품의 백업 디렉터리
  PRE_DONE=0
  [ -n "$PREP_PID" ] || return 0
  wait "$PREP_PID" 2>/dev/null; PREP_PID=""
  [ -f "$1.prep.log" ] && { cat "$1.prep.log"; rm -f "$1.prep.log"; }
  local pn; [ -f "$1/.padn" ] && { pn=$(cat "$1/.padn"); [ "$pn" -gt "$PADN" ] && PADN=$pn; }
  if [ -f "$1/.predeleted" ]; then PRE_DONE=1; else PRE_DONE=2; fi
  return 0
}

# y 로 진행, n 으로 중단. 입력에 섞여 오는 \r·공백·전각은 무시하고, 한글 자판(ㅛ/ㅜ)도 받는다.
# 빈 입력이나 알 수 없는 입력은 중단으로 치지 않고 다시 묻는다 (실수로 멈추지 않도록).
ask_continue() {
  local ans
  while :; do
    read -r -p "$1" ans < /dev/tty || return 1
    ans=${ans//$'\r'/}; ans=${ans//[[:space:]]/}
    case $ans in
      y|Y|yes|YES|ｙ|Ｙ|ㅛ) return 0 ;;
      n|N|no|NO|ｎ|Ｎ|ㅜ)   return 1 ;;
      *) echo "  y 또는 n 을 입력하세요 (받은 입력: $(printf '%q' "$ans"))" ;;
    esac
  done
}
backup_dir() { printf '%s/%s_%s_%03d' "$BACKUP_BASE" "$(date +%Y%m%d_%H%M%S)" "$$" "$1"; }

# 작품 하나 다시 만들기 (방식 A). 백업은 이미 끝나 있다.
rebuild_work() {   # $1 plan  $2 검증된 백업 디렉터리  $3 시작 시각
  local P=$1 B=$2 t0=$3 name wuri
  name=$(jq -r .name <<<"$P"); wuri=$(jq -r .uri <<<"$P")
  CUR_WORK=$name CUR_BACKUP=$B
  STAGE=delete
  if [ "$PRE_DONE" = 1 ]; then
    log "  (원본 삭제와 빈자리 메우기는 앞 작품을 쓰는 동안 이미 끝남)"
  else
    rm_checked "$wuri" "$name" "$ROOT" || fail "원본 폴더 삭제 실패 — 아직 SD 에 남아 있음"
    log "  원본 작품 폴더 삭제"
  fi
  write_phase "$B"
  log "=== [$name] 성공 (총 $(ms "$t0") ms)"
  CUR_WORK=""
}

# ---------------------------------------------------------------- 복구
# 같은 이름 폴더가 SD 에 남아 있으면, 그 안의 모든 항목이 백업에 있는 것(같은 경로, 크기 ≤ 백업)일 때만 지운다.
restore_check_tree() {   # $1 URI  $2 상대경로(.=루트)  $3 백업 경로표 JSON 파일
  local L sub u
  L=$(saf_ls "$1") || fail "기존 폴더 ls 실패 ($2)"
  jq -e --slurpfile T "$3" --arg p "$2" --arg d "$DIR_MIME" '
    def jp($p; $n): if $p == "." then $n else $p + "/" + $n end;
    all(.[]; jp($p; .name) as $k | $T[0][$k] as $t
        | $t != null and ($t.kind == (if .type == $d then "d" else "f" end))
          and (.type == $d or (.length // 0) <= $t.length))' <<<"$L" >/dev/null \
    || fail "SD 의 '$2' 에 백업에 없는 내용이 있음 — 직접 확인 필요 (아무것도 지우지 않음)"
  while IFS=$'\t' read -r -u 3 sub u; do
    restore_check_tree "$u" "$(jp "$2" "$sub")" "$3"
  done 3< <(jq -r --arg d "$DIR_MIME" '.[] | select(.type == $d) | [.name, .uri] | @tsv' <<<"$L")
}

restore() {
  local B=${RESTORE%/} name seq kind parent fname ftype flen fsha f L cnt ex
  [ -f "$B/.complete" ] && [ -f "$B/meta.json" ] && [ -f "$B/index.tsv" ] || { log "완전한 백업이 아님: $B"; exit 1; }
  name=$(jq -r .work "$B/meta.json")
  CUR_WORK=$name CUR_BACKUP=$B
  STAGE=restore-check
  log "=== 복구: [$name] ← $B"
  while IFS=$'\t' read -r -u 3 seq kind parent fname ftype flen fsha; do
    [ "$kind" = f ] || continue
    f=$B/$(printf %05d "$seq").bin
    [ "$(stat -c %s "$f")" = "$flen" ] && [ "$(sha256sum < "$f" | cut -d' ' -f1)" = "$fsha" ] \
      || fail "백업 파일 손상: $(jp "$parent" "$fname")"
  done 3< "$B/index.tsv"
  log "  백업 sha256 확인 OK"

  L=$(saf_ls "$ROOT") || fail "루트 ls 실패"
  cnt=$(jq --arg n "$name" '[.[] | select(.name == $n)] | length' <<<"$L")
  if [ "$cnt" -gt 0 ]; then
    ex=$(jq -r --arg n "$name" '.[] | select(.name == $n) | .uri' <<<"$L")
    jq -Rs 'split("\n") | map(select(length > 0) | split("\t")
              | {key: (if .[2] == "." then .[3] else .[2] + "/" + .[3] end),
                 value: {kind: .[1], length: (.[5] | tonumber)}}) | from_entries' < "$B/index.tsv" > "$B/paths.json"
    restore_check_tree "$ex" . "$B/paths.json"
    STAGE=mkdir   # 이 시점 이후 실패 시 백업 유지
    log "  남아 있는 같은 이름 폴더(백업 내용의 일부/전체만 포함) 삭제"
    rm_checked "$ex" "$name" "$ROOT" || fail "기존 폴더 삭제 실패 — 아직 SD 에 남아 있음"
  fi
  write_phase "$B"
  log "=== 복구 성공: [$name]"
}

# ---------------------------------------------------------------- main
if [ "$MODE" = restore ]; then
  [ "$TRACKS_ONLY" = 1 ] && PADS_FILE=$CACHE_DIR/pads.$$ || prepare_pads
  restore
  [ "$KEEP_PADS" = 0 ] && cleanup_pads
  exit 0
fi
if [ "$MODE" = cleanpads ]; then
  KEEP_PADS=0; prepare_pads; log "패드 정리 완료"; exit 0
fi

if [ "$RESCAN" = 1 ] || [ ! -s "$MANIFEST" ] || [ "$(jq -r .root "$MANIFEST" 2>/dev/null)" != "$ROOT" ] \
   || ! jq -e '.works | all(has("dirs") and has("pos"))' "$MANIFEST" >/dev/null 2>&1; then
  scan
else
  log "저장된 스캔 사용: $(jq -r .scanned_at "$MANIFEST") (다시 스캔하려면 --rescan)"
fi

PLANS=$(jq --arg dir "$DIR_MIME" "$PLAN_JQ" "$MANIFEST") || { log "계획 계산 실패"; exit 1; }

# ---------------------------------------------------------------- 리포트 (읽기 전용, 로컬 계산만)
# 각 MP3 폴더에서 공통 앞부분을 뺀 이름의 "번호 모양"(번호 앞 글자 + N + 번호 뒤 한 글자)을 모아
# 라이브러리 전체에서 어떤 패턴이 몇 번 나오는지, 그리고 확인이 필요한 폴더의 전체 파일명과
# 계획된 순서를 출력한다.
if [ "$MODE" = report ]; then
  REPORT=$HOME/snowsky_report.txt
  jq -r --arg dir "$DIR_MIME" "$COMMON_JQ"'
    def pat($pre): (.name | norm) as $n
      | (if ($n | startswith($pre)) then $n[($pre | length):] else $n end)
      | (capture("^(?<a>[^0-9]*)(?<n>[0-9]+)(?<b>.?)") | (.a | if length > 6 then "…" + .[-6:] else . end) + "N" + .b) // "(숫자 없음)";
    [ .works[] as $w | $w.dirs[]
      | [.entries[] | select(.type != $dir and ismp3)] as $m | select(($m | length) > 0)
      | ([$m[] | .name | norm] | lcp) as $pre
      | { work: $w.name, path: .path, pre: $pre,
          files: [$m | sort_by(.idx)[] | {name, pat: pat($pre), tok: tracklabel($pre)}],
          planned: ( ([$m[] | select((hidden | not) and hasnum)] | sort_by(tkey($pre)) | map(.name))
                   + ([$m[] | select((hidden | not) and (hasnum | not))] | sort_by(okey) | map(.name))
                   + ([$m[] | select(hidden)] | sort_by(.name) | map(.name)) ) } ] as $F
    | ([$F[].files[]] | length) as $nf
    | "==== 요약: MP3 폴더 \($F | length)개, MP3 \($nf)개",
      "",
      "==== 번호 패턴 (공통 앞부분을 뺀 뒤 첫 번호 주변. 많은 순)",
      ( [$F[] as $f | $f.files[] | {pat, ex: ($f.work + "/" + (if $f.path == "" then "" else $f.path + "/" end) + .name)}]
        | group_by(.pat) | sort_by(-length)[]
        | "  \(length)개  \(.[0].pat)    예: \(.[0].ex)" ),
      "",
      "==== 확인이 필요한 폴더 (같은 번호 / 숫자 없음 / 한 폴더에 패턴 여러 개)",
      ( $F[]
        | ([.files[] | select(.tok != null) | .tok | tonumber] | group_by(.) | map(select(length > 1) | .[0])) as $dup
        | ([.files[] | select(.pat == "(숫자 없음)")] | length) as $nod
        | ([.files[].pat] | unique | length) as $np
        | select(($dup | length) > 0 or $nod > 0 or $np > 1)
        | "",
          "[\(.work)\(if .path == "" then "" else "/" + .path end)]",
          "  이유: " + ([ (if ($dup | length) > 0 then "같은 번호 \($dup | map(tostring) | join(","))" else empty end),
                         (if $nod > 0 then "숫자 없음 \($nod)개" else empty end),
                         (if $np > 1 then "패턴 \($np)종" else empty end) ] | join(", ")),
          "  공통 앞부분: " + (if .pre == "" then "(없음)" else .pre end),
          "  계획된 순서:",
          (.planned | to_entries[] | "    \(.key + 1). \(.value)") ),
      "",
      "==== 전체 MP3 폴더 (작품 이름순). 줄 형식: 계획순번. [표시번호] 파일명   (현재 위치가 다르면 ← 현재 N번째)",
      ( $F | sort_by([.work, .path])[]
        | (.files | map({key: .name, value: .}) | from_entries) as $by
        | (.files | to_entries | map({key: .value.name, value: (.key + 1)}) | from_entries) as $cur
        | "",
          "[\(.work)\(if .path == "" then "" else "/" + .path end)]" + (if [.files[].name] == .planned then "  (순서 정상)" else "" end),
          (.planned | to_entries[]
            | "  \(.key + 1). [\($by[.value].tok // "-")] \(.value)"
              + (if $cur[.value] != .key + 1 then "   ← 현재 \($cur[.value])번째" else "" end)) )
  ' "$MANIFEST" > "$REPORT" || { log "리포트 생성 실패"; exit 1; }
  jq -r --arg dir "$DIR_MIME" "$COMMON_JQ"'
    "", "==== 작품 폴더 순서 (Asmr 바로 아래). 줄 형식: 목표순번. 작품명   (현재 위치가 다르면 ← 현재 N번째)",
    ( [.works[] | {name, pos}] | (sort_by(.pos) | to_entries | map({key: .value.name, value: (.key + 1)}) | from_entries) as $cur
      | sort_by(wkey) | to_entries[]
      | "  \(.key + 1). \(.value.name)" + (if $cur[.value.name] != .key + 1 then "   ← 현재 \($cur[.value.name])번째" else "" end) )
  ' "$MANIFEST" >> "$REPORT" || { log "리포트 생성 실패"; exit 1; }
  log "리포트: $REPORT ($(wc -l < "$REPORT") 줄, $(wc -c < "$REPORT") 바이트)"
  exit 0
fi

# 작품 순서 계획: 정렬 순서의 앞부분 중 (내부 순서 정상 + 현재 위치도 순서대로)인 최대 구간은 유지,
# 나머지는 정렬 순서대로 다시 생성(루트 맨 뒤). 건너뜀(skip) 작품은 다시 만들 수 없으므로 고정.
ROOTPLAN=$(jq -c --argjson tracks "$TRACKS_ONLY" "$COMMON_JQ"'
  ([.[] | select(.status != "skip")] | sort_by(wkey)) as $S
  | (reduce range(0; $S | length) as $i ({m: 0, last: -1, stop: false};
       if .stop then .
       elif $S[$i].status != "reorder" and $S[$i].pos > .last then .m = $i + 1 | .last = $S[$i].pos
       else .stop = true end)) as $r
  | { current: [sort_by(.pos)[].name],
      sorted: [$S[].name],
      pinned: [.[] | select(.status == "skip") | .name],
      keep: (if $tracks == 1 then [] else [$S[:$r.m][].name] end),
      rebuild: (if $tracks == 1 then [$S[] | select(.status == "reorder") | .name] else [$S[$r.m:][].name] end) }' <<<"$PLANS")

if [ -n "$FOLDER" ]; then
  SEL=$(jq -c --arg n "$FOLDER" '[.[] | select(.name == $n)]' <<<"$PLANS")
  [ "$(jq length <<<"$SEL")" = 1 ] || { log "작품 폴더를 찾을 수 없음: '$FOLDER' (정확한 이름 필요, 필요하면 --rescan)"; exit 1; }
  TARGETS=$(jq -c --argjson f "$FORCE" --argjson R "$(jq -c .rebuild <<<"$ROOTPLAN")" \
    '[.[] | select(.status != "skip" and (.name as $n | $f == 1 or ($R | index($n)) != null))]' <<<"$SEL")
else
  SEL=$PLANS
  TARGETS=$(jq -c --argjson R "$(jq -c .rebuild <<<"$ROOTPLAN")" --argjson lim "$LIMIT" \
    '(map({key: .name, value: .}) | from_entries) as $by | [$R[] | $by[.]] | if $lim > 0 then .[:$lim] else . end' <<<"$PLANS")
fi
NT=$(jq length <<<"$TARGETS")

if [ "$MODE" = dry ]; then
  echo
  if [ -n "$FOLDER" ]; then
    jq -r "$DETAIL_JQ" <<<"$SEL"
    [ "$NT" = 1 ] && echo "→ 이 작품은 다시 생성 대상입니다." || echo "→ 이 작품은 다시 생성하지 않습니다."
  else
    echo "==== 작품 순서 (Asmr 바로 아래)"
    if [ "$TRACKS_ONLY" = 1 ]; then
      echo "  --tracks-only: 작품 순서는 맞추지 않음"
    else
      jq -r '
        "  현재 순서 (앞 5개): " + (.current[:5] | join(" → ")) + (if (.current | length) > 5 then " → …" else "" end),
        "  목표 순서 (앞 5개): " + (.sorted[:5] | join(" → ")) + (if (.sorted | length) > 5 then " → … (전체는 --report)" else "" end),
        "  그대로 둠: \(.keep | length)개" + (if (.keep | length) > 0 and (.keep | length) <= 5 then " (" + (.keep | join(", ")) + ")" else "" end),
        "  다시 생성: \(.rebuild | length)개 (이 순서로 맨 뒤에 생성)",
        (if (.pinned | length) > 0 then "  고정(건너뜀 작품 — 위치를 바꿀 수 없음): " + (.pinned | join(", ")) else empty end)' <<<"$ROOTPLAN"
    fi
    echo
    echo "==== 다시 생성할 작품"
    jq -r '.[] | "  \(.name)  [" + (if .status == "reorder" then "트랙 순서 + 위치" else "위치만 (내부는 그대로 복사)" end) + "]  파일 \(.nfiles)개, \(.bytes / 1048576 * 10 | floor / 10) MB"' <<<"$TARGETS"
    echo
    if [ "$LIMIT" -gt 0 ]; then
      jq -r "$DETAIL_JQ" <<<"$TARGETS"
    else
      echo "==== 트랙 순서가 바뀌는 작품 (상세)"
      jq -c '[.[] | select(.status == "reorder")]' <<<"$TARGETS" | jq -r "$DETAIL_JQ"
    fi
    echo "---- MP3 없는 폴더 (작품 순서에는 포함)"
    jq -r '[.[] | select(.status == "nomp3") | "  " + .name] | if length == 0 then "  없음" else .[] end' <<<"$PLANS"
  fi
  echo "==== 경고가 있는 작품"
  jq -r '[.[] | select((.warnings | length) > 0)] | if length == 0 then "  없음" else .[] | "  [\(.name)] (\(.status))", (.warnings[] | "    - " + .) end' <<<"$SEL"
  echo "==== 요약"
  jq -r '
    "  작품 \(length)개: 트랙 순서 틀림 \([.[] | select(.status == "reorder")] | length), 트랙 정상 \([.[] | select(.status == "ok")] | length), 건너뜀 \([.[] | select(.status == "skip")] | length), MP3 없음 \([.[] | select(.status == "nomp3")] | length)"' <<<"$SEL"
  jq -r '"  이번 실행 대상: \(length)개 작품, 파일 \([.[].nfiles] | add // 0)개, \(([.[].bytes] | add // 0) / 1048576 | floor) MB (휴대폰 내부에는 한 번에 작품 1개 분량만 사용)"' <<<"$TARGETS"
  log "dry-run: 아무것도 변경하지 않았습니다. 로그: $LOG"
  exit 0
fi

# --test / --apply
if [ "$NT" = 0 ]; then
  log "처리할 작품이 없습니다. (이미 정상인 작품을 다시 만들려면 --folder 와 --force)"
  exit 0
fi
if [ "$MODE" = test ]; then
  [ "$(jq '.[0].nmp3' <<<"$TARGETS")" -ge 3 ] || log "참고: MP3 가 3개 미만인 작품입니다."
  [ "$TRACKS_ONLY" = 1 ] || log "참고: 이 작품은 루트 맨 뒤로 옮겨집니다. 전체 적용 때 작품 순서는 다시 맞춰집니다."
fi
if [ "$TRACKS_ONLY" = 1 ]; then PADS_FILE=$CACHE_DIR/pads.$$; : > "$PADS_FILE"; else prepare_pads; fi
# 복구하지 않은 백업이 남아 있으면 그 작품은 SD 에 없거나 불완전할 수 있다 → 먼저 --restore. (불완전한 백업은 원본이 그대로이므로 지운다)
LEFT=0
for d in "$BACKUP_BASE"/*/; do
  [ -d "$d" ] || continue
  d=${d%/}
  if [ -f "$d/.complete" ]; then
    LEFT=$((LEFT + 1)); log "복구하지 않은 백업: [$(jq -r .work "$d/meta.json" 2>/dev/null)] → bash $0 --restore '$d'"
  else
    rm -rf -- "$d" "$d.log" "$d.prep.log" "$d.nodes.jsonl" "$d.mini.json"
  fi
done
if [ "$LEFT" -gt 0 ] && [ "$IGNORE_BACKUPS" = 0 ]; then
  log "위 작품을 먼저 복구한 뒤 다시 실행하세요. (무시하고 진행하려면 --ignore-backups)"
  exit 1
fi
log "실제 적용: $NT 개 작품 (로그: $LOG)"
for ((t = 0; t < NT; t++)); do
  P=$(jq -c ".[$t]" <<<"$TARGETS"); NAME=$(jq -r .name <<<"$P")
  echo
  log "=== [$NAME] 처리 시작 ($((t + 1))/$NT, 파일 $(jq .nfiles <<<"$P")개, 하위 폴더 $(jq .ndirs <<<"$P")개, $(hsize "$(jq .bytes <<<"$P")"))"
  T0=$(now)
  if [ -n "$PF_PID" ]; then
    PID=$PF_PID B=$PF_B; PF_PID="" PF_B=""
    log "  (앞 작품을 쓰는 동안 미리 읽어 둔 백업 사용)"
  else
    B=$(backup_dir "$t"); start_backup "$P" "$B"; PID=$BG_PID
  fi
  finish_backup "$PID" "$B" || { log "중단: [$NAME] 사전 확인/백업 실패 — 원본은 그대로입니다."; rm -f "$B.nodes.jsonl" "$B.mini.json"; exit 1; }
  join_prep "$B"
  if [ -f "$B/.deleting" ]; then CUR_WORK=$NAME CUR_BACKUP=$B STAGE=delete; fi   # 지금부터 멈추면 복구 명령을 안내한다
  if [ "$PRE_DONE" = 2 ]; then fail "미리 준비(원본 삭제/빈자리 메우기)가 실패함 — 원본이 이미 삭제됐을 수 있음"; fi
  if [ -f "$B/refreshed.jsonl" ]; then   # 스캔 이후 바뀐 작품: 갱신된 정보로 이어간다
    update_manifest_keep "$NAME" "$B/refreshed.jsonl" || log "경고: 매니페스트 갱신 실패 (다음 실행 시 --rescan 권장)"
    P=$(cat "$B/plan.json")
    log "  갱신된 계획: 파일 $(jq .nfiles <<<"$P")개, 하위 폴더 $(jq .ndirs <<<"$P")개"
  fi
  # 다음 작품은 지금 작품을 쓰는 동안 미리 읽는다 (SD 읽기와 쓰기를 겹쳐 전체 시간을 줄인다)
  NEXT_P=""
  if [ $((t + 1)) -lt "$NT" ]; then
    NEXT_P=$(jq -c ".[$((t + 1))]" <<<"$TARGETS")
    PF_B=$(backup_dir $((t + 1))); start_backup "$NEXT_P" "$PF_B"; PF_PID=$BG_PID
  fi
  rebuild_work "$P" "$B" "$T0"
  if [ "$MODE" = apply ] && [ "$YES" = 0 ] && [ $((t + 1)) -lt "$NT" ]; then
    ask_continue "다음 작품으로 진행할까요? ($((t + 2))/$NT: $(jq -r ".[$((t + 1))].name" <<<"$TARGETS")) [y/n] " \
      || { kill_prefetch; log "사용자 중단. 처리 완료 $((t + 1))/$NT (다시 실행하면 남은 작품부터 이어서 진행)"; exit 0; }
  fi
done
REMAIN=$(jq '.rebuild | length' <<<"$ROOTPLAN")
[ -z "$FOLDER" ] && REMAIN=$((REMAIN - NT))
if [ "$KEEP_PADS" = 0 ] || { [ "$KEEP_PADS" = auto ] && [ "$REMAIN" -le 0 ]; }; then cleanup_pads
else log "패드 $(wc -l < "$PADS_FILE") 개 유지 — 다음 실행이 빈자리를 다시 채우지 않아도 됨 (남은 작품 ${REMAIN}개). 모두 끝나면 자동 삭제, 지금 지우려면: bash $0 --cleanup-pads"; fi
if [ "$TRACKS_ONLY" = 0 ] && [ "$MODE" = apply ] && [ -z "$FOLDER" ]; then
  want=$(jq -c '.sorted' <<<"$ROOTPLAN")
  got=$(saf_ls "$ROOT" | jq -c --argjson W "$want" '[.[] | select(.name as $n | $W | index($n)) | .name]')
  if [ "$got" = "$want" ]; then log "작품 순서 확인 OK: 루트의 작품 폴더가 목표 순서와 일치"
  elif [ "$LIMIT" -gt 0 ]; then log "참고: --limit 실행이라 작품 순서는 아직 중간 상태입니다 (남은 작품을 처리하면 맞춰짐)"
  else log "경고: 루트의 작품 순서가 목표와 다름 — --rescan 으로 dry-run 해서 확인하세요"; fi
fi
log "모두 완료: $NT 개 작품. 로그: $LOG"
