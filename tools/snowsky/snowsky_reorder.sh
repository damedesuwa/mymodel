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
JOBS=4
GAP=0

MODE=dry FOLDER="" LIMIT=0 FORCE=0 RESCAN=0 RESTORE="" TRACKS_ONLY=0
PAD_FMT="~P%04d" PAD_RE='^~P[0-9]{4}$' PADN=0 PADS_FILE=""

usage() {
  cat <<'U'
사용법: bash snowsky_reorder.sh [옵션]

  --dry-run           (기본값) 아무것도 변경하지 않고 계획만 출력
  --folder <작품명>    그 작품 하나만 대상 (dry-run 또는 --apply 와 함께)
  --limit <N>         다시 생성할 작품 중 앞에서 N개만 대상 (여러 번 나눠 실행해도 결과는 같음)
  --test <작품명>      그 작품 하나만 실제 처리 (이미 순서가 맞아도 다시 생성)
  --apply             실제 적용. 작품 하나 끝날 때마다 다음으로 갈지 y/N 확인
  --force             이미 트랙 순서대로인 작품도 대상에 포함
  --rescan            저장된 스캔 결과를 버리고 SD 카드를 다시 스캔
  --tracks-only       작품 순서는 맞추지 않고 작품 안의 트랙 순서만 맞춤
  --gap <초>          파일 생성 사이 대기 시간 (기본 0)
  --jobs <N>          스캔 시 병렬 ls 개수 (기본 4, 읽기 전용)
  --restore <백업>     실패로 남은 백업 폴더에서 작품을 다시 생성
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
    --test)    MODE=test; FOLDER=${2:?--test 에 작품명 필요}; shift ;;
    --folder)  FOLDER=${2:?--folder 에 작품명 필요}; shift ;;
    --limit)   LIMIT=${2:?--limit 에 숫자 필요}; shift ;;
    --force)   FORCE=1 ;;
    --rescan)  RESCAN=1 ;;
    --tracks-only) TRACKS_ONLY=1 ;;
    --gap)     GAP=${2:?}; shift ;;
    --jobs)    JOBS=${2:?}; shift ;;
    --restore) MODE=restore; RESTORE=${2:?--restore 에 백업 경로 필요}; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "알 수 없는 옵션: $1" >&2; usage >&2; exit 2 ;;
  esac
  shift
done
[[ $LIMIT =~ ^[0-9]+$ ]] || { echo "--limit 은 숫자" >&2; exit 2; }
[[ $JOBS =~ ^[1-9][0-9]*$ ]] || { echo "--jobs 는 1 이상" >&2; exit 2; }
[[ $GAP =~ ^[0-9]+([.][0-9]+)?$ ]] || { echo "--gap 은 초 단위 숫자" >&2; exit 2; }
[ "$MODE" = test ] && FORCE=1

for c in jq sha256sum termux-saf-dirs termux-saf-ls termux-saf-read termux-saf-stat \
         termux-saf-mkdir termux-saf-create termux-saf-write termux-saf-rm; do
  command -v "$c" >/dev/null || { echo "필요한 명령 없음: $c" >&2; exit 1; }
done

mkdir -p "$CACHE_DIR" "$LOG_DIR" "$BACKUP_BASE" || exit 1
LOG=$LOG_DIR/$(date +%Y%m%d_%H%M%S)_$MODE.log
exec > >(tee -a "$LOG") 2>&1

LOCK=$CACHE_DIR/lock
mkdir "$LOCK" 2>/dev/null || { echo "다른 실행이 진행 중이거나 비정상 종료됨: $LOCK (확인 후 rmdir)"; exit 1; }

STAGE=idle CUR_WORK="" CUR_BACKUP=""
on_exit() { cleanup_pads; rmdir "$LOCK" 2>/dev/null; sleep 0.2; }
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
def tok: [.name | fw | capture("^\\s*(?<n>[0-9]+)") | .n] | first;
def jp($p; $n): if $p == "" then $n else $p + "/" + $n end;
def okey: tok as $t | if $t then [0, ($t | tonumber), (.name | fw | ascii_downcase), .name]
                      else [1, 0, (.name | fw | ascii_downcase), .name] end;
