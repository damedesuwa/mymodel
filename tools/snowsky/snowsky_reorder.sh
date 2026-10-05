#!/data/data/com.termux/files/usr/bin/bash
# snowsky_reorder.sh — FiiO SnowSky ECHO NANO 용 MP3 "생성 순서" 재정렬 (Termux + SAF)
#
# SnowSky 는 파일을 SD 카드 디렉터리 엔트리 순서(복사 순서)로 보여준다.
# 이 스크립트는 Asmr/<작품>/ 의 파일을 트랙 번호 순서대로 새 폴더에 다시 생성한다.
#
# 방식 A: 작품 하나씩
#   1) 폴더 재확인(ls) → 2) 모든 파일을 $TMPDIR 로 읽어 sha256/크기 검증(백업)
#   3) 원본 폴더 삭제 → 4) 같은 이름으로 새 폴더 생성 → 5) 트랙 순서로 create+write
#   6) ls 로 이름/크기/엔트리 순서 검증 + 전 파일 재읽기 sha256 비교 → 7) 백업 삭제
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

MODE=dry FOLDER="" LIMIT=0 FORCE=0 RESCAN=0 RESTORE=""

usage() {
  cat <<'U'
사용법: bash snowsky_reorder.sh [옵션]

  --dry-run           (기본값) 아무것도 변경하지 않고 계획만 출력
  --folder <작품명>    그 작품 하나만 대상 (dry-run 또는 --apply 와 함께)
  --limit <N>         재정렬이 필요한 작품 중 이름순 앞에서 N개만 대상
  --test <작품명>      그 작품 하나만 실제 처리 (이미 순서가 맞아도 다시 생성)
  --apply             실제 적용. 작품 하나 끝날 때마다 다음으로 갈지 y/N 확인
  --force             이미 트랙 순서대로인 작품도 대상에 포함
  --rescan            저장된 스캔 결과를 버리고 SD 카드를 다시 스캔
  --gap <초>          파일 생성 사이 대기 시간 (기본 0)
  --jobs <N>          스캔 시 병렬 ls 개수 (기본 4, 읽기 전용)
  --restore <백업>     실패로 남은 백업 폴더에서 작품을 다시 생성
  -h, --help          이 도움말

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
on_exit() { rmdir "$LOCK" 2>/dev/null; sleep 0.2; }
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
      log "원본 폴더는 이미 삭제됨. 검증된 백업(유지됨): $CUR_BACKUP"
      log "복구: bash $0 --restore '$CUR_BACKUP'" ;;
  esac
}
fail() {
  log "오류 [$STAGE] ${CUR_WORK:+[$CUR_WORK] }$*"
  explain_state
  exit 1
}

# ---------------------------------------------------------------- SAF helpers
saf_ls() {   # stdout: JSON 배열. 실패 시 return 1
  local out
  out=$(termux-saf-ls "$1" 2>&1)
  jq -e 'type == "array"' >/dev/null 2>&1 <<<"$out" || { log "ls 실패: ${out:0:200}"; return 1; }
  printf '%s' "$out"
}

ROOT=$(termux-saf-dirs 2>/dev/null | jq -r --arg n "$ROOT_NAME" \
        '[.[] | select(.name == $n)] | if length == 1 then .[0].uri else empty end')
[ -n "$ROOT" ] || { log "termux-saf-dirs 에서 '$ROOT_NAME' 를 하나로 찾을 수 없음 (termux-saf-managedir 로 권한 부여)"; exit 1; }

# ---------------------------------------------------------------- 스캔 (1회)
scan() {
  local t0 rl tmp n i u
  t0=$(now)
  log "스캔 시작: $ROOT_NAME"
  rl=$(saf_ls "$ROOT") || { log "루트 ls 실패"; exit 1; }
  tmp=$(mktemp -d "$TMPDIR/snowsky_scan.XXXXXX")
  jq --arg d "$DIR_MIME" '[.[] | select(.type == $d) | {name, uri}]' <<<"$rl" > "$tmp/dirs.json"
  n=$(jq length "$tmp/dirs.json")
  # 병렬 ls (읽기 전용). 맨 wait 는 tee 프로세스 치환까지 기다리므로 PID 로만 기다린다.
  local -a pids=()
  i=0
  while IFS= read -r u; do
    ( termux-saf-ls "$u" > "$tmp/$i.json" 2>&1 ) &
    pids[i]=$!
    [ "$i" -ge "$JOBS" ] && wait "${pids[i - JOBS]}"
    i=$((i + 1))
    [ $((i % 20)) -eq 0 ] && log "  $i / $n"
  done < <(jq -r '.[].uri' "$tmp/dirs.json")
  for u in "${pids[@]}"; do wait "$u"; done
  for ((i = 0; i < n; i++)); do   # 실패한 것은 한 번 순차 재시도
    jq -e 'type == "array"' "$tmp/$i.json" >/dev/null 2>&1 && continue
    u=$(jq -r ".[$i].uri" "$tmp/dirs.json")
    saf_ls "$u" > "$tmp/$i.json" || { rm -rf "$tmp"; log "작품 폴더 ls 실패 — 스캔 중단"; exit 1; }
  done
  for ((i = 0; i < n; i++)); do cat "$tmp/$i.json"; echo; done |
    jq -s --slurpfile D "$tmp/dirs.json" --arg root "$ROOT" --arg at "$(date '+%F %T')" '
      { root: $root, scanned_at: $at,
        works: [ to_entries[] | { name: $D[0][.key].name, uri: $D[0][.key].uri,
                                  entries: (.value | to_entries | map(.value + {idx: .key})) } ] }
    ' > "$MANIFEST.tmp" && mv "$MANIFEST.tmp" "$MANIFEST" || { rm -rf "$tmp"; log "manifest 저장 실패"; exit 1; }
  rm -rf "$tmp"
  log "스캔 완료: 작품 $n 개, $(ms "$t0") ms → $MANIFEST"
}

update_manifest() {   # $1 작품명 $2 새 URI $3 ls JSON 파일
  jq --arg n "$1" --arg u "$2" --slurpfile L "$3" '
    { name: $n, uri: $u, entries: ($L[0] | to_entries | map(.value + {idx: .key})) } as $w
    | .works |= (if any(.[]; .name == $n) then map(if .name == $n then $w else . end) else . + [$w] end)
  ' "$MANIFEST" > "$MANIFEST.tmp" && mv "$MANIFEST.tmp" "$MANIFEST"
}