'
PLAN_JQ=$COMMON_JQ'
def dirplan:
  .path as $p
  | (.entries | sort_by(.idx)) as $e
  | [$e[] | select(.type == $dir) | . + {tok: tok}] as $dirs
  | [$e[] | select(.type != $dir and ismp3) | . + {tok: tok}] as $mp3
  | ([$mp3[] | select(.tok != null) | . + {num: (.tok | tonumber)}] | sort_by([.num, .name])) as $numd
  | ([$mp3[] | select(.tok == null)] | sort_by(.name)) as $unnum
  | [$e[] | select(.type != $dir and (ismp3 | not))] as $others
  | ($dirs | sort_by(okey)) as $sdirs
  | ($numd + $unnum) as $pm
  | { path: $p, uri: .uri,
      mp3ok: ([$mp3[].name] == [$pm[].name]),
      dirok: ([$dirs[].name] == [$sdirs[].name]),
      current: [$mp3[] | .tok // .name],
      curdirs: [$dirs[].name],
      mp3: [$pm[].name], others: [$others[].name], subdirs: [$sdirs[].name],
      files: [($pm + $others)[] | {name, uri, type: (.type // ""), length: (.length // 0), label: jp($p; .tok // .name)}],
      nmp3: ($mp3 | length),
      bad: ([$e[] | select(.name | test("[\\t\\n\\r\\\\]"))] | length > 0),
      warnings: ( [$unnum[] | "번호 없음 (이름순으로 뒤에 배치): " + jp($p; .name)]
                + [$numd | group_by(.num)[] | select(length > 1)
                   | "트랙 번호 중복 \(.[0].num): " + (map(jp($p; .name)) | join(", "))] ) };
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
  local B=$1 name nw nm n nf k t0 tw seq kind parent fname ftype flen fsha u L got want uri sha key j bytes=0
  local -A DU=() DLS=()
  name=$(jq -r .work "$B/meta.json")
  n=$(wc -l < "$B/index.tsv")
  nf=$(awk -F'\t' '$2 == "f"' "$B/index.tsv" | wc -l)
  t0=$(now)

  STAGE=mkdir
  make_work_folder "$name"
  nw=$NEW_WORK_URI
  DU[.]=$nw

  STAGE=write
  k=0
  while IFS=$'\t' read -r seq kind parent fname ftype flen fsha; do
    k=$((k + 1)); tw=$(now)
    [ -n "${DU[$parent]:-}" ] || fail "부모 폴더 URI 없음: $parent"
    key=$(jp "$parent" "$fname")
    if [ "$kind" = d ]; then
      u=$(termux-saf-mkdir "${DU[$parent]}" "$fname" 2>&1)
      [[ $u == content://* ]] || fail "mkdir 실패 ($key): ${u:0:200}"
      nm=$(saf_stat_name "$u" "${DU[$parent]}")
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
  done < "$B/index.tsv"

  STAGE=verify
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
  k=0
  while IFS=$'\t' read -r seq kind parent fname ftype flen fsha; do
    [ "$kind" = f ] || continue
    k=$((k + 1)); key=$(jp "$parent" "$fname")
    uri=$(jq -r --arg n "$fname" '.[] | select(.name == $n) | .uri' "${DLS[$parent]}")
    [[ $uri == content://* ]] || fail "검증: URI 없음 ($key)"
    sha=$(saf_sha "$uri" "$fsha")
    [ "$sha" = "$fsha" ] || fail "sha256 불일치 ($key): 원본 $fsha / 새 파일 $sha"
    log "  [$k/$nf] 검증 OK $key"
  done < "$B/index.tsv"

  update_manifest "$name" "$nw" "$B/final_nodes.jsonl" || log "경고: manifest 갱신 실패 (다음 실행 시 --rescan 권장)"
  STAGE=done
  rm -rf -- "$B"; CUR_BACKUP=""
  log "  완료: 파일 $nf 개 + 폴더 $(( ${#DU[@]} - 1 )) 개, $(hsize "$bytes"), 쓰기+검증 $(ms "$t0") ms — 백업 삭제함"
}


# ---------------------------------------------------------------- 루트 배치 (작품 순서)
# 패드: 아주 짧은 이름의 빈 파일. 앞쪽 빈 엔트리를 메우는 용도이며 실행이 끝나면 모두 지운다.
# 짧은 이름은 어떤 빈자리에도 들어가므로, 패드가 맨 뒤에 생겼다면 그보다 앞에는 (더 긴 이름이
# 들어갈 수 있는) 빈자리가 없다는 뜻이다.
plug_holes() {
  local i=0 name u last
  while :; do
    PADN=$((PADN + 1)); name=$(printf "$PAD_FMT" "$PADN")
    u=$(termux-saf-create -t application/octet-stream "$ROOT" "$name" 2>&1)
    [[ $u == content://* ]] || fail "패드 생성 실패: ${u:0:200}"
    printf '%s\n' "$u" >> "$PADS_FILE"
    i=$((i + 1))
    last=$(saf_ls "$ROOT" | jq -r '.[-1].name') || fail "루트 ls 실패"
    [ "$last" = "$name" ] && break
    [ "$i" -ge 400 ] && fail "빈자리 채우기가 끝나지 않음 (패드 $i 개)"
  done
  [ "$i" -gt 1 ] && log "  루트 빈자리 채움: 패드 $((i - 1)) 개"
}

# 작품 폴더를 만든다. 작품 순서 모드에서는 루트 맨 뒤에 생겼는지 확인한다. 결과: NEW_WORK_URI
make_work_folder() {
  local name=$1 try nw nm last
  for try in 1 2 3; do
    [ "$TRACKS_ONLY" = 1 ] || plug_holes
    nw=$(termux-saf-mkdir "$ROOT" "$name" 2>&1)
    [[ $nw == content://* ]] || fail "mkdir 실패: ${nw:0:200}"
    nm=$(saf_stat_name "$nw" "$ROOT")
    [ "$nm" = "$name" ] || fail "새 폴더 이름이 '$nm' 로 생성됨 (원래: '$name') — 같은 이름이 남아 있는지 확인"
    if [ "$TRACKS_ONLY" = 1 ]; then break; fi
    last=$(saf_ls "$ROOT" | jq -r '.[-1].name') || fail "루트 ls 실패"
    [ "$last" = "$name" ] && break
    # 맨 뒤가 아님 → 방금 만든 빈 폴더만 지우고 다시 시도
    [ "$(saf_ls "$nw" | jq length)" = 0 ] || fail "방금 만든 폴더가 비어 있지 않음"
    log "  새 폴더가 루트 맨 뒤가 아님 (맨 뒤: $last) → 빈 폴더 삭제 후 재시도 $try"
    termux-saf-rm "$nw" || fail "재시도용 빈 폴더 삭제 실패"
    [ "$try" = 3 ] && fail "새 폴더를 루트 맨 뒤에 만들 수 없음"
  done
  NEW_WORK_URI=$nw
  log "  새 폴더 생성: $name"
}

# 이전 실행이 남긴 패드(이름 ~P0000 형식, 0 바이트 파일)와 이번 실행의 패드를 지운다.
cleanup_pads() {
  local n=0 u
  [ -n "$PADS_FILE" ] && [ -f "$PADS_FILE" ] || return 0
  while IFS= read -r u; do
    [ -n "$u" ] && termux-saf-rm "$u" >/dev/null 2>&1 && n=$((n + 1))
  done < "$PADS_FILE"
  rm -f "$PADS_FILE"
  [ "$n" -gt 0 ] && log "패드 $n 개 삭제"
  return 0
}
remove_leftover_pads() {
  local L
  L=$(saf_ls "$ROOT") || return 0
  jq -r --arg re "$PAD_RE" '.[] | select((.name | test($re)) and (.length // 0) == 0 and .type != "vnd.android.document/directory") | .uri' <<<"$L" >> "$PADS_FILE"
  cleanup_pads
  PADS_FILE=$CACHE_DIR/pads.$$; : > "$PADS_FILE"
}

# ---------------------------------------------------------------- 작품 하나 처리 (방식 A)
process_work() {   # $1 = plan 객체(JSON)
  local P=$1 name wuri need avail B i kind parent fname furi ftype flen f sha lsha lsz t0 got want u key nd=0
  name=$(jq -r .name <<<"$P"); wuri=$(jq -r .uri <<<"$P")
  CUR_WORK=$name CUR_BACKUP=""
  t0=$(now)
  echo
  log "=== [$name] 처리 시작 (파일 $(jq .nfiles <<<"$P")개, 하위 폴더 $(jq .ndirs <<<"$P")개, $(hsize "$(jq .bytes <<<"$P")"))"

  STAGE=precheck
  while IFS=$'\t' read -r key u; do   # 작품 안 모든 폴더를 다시 ls 해서 스캔 결과와 비교
    nd=$((nd + 1))
    got=$(saf_ls "$u" | jq -c '[.[] | [.name, .uri, (.length // 0)]] | sort') || fail "폴더 ls 실패 ($name/$key)"
    want=$(jq -c --arg n "$name" --arg k "$key" \
      '($k | if . == "." then "" else . end) as $k | .works[] | select(.name == $n) | .dirs[] | select(.path == $k) | [.entries[] | [.name, .uri, (.length // 0)]] | sort' "$MANIFEST")
    [ "$got" = "$want" ] || fail "스캔 이후 폴더 내용이 바뀜 ($name/$key) — --rescan 후 다시 실행 (원본 변경 없음)"
  done < <(jq -r '.dirs[] | [(if .path == "" then "." else .path end), .uri] | @tsv' <<<"$P")
  need=$(( $(jq .bytes <<<"$P") / 1024 + 200 * 1024 ))
  avail=$(df -Pk "$TMPDIR" | awk 'NR == 2 { print $4 }')
  [ "$avail" -gt "$need" ] || fail "\$TMPDIR 여유 공간 부족: 필요 ${need} KB, 여유 ${avail} KB"
  log "  사전 확인 OK (폴더 $nd 개 재확인)"

  STAGE=backup
  B=$BACKUP_BASE/$(date +%Y%m%d_%H%M%S)_$$
  mkdir -p "$B" || fail "백업 폴더 생성 실패"
  CUR_BACKUP=$B
  jq -n --arg w "$name" --arg at "$(date '+%F %T')" '{work: $w, created: $at}' > "$B/meta.json"
  : > "$B/index.tsv"
  i=0
  while IFS=$'\t' read -r kind parent fname furi ftype flen; do
    i=$((i + 1))
    if [ "$kind" = d ]; then
      printf '%s\td\t%s\t%s\t-\t0\t-\n' "$i" "$parent" "$fname" >> "$B/index.tsv"
      continue
    fi
    key=$(jp "$parent" "$fname"); f=$B/$(printf %05d "$i").bin
    sha=$(termux-saf-read "$furi" | tee "$f" | sha256sum | cut -d' ' -f1) || fail "읽기 실패 ($key)"
    lsz=$(stat -c %s "$f")
    [ "$lsz" = "$flen" ] || fail "읽은 크기 불일치 ($key): SD $flen / 로컬 $lsz"
    lsha=$(sha256sum < "$f" | cut -d' ' -f1)
    [ "$lsha" = "$sha" ] || fail "로컬 백업 sha256 불일치 ($key)"
    printf '%s\tf\t%s\t%s\t%s\t%s\t%s\n' "$i" "$parent" "$fname" "$ftype" "$flen" "$sha" >> "$B/index.tsv"
  done < <(jq -r '.ops[] | [.op, (if .parent == "" then "." else .parent end), .name, (.uri // "-"),
                            (if (.type // "") == "" then "-" else .type end), (.length // 0)] | @tsv' <<<"$P")
  touch "$B/.complete"
  log "  백업 완료: 항목 $i 개, 파일 sha256 검증 ($(ms "$t0") ms) → $B"

  STAGE=delete
  termux-saf-rm "$wuri" || fail "원본 폴더 삭제 실패 (rc=$?)"
  log "  원본 작품 폴더 삭제"

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
  while IFS=$'\t' read -r sub u; do
    restore_check_tree "$u" "$(jp "$2" "$sub")" "$3"
  done < <(jq -r --arg d "$DIR_MIME" '.[] | select(.type == $d) | [.name, .uri] | @tsv' <<<"$L")
}

restore() {
  local B=${RESTORE%/} name seq kind parent fname ftype flen fsha f L cnt ex
  [ -f "$B/.complete" ] && [ -f "$B/meta.json" ] && [ -f "$B/index.tsv" ] || { log "완전한 백업이 아님: $B"; exit 1; }
  name=$(jq -r .work "$B/meta.json")
  CUR_WORK=$name CUR_BACKUP=$B
  STAGE=restore-check
  log "=== 복구: [$name] ← $B"
  while IFS=$'\t' read -r seq kind parent fname ftype flen fsha; do
    [ "$kind" = f ] || continue
    f=$B/$(printf %05d "$seq").bin
    [ "$(stat -c %s "$f")" = "$flen" ] && [ "$(sha256sum < "$f" | cut -d' ' -f1)" = "$fsha" ] \
      || fail "백업 파일 손상: $(jp "$parent" "$fname")"
  done < "$B/index.tsv"
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
    termux-saf-rm "$ex" || fail "기존 폴더 삭제 실패"
  fi
  write_phase "$B"
  log "=== 복구 성공: [$name]"
}

# ---------------------------------------------------------------- main
if [ "$MODE" = restore ]; then
  PADS_FILE=$CACHE_DIR/pads.$$; : > "$PADS_FILE"
  [ "$TRACKS_ONLY" = 1 ] || remove_leftover_pads
  restore; cleanup_pads; exit 0
fi

if [ "$RESCAN" = 1 ] || [ ! -s "$MANIFEST" ] || [ "$(jq -r .root "$MANIFEST" 2>/dev/null)" != "$ROOT" ] \
   || ! jq -e '.works | all(has("dirs") and has("pos"))' "$MANIFEST" >/dev/null 2>&1; then
  scan
else
  log "저장된 스캔 사용: $(jq -r .scanned_at "$MANIFEST") (다시 스캔하려면 --rescan)"
fi

PLANS=$(jq --arg dir "$DIR_MIME" "$PLAN_JQ" "$MANIFEST") || { log "계획 계산 실패"; exit 1; }

# 작품 순서 계획: 정렬 순서의 앞부분 중 (내부 순서 정상 + 현재 위치도 순서대로)인 최대 구간은 유지,
# 나머지는 정렬 순서대로 다시 생성(루트 맨 뒤). 건너뜀(skip) 작품은 다시 만들 수 없으므로 고정.
ROOTPLAN=$(jq -c --argjson tracks "$TRACKS_ONLY" "$COMMON_JQ"'
  ([.[] | select(.status != "skip")] | sort_by(okey)) as $S
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
        "  현재 순서: " + (.current | join(" → ")),
        "  목표 순서: " + (.sorted | join(" → ")),
        "  그대로 둠: \(.keep | length)개" + (if (.keep | length) > 0 then " (" + (.keep | join(", ")) + ")" else "" end),
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
PADS_FILE=$CACHE_DIR/pads.$$; : > "$PADS_FILE"
[ "$TRACKS_ONLY" = 1 ] || remove_leftover_pads
log "실제 적용: $NT 개 작품 (로그: $LOG)"
for ((t = 0; t < NT; t++)); do
  process_work "$(jq -c ".[$t]" <<<"$TARGETS")"
  if [ "$MODE" = apply ] && [ $((t + 1)) -lt "$NT" ]; then
    read -r -p "다음 작품으로 진행할까요? ($((t + 2))/$NT: $(jq -r ".[$((t + 1))].name" <<<"$TARGETS")) [y/N] " ans < /dev/tty
    [[ $ans == [yY] ]] || { log "사용자 중단. 처리 완료 $((t + 1))/$NT (다시 실행하면 남은 작품부터 이어서 진행)"; exit 0; }
  fi
done
cleanup_pads
if [ "$TRACKS_ONLY" = 0 ] && [ "$MODE" = apply ] && [ -z "$FOLDER" ]; then
  want=$(jq -c '.sorted' <<<"$ROOTPLAN")
  got=$(saf_ls "$ROOT" | jq -c --argjson W "$want" '[.[] | select(.name as $n | $W | index($n)) | .name]')
  if [ "$got" = "$want" ]; then log "작품 순서 확인 OK: 루트의 작품 폴더가 목표 순서와 일치"
  elif [ "$LIMIT" -gt 0 ]; then log "참고: --limit 실행이라 작품 순서는 아직 중간 상태입니다 (남은 작품을 처리하면 맞춰짐)"
  else log "경고: 루트의 작품 순서가 목표와 다름 — --rescan 으로 dry-run 해서 확인하세요"; fi
fi
log "모두 완료: $NT 개 작품. 로그: $LOG"