# ---------------------------------------------------------------- 계획 (로컬 계산만)
PLAN_JQ='
def fw: explode | map(if . >= 65296 and . <= 65305 then . - 65248 else . end) | implode;
def ismp3: .name | ascii_downcase | endswith(".mp3");
def tok: [.name | fw | capture("^\\s*(?<n>[0-9]+)") | .n] | first;
.works | sort_by(.name) | map(
  . as $w
  | ($w.entries | sort_by(.idx)) as $e
  | [$e[] | select(.type == $dir)] as $dirs
  | [$e[] | select(.type != $dir and ismp3) | . + {tok: tok}] as $mp3
  | ([$mp3[] | select(.tok != null) | . + {num: (.tok | tonumber)}] | sort_by([.num, .name])) as $numd
  | ([$mp3[] | select(.tok == null)] | sort_by(.name)) as $unnum
  | [$e[] | select(.type != $dir and (ismp3 | not))] as $others
  | ($numd + $unnum) as $planmp3
  | ([$e[] | select(.name | test("[\\t\\n\\r\\\\]"))] | length > 0) as $bad
  | ( (if ($dirs | length) > 0 then ["하위 폴더 있음 → 건너뜀: " + ($dirs | map(.name) | join(", "))] else [] end)
    + (if $bad then ["파일명에 탭/줄바꿈/역슬래시 → 건너뜀"] else [] end)
    + [$unnum[] | "번호 없음 (이름순으로 뒤에 배치): " + .name]
    + [$numd | group_by(.num)[] | select(length > 1) | "트랙 번호 중복 \(.[0].num): " + (map(.name) | join(", "))]
    ) as $warn
  | { name: $w.name, uri: $w.uri, warnings: $warn,
      status: ( if ($dirs | length) > 0 or $bad then "skip"
                elif ($mp3 | length) == 0 then "nomp3"
                elif [$mp3[].name] == [$planmp3[].name] then "ok"
                else "reorder" end ),
      current: [$mp3[] | .tok // .name],
      mp3: [$planmp3[].name],
      others: [$others[].name],
      order: [($planmp3 + $others)[] | {name, uri, type: (.type // ""), length: (.length // 0), label: (.tok // .name)}],
      bytes: ([($planmp3 + $others)[].length // 0] | add // 0) }
)'

DETAIL_JQ='
def st: {"reorder": "재정렬 필요", "ok": "이미 트랙 순서대로임", "skip": "건너뜀", "nomp3": "MP3 없음"}[.status];
.[] |
  "[\(.name)]",
  "상태: \(st)" + (if .status == "reorder" then "  (현재 순서: " + (.current | join(" → ")) + ")" else "" end),
  (.mp3 | to_entries[] | "\(.key + 1). \(.value)"),
  "기타 파일: " + (if (.others | length) == 0 then "없음" else (.others | join(", ")) end),
  "생성 순서: " + ([.order[].label] | join(" → ")),
  "경고: " + (if (.warnings | length) == 0 then "없음" else (.warnings | join("\n      ")) end),
  ""'

# ---------------------------------------------------------------- 쓰기 단계 (백업 → SD)
# $1 = 검증된 백업 디렉터리 (meta.json, index.tsv, NNNN.bin, .complete)
write_phase() {
  local B=$1 name nw nm n k t0 tw idx fname ftype flen fsha u L got want uri sha bytes=0
  name=$(jq -r .work "$B/meta.json")
  n=$(wc -l < "$B/index.tsv")
  t0=$(now)

  STAGE=mkdir
  nw=$(termux-saf-mkdir "$ROOT" "$name" 2>&1)
  [[ $nw == content://* ]] || fail "mkdir 실패: ${nw:0:200}"
  nm=$(termux-saf-stat "$nw" 2>/dev/null | jq -r '.name // empty')
  [ "$nm" = "$name" ] || fail "새 폴더 이름이 '$nm' 로 생성됨 (원래: '$name') — 같은 이름이 남아 있는지 확인"
  log "  새 폴더 생성: $name"

  STAGE=write
  k=0
  while IFS=$'\t' read -r idx fname ftype flen fsha; do
    k=$((k + 1)); tw=$(now)
    u=$(termux-saf-create -t "${ftype:-application/octet-stream}" "$nw" "$fname" 2>&1)
    [[ $u == content://* ]] || fail "create 실패 ($fname): ${u:0:200}"
    termux-saf-write "$u" < "$B/$(printf %04d "$idx").bin" || fail "write 실패 ($fname)"
    bytes=$((bytes + flen))
    log "  [$k/$n] 생성 $fname ($(hsize "$flen"), $(ms "$tw") ms)"
    [ "$GAP" != 0 ] && sleep "$GAP"
  done < "$B/index.tsv"

  STAGE=verify
  L=$(saf_ls "$nw") || fail "검증용 ls 실패"
  printf '%s' "$L" > "$B/final_ls.json"
  got=$(jq -c '[.[] | [.name, (.length // 0)]]' <<<"$L")
  want=$(jq -Rsc 'split("\n") | map(select(length > 0) | split("\t") | [.[1], (.[3] | tonumber)])' < "$B/index.tsv")
  [ "$got" = "$want" ] || fail "엔트리 순서/이름/크기 불일치
      기대: $want
      실제: $got"
  k=0
  while IFS=$'\t' read -r idx fname ftype flen fsha; do
    k=$((k + 1))
    uri=$(jq -r --arg n "$fname" '.[] | select(.name == $n) | .uri' <<<"$L")
    [[ $uri == content://* ]] || fail "검증: URI 없음 ($fname)"
    sha=$(termux-saf-read "$uri" | sha256sum | cut -d' ' -f1) || fail "재읽기 실패 ($fname)"
    [ "$sha" = "$fsha" ] || fail "sha256 불일치 ($fname): 원본 $fsha / 새 파일 $sha"
    log "  [$k/$n] 검증 OK $fname"
  done < "$B/index.tsv"

  update_manifest "$name" "$nw" "$B/final_ls.json" || log "경고: manifest 갱신 실패 (다음 실행 시 --rescan 권장)"
  STAGE=done
  rm -rf -- "$B"; CUR_BACKUP=""
  local el=$(( ($(now) - t0) / 1000000 ))
  log "  완료: $n 개 파일, $(hsize "$bytes"), 쓰기+검증 ${el} ms — 백업 삭제함"
}

# ---------------------------------------------------------------- 작품 하나 처리 (방식 A)
process_work() {   # $1 = plan 객체(JSON)
  local P=$1 name wuri cur want got need avail B i fname furi ftype flen f sha lsha lsz t0
  name=$(jq -r .name <<<"$P"); wuri=$(jq -r .uri <<<"$P")
  CUR_WORK=$name CUR_BACKUP=""
  t0=$(now)
  echo
  log "=== [$name] 처리 시작 ($(jq '.order | length' <<<"$P") 개 파일, $(hsize "$(jq .bytes <<<"$P")"))"

  STAGE=precheck
  cur=$(saf_ls "$wuri") || fail "작품 폴더 ls 실패"
  want=$(jq -c --arg n "$name" '.works[] | select(.name == $n) | [.entries[] | [.name, .uri, (.length // 0)]] | sort' "$MANIFEST")
  got=$(jq -c '[.[] | [.name, .uri, (.length // 0)]] | sort' <<<"$cur")
  [ "$got" = "$want" ] || fail "스캔 이후 폴더 내용이 바뀜 — --rescan 후 다시 실행 (원본 변경 없음)"
  need=$(( $(jq .bytes <<<"$P") / 1024 + 200 * 1024 ))
  avail=$(df -Pk "$TMPDIR" | awk 'NR == 2 { print $4 }')
  [ "$avail" -gt "$need" ] || fail "\$TMPDIR 여유 공간 부족: 필요 ${need} KB, 여유 ${avail} KB"

  STAGE=backup
  B=$BACKUP_BASE/$(date +%Y%m%d_%H%M%S)_$$
  mkdir -p "$B" || fail "백업 폴더 생성 실패"
  CUR_BACKUP=$B
  jq -n --arg w "$name" --arg at "$(date '+%F %T')" '{work: $w, created: $at}' > "$B/meta.json"
  : > "$B/index.tsv"
  i=0
  while IFS=$'\t' read -r fname furi ftype flen; do
    i=$((i + 1)); f=$B/$(printf %04d "$i").bin
    sha=$(termux-saf-read "$furi" | tee "$f" | sha256sum | cut -d' ' -f1) || fail "읽기 실패 ($fname)"
    lsz=$(stat -c %s "$f")
    [ "$lsz" = "$flen" ] || fail "읽은 크기 불일치 ($fname): SD $flen / 로컬 $lsz"
    lsha=$(sha256sum < "$f" | cut -d' ' -f1)
    [ "$lsha" = "$sha" ] || fail "로컬 백업 sha256 불일치 ($fname)"
    printf '%s\t%s\t%s\t%s\t%s\n' "$i" "$fname" "$ftype" "$flen" "$sha" >> "$B/index.tsv"
  done < <(jq -r '.order[] | [.name, .uri, .type, .length] | @tsv' <<<"$P")
  touch "$B/.complete"
  log "  백업 완료: $i 개 파일 sha256 검증 ($(ms "$t0") ms) → $B"

  STAGE=delete
  termux-saf-rm "$wuri" || fail "원본 폴더 삭제 실패 (rc=$?)"
  log "  원본 폴더 삭제"

  write_phase "$B"
  log "=== [$name] 성공 (총 $(ms "$t0") ms)"
  CUR_WORK=""
}

# ---------------------------------------------------------------- 복구
restore() {
  local B=${RESTORE%/} name idx fname ftype flen fsha f L names
  [ -f "$B/.complete" ] && [ -f "$B/meta.json" ] && [ -f "$B/index.tsv" ] || { log "완전한 백업이 아님: $B"; exit 1; }
  name=$(jq -r .work "$B/meta.json")
  CUR_WORK=$name CUR_BACKUP=$B
  log "=== 복구: [$name] ← $B"
  while IFS=$'\t' read -r idx fname ftype flen fsha; do
    f=$B/$(printf %04d "$idx").bin
    [ "$(stat -c %s "$f")" = "$flen" ] && [ "$(sha256sum < "$f" | cut -d' ' -f1)" = "$fsha" ] \
      || { log "백업 파일 손상: $fname — 중단 (백업은 그대로)"; exit 1; }
  done < "$B/index.tsv"
  log "  백업 sha256 확인 OK"

  STAGE=mkdir   # 이 시점 이후 실패 시 백업 유지
  L=$(saf_ls "$ROOT") || fail "루트 ls 실패"
  local cnt; cnt=$(jq --arg n "$name" '[.[] | select(.name == $n)] | length' <<<"$L")
  if [ "$cnt" -gt 0 ]; then
    local ex exl
    ex=$(jq -r --arg n "$name" '.[] | select(.name == $n) | .uri' <<<"$L")
    exl=$(saf_ls "$ex") || fail "기존 폴더 ls 실패"
    names=$(jq -Rsc 'split("\n") | map(select(length > 0) | split("\t") | {key: .[1], value: (.[3] | tonumber)}) | from_entries' < "$B/index.tsv")
    jq -e --argjson B "$names" --arg d "$DIR_MIME" \
      'all(.[]; .type != $d and ($B[.name] != null) and ((.length // 0) <= $B[.name]))' <<<"$exl" >/dev/null \
      || fail "SD 에 같은 이름 폴더가 있고 백업에 없는 내용이 들어 있음 — 직접 확인 필요 (아무것도 지우지 않음)"
    log "  남아 있는 같은 이름 폴더(백업의 일부/전체만 포함) 삭제"
    termux-saf-rm "$ex" || fail "기존 폴더 삭제 실패"
  fi
  write_phase "$B"
  log "=== 복구 성공: [$name]"
}

# ---------------------------------------------------------------- main
if [ "$MODE" = restore ]; then restore; exit 0; fi

if [ "$RESCAN" = 1 ] || [ ! -s "$MANIFEST" ] || [ "$(jq -r .root "$MANIFEST" 2>/dev/null)" != "$ROOT" ]; then
  scan
else
  log "저장된 스캔 사용: $(jq -r .scanned_at "$MANIFEST") (다시 스캔하려면 --rescan)"
fi

PLANS=$(jq --arg dir "$DIR_MIME" "$PLAN_JQ" "$MANIFEST") || { log "계획 계산 실패"; exit 1; }

if [ -n "$FOLDER" ]; then
  SEL=$(jq -c --arg n "$FOLDER" '[.[] | select(.name == $n)]' <<<"$PLANS")
  [ "$(jq length <<<"$SEL")" = 1 ] || { log "작품 폴더를 찾을 수 없음: '$FOLDER' (정확한 이름 필요, 필요하면 --rescan)"; exit 1; }
else
  SEL=$PLANS
fi
TARGETS=$(jq -c --argjson f "$FORCE" --argjson lim "$LIMIT" \
  '[.[] | select(.status == "reorder" or ($f == 1 and .status == "ok"))] | if $lim > 0 then .[:$lim] else . end' <<<"$SEL")
NT=$(jq length <<<"$TARGETS")

if [ "$MODE" = dry ]; then
  echo
  if [ -n "$FOLDER" ]; then
    jq -r "$DETAIL_JQ" <<<"$SEL"
  elif [ "$LIMIT" -gt 0 ]; then
    jq -r "$DETAIL_JQ" <<<"$TARGETS"
  else
    jq -c '[.[] | select(.status == "reorder")]' <<<"$PLANS" | jq -r "$DETAIL_JQ"
    echo "---- 이미 트랙 순서대로인 작품 (건너뜀)"
    jq -r '.[] | select(.status == "ok") | "  \(.name)  (MP3 \(.mp3 | length)개)"' <<<"$PLANS"
    echo "---- MP3 없는 폴더"
    jq -r '.[] | select(.status == "nomp3") | "  \(.name)"' <<<"$PLANS"
  fi
  echo "==== 경고가 있는 작품"
  jq -r '[.[] | select((.warnings | length) > 0)] | if length == 0 then "  없음" else .[] | "  [\(.name)] (\(.status))", (.warnings[] | "    - " + .) end' <<<"$SEL"
  echo "==== 요약"
  jq -r --argjson nt "$NT" '
    "  작품 \(length)개: 재정렬 필요 \([.[] | select(.status == "reorder")] | length), 정상 \([.[] | select(.status == "ok")] | length), 건너뜀 \([.[] | select(.status == "skip")] | length), MP3 없음 \([.[] | select(.status == "nomp3")] | length)",
    "  이번 실행 대상: \($nt)개"' <<<"$SEL"
  log "dry-run: 아무것도 변경하지 않았습니다. 로그: $LOG"
  exit 0
fi

# --test / --apply
if [ "$NT" = 0 ]; then
  log "처리할 작품이 없습니다. (이미 순서가 맞는 작품은 --force 로 포함)"
  exit 0
fi
if [ "$MODE" = test ]; then
  [ "$(jq '.[0].mp3 | length' <<<"$TARGETS")" -ge 3 ] || log "참고: MP3 가 3개 미만인 작품입니다."
fi
log "실제 적용: $NT 개 작품 (로그: $LOG)"
for ((t = 0; t < NT; t++)); do
  process_work "$(jq -c ".[$t]" <<<"$TARGETS")"
  if [ "$MODE" = apply ] && [ $((t + 1)) -lt "$NT" ]; then
    read -r -p "다음 작품으로 진행할까요? ($((t + 2))/$NT: $(jq -r ".[$((t + 1))].name" <<<"$TARGETS")) [y/N] " ans < /dev/tty
    [[ $ans == [yY] ]] || { log "사용자 중단. 처리 완료 $((t + 1))/$NT"; exit 0; }
  fi
done
log "모두 완료: $NT 개 작품. 로그: $LOG"
