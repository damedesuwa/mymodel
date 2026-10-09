#!/usr/bin/env bash
# asmr_cover_embed.sh — Termux + SAF(외장 SD) MP3 썸네일 삽입기
#
#   * 작품 폴더(기본: SAF 루트 바로 아래 폴더)를 작업 단위로 삼고,
#     그 작품 폴더와 모든 하위 폴더에서 MP3/JPG를 모아 작품 안에서만 매칭한다.
#   * 기본 실행은 DRY RUN. --apply 를 줘야만 SD 카드의 파일이 바뀐다.
#   * 경로/URI는 셸에서 한 번도 줄 단위로 파싱하지 않는다. termux-saf-* 호출은
#     모두 Python subprocess 인자 리스트로 넘기므로 공백·일본어·특수문자가 안전하다.
#
# 사용법:  bash asmr_cover_embed.sh --help
set -euo pipefail

STATE_DIR="${ASMR_COVER_HOME:-$HOME/.asmr_cover}"
mkdir -p "$STATE_DIR"

PY=""
for c in python3 python; do
    if command -v "$c" >/dev/null 2>&1; then PY="$c"; break; fi
done

missing=()
[ -n "$PY" ] || missing+=("python")
for c in ffmpeg ffprobe termux-saf-ls termux-saf-read termux-saf-write \
         termux-saf-stat termux-saf-rm termux-saf-create; do
    command -v "$c" >/dev/null 2>&1 || missing+=("$c")
done
if [ "${#missing[@]}" -gt 0 ]; then
    echo "필요한 명령어가 없습니다: ${missing[*]}" >&2
    echo "  pkg install python ffmpeg termux-api" >&2
    echo "  (Termux:API 앱도 Termux와 같은 출처(F-Droid/GitHub)로 설치되어 있어야 합니다)" >&2
    exit 1
fi
if ! "$PY" -c 'import mutagen' >/dev/null 2>&1; then
    echo "Python 모듈 mutagen 이 없습니다:  pip install mutagen" >&2
    exit 1
fi

PY_FILE="$STATE_DIR/asmr_cover_embed.py"
cat > "$PY_FILE" <<'PYEOF'
import argparse
import datetime
import hashlib
import json
import os
import re
import shutil
import subprocess
import sys
import threading
import time
import unicodedata
import urllib.parse
from collections import OrderedDict, defaultdict, deque

DEFAULT_ROOT_URI = "content://com.android.externalstorage.documents/tree/0000-0000%3AAsmr"
TARGET = 240
DIR_TYPES = {"dir", "directory", "vnd.android.document/directory"}
MP3_EXTS = {".mp3"}
JPG_EXTS = {".jpg", ".jpeg"}
COVER_NAMES = ["cover", "front", "folder", "jacket", "album", "albumart",
               "artwork", "ジャケット", "ジャケ", "表紙", "カバー",
               "커버", "자켓", "재킷", "표지"]
SELFTEST_NAME = ".asmr_cover_selftest.tmp"


class SafError(Exception):
    pass


class Fatal(Exception):
    pass


# --------------------------------------------------------------------------
# 출력
# --------------------------------------------------------------------------
class Out:
    def __init__(self, path):
        self.f = open(path, "w", encoding="utf-8")

    def __call__(self, line=""):
        print(line, flush=True)
        self.f.write(line + "\n")
        self.f.flush()


def progress(msg):
    sys.stderr.write("\r\033[K" + msg)
    sys.stderr.flush()


def progress_end():
    sys.stderr.write("\r\033[K")
    sys.stderr.flush()


# --------------------------------------------------------------------------
# termux-saf-* 래퍼 (항상 인자 리스트로 호출 -> 셸 파싱 없음)
# --------------------------------------------------------------------------
def _run(cmd, timeout, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE):
    return subprocess.run(cmd, stdin=stdin, stdout=stdout,
                          stderr=subprocess.PIPE, timeout=timeout)


def _err_text(r):
    e = (r.stderr or b"").decode("utf-8", "replace").strip()
    o = (r.stdout or b"").decode("utf-8", "replace").strip() if isinstance(r.stdout, bytes) else ""
    return ("rc=%d %s %s" % (r.returncode, e[:300], o[:300])).strip()


def saf_json(args, retries=3):
    last = None
    for i in range(retries):
        try:
            r = _run(args, timeout=300)
            text = r.stdout.decode("utf-8", "replace").strip()
            if r.returncode == 0 and text:
                data = json.loads(text)
                if isinstance(data, dict) and "error" in data and "uri" not in data:
                    last = str(data["error"])
                else:
                    return data
            else:
                last = _err_text(r)
        except subprocess.TimeoutExpired:
            last = "timeout"
        except json.JSONDecodeError as e:
            last = "JSON 해석 실패: %s" % e
        time.sleep(1 + i)
    raise SafError("%s 실패: %s" % (args[0], last))


def saf_ls(uri):
    data = saf_json(["termux-saf-ls", uri])
    if not isinstance(data, list):
        raise SafError("termux-saf-ls 출력이 목록이 아님")
    return data


def saf_stat(uri):
    data = saf_json(["termux-saf-stat", uri])
    if isinstance(data, list) and len(data) == 1:
        data = data[0]
    if not isinstance(data, dict):
        raise SafError("termux-saf-stat 출력이 객체가 아님")
    return data


def entry_length(e):
    for k in ("length", "size"):
        v = e.get(k)
        if isinstance(v, (int, float)) and v >= 0:
            return int(v)
    return None


def entry_mtime(e):
    for k in ("last_modified", "lastModified", "mtime"):
        if k in e:
            return e[k]
    return None


def is_dir_entry(e):
    t = str(e.get("type") or "").lower()
    return t in DIR_TYPES or t.endswith("/directory")


def saf_read_to_file(uri, path, expect_len=None):
    with open(path, "wb") as f:
        r = _run(["termux-saf-read", uri], timeout=3600, stdout=f)
    if r.returncode != 0:
        raise SafError("termux-saf-read 실패: %s" % _err_text(r))
    size = os.path.getsize(path)
    if expect_len is not None and size != expect_len:
        raise SafError("읽은 크기 %d != SAF 크기 %d" % (size, expect_len))
    if size == 0:
        raise SafError("읽은 데이터가 0바이트")
    return size


def saf_write_from_file(uri, path):
    with open(path, "rb") as f:
        r = _run(["termux-saf-write", uri], timeout=3600, stdin=f)
    if r.returncode != 0:
        raise SafError("termux-saf-write 실패: %s" % _err_text(r))


def saf_create(parent_uri, name):
    r = _run(["termux-saf-create", "-t", "application/octet-stream", parent_uri, name], timeout=120)
    text = r.stdout.decode("utf-8", "replace").strip()
    if r.returncode != 0 or not text:
        raise SafError("termux-saf-create 실패: %s" % _err_text(r))
    if text.startswith("{"):
        text = json.loads(text).get("uri", "")
    if not text.startswith("content://"):
        raise SafError("termux-saf-create 출력 이상: %r" % text[:200])
    return text


def saf_rm(uri):
    r = _run(["termux-saf-rm", uri], timeout=120)
    if r.returncode != 0:
        raise SafError("termux-saf-rm 실패: %s" % _err_text(r))


def read_exact(stream, n):
    buf = bytearray()
    while len(buf) < n:
        chunk = stream.read(n - len(buf))
        if not chunk:
            break
        buf += chunk
    return bytes(buf)


# --------------------------------------------------------------------------
# ID3 헤더만 읽어서 기존 썸네일(APIC/PIC) 유무 확인 (파일 전체를 받지 않음)
# --------------------------------------------------------------------------
def syncsafe(b):
    return ((b[0] & 0x7f) << 21) | ((b[1] & 0x7f) << 14) | ((b[2] & 0x7f) << 7) | (b[3] & 0x7f)


def id3_has_picture(hdr, body):
    """True / False / None(구조 해석 불가)"""
    major, flags = hdr[3], hdr[5]
    data = body
    if major in (2, 3) and flags & 0x80:          # 태그 전체 unsynchronisation
        data = data.replace(b"\xff\x00", b"\xff")
    pos = 0
    if flags & 0x40 and major == 3:
        pos = 4 + int.from_bytes(data[0:4], "big")
    elif flags & 0x40 and major == 4:
        pos = syncsafe(data[0:4])
    idlen, hlen = (3, 6) if major == 2 else (4, 10)
    while pos + hlen <= len(data):
        fid = data[pos:pos + idlen]
        if fid[0] == 0:
            return False                           # padding
        if not re.fullmatch(rb"[A-Z0-9]+", fid):
            return None
        if major == 2:
            size = int.from_bytes(data[pos + 3:pos + 6], "big")
        elif major == 3:
            size = int.from_bytes(data[pos + 4:pos + 8], "big")
        else:
            size = syncsafe(data[pos + 4:pos + 8])
        if fid in (b"APIC", b"PIC"):
            return True
        pos += hlen + size
    return False


def probe_art_head(uri):
    """'yes' | 'no' | 'unknown' ; 실패 시 SafError"""
    p = subprocess.Popen(["termux-saf-read", uri], stdin=subprocess.DEVNULL,
                         stdout=subprocess.PIPE, stderr=subprocess.DEVNULL,
                         start_new_session=True)
    killer = threading.Timer(180, lambda: _kill(p))
    killer.start()
    try:
        hdr = read_exact(p.stdout, 10)
        if len(hdr) < 10:
            raise SafError("파일을 읽을 수 없음(헤더 %d바이트)" % len(hdr))
        if hdr[:3] != b"ID3" or hdr[3] not in (2, 3, 4):
            return "no"
        size = syncsafe(hdr[6:10])
        if size > 64 * 1024 * 1024:
            return "unknown"
        body = read_exact(p.stdout, size)
        if len(body) < size:
            raise SafError("ID3 태그를 끝까지 읽지 못함")
        res = id3_has_picture(hdr, body)
        if res is None:
            return "unknown" if (b"APIC" in body or b"PIC" in body) else "no"
        return "yes" if res else "no"
    finally:
        killer.cancel()
        _kill(p)


def _kill(p):
    try:
        if p.poll() is None:
            try:
                os.killpg(p.pid, 15)
            except Exception:
                p.terminate()
        p.wait(timeout=10)
    except Exception:
        pass
    try:
        p.stdout.close()
    except Exception:
        pass


# --------------------------------------------------------------------------
# 이름 규칙
# --------------------------------------------------------------------------
def nfc(s):
    return unicodedata.normalize("NFC", s)


def norm(s):
    return nfc(s).casefold()


def split_ext(name):
    i = name.rfind(".")
    if i <= 0:
        return name, ""
    return name[:i], name[i:].lower()


def mp3_key(name):
    return norm(split_ext(name)[0])


def jpg_key(name):
    stem = norm(split_ext(name)[0])
    if stem.endswith(".mp3"):                       # "01.mp3.jpg" 형태
        stem = stem[:-4]
    return stem


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def short_hash(s):
    return hashlib.sha1(s.encode("utf-8")).hexdigest()[:20]


# --------------------------------------------------------------------------
# 1) SD 카드 전체 1회 순회
# --------------------------------------------------------------------------
def scan(root_uri, cache_path, reuse):
    if reuse and os.path.exists(cache_path):
        with open(cache_path, encoding="utf-8") as f:
            data = json.load(f)
        if data.get("root_uri") == root_uri:
            return data, True
    dirs = []
    queue = deque([([], root_uri)])
    while queue:
        rel, uri = queue.popleft()
        progress("스캔 중: 디렉터리 %d개, 대기 %d개  %s" % (len(dirs), len(queue), "/".join(rel)[-50:]))
        d = {"rel": rel, "uri": uri, "files": [], "error": None}
        try:
            entries = saf_ls(uri)
        except SafError as e:
            d["error"] = str(e)
            dirs.append(d)
            continue
        for e in sorted(entries, key=lambda x: nfc(str(x.get("name") or ""))):
            name = e.get("name")
            euri = e.get("uri")
            if not name or not euri:
                continue
            if is_dir_entry(e):
                queue.append((rel + [name], euri))
            else:
                d["files"].append({"name": name, "uri": euri,
                                   "length": entry_length(e), "mtime": entry_mtime(e)})
        dirs.append(d)
    progress_end()
    data = {"root_uri": root_uri, "scanned_at": datetime.datetime.now().isoformat(), "dirs": dirs}
    with open(cache_path, "w", encoding="utf-8") as f:
        json.dump(data, f, ensure_ascii=False)
    return data, False


# --------------------------------------------------------------------------
# 2,3) 디렉터리별 파일 -> 작품 단위 그룹
# --------------------------------------------------------------------------
def group(scan_data, depth, root_label):
    works = OrderedDict()
    stats = defaultdict(int)
    outside = []
    for d in scan_data["dirs"]:
        stats["dirs"] += 1
        if d.get("error"):
            stats["dir_fail"] += 1
        rel = d["rel"]
        if len(rel) >= depth:
            wkey = tuple(rel[:depth])
            works.setdefault(wkey, {"name": "/".join(wkey), "mp3": [], "jpg": [], "failed_dirs": []})
            if d.get("error"):
                works[wkey]["failed_dirs"].append("/".join(rel))
        for f in d["files"]:
            name = f["name"]
            _, ext = split_ext(name)
            if ext in MP3_EXTS:
                kind = "mp3"
            elif ext in JPG_EXTS:
                kind = "jpg"
            else:
                continue
            if name.startswith("."):                 # "._01.mp3" 같은 macOS 찌꺼기 등
                stats["ignored_hidden"] += 1
                continue
            stats[kind] += 1
            item = dict(f)
            item["kind"] = kind
            item["dir"] = tuple(rel)
            item["dir_uri"] = d["uri"]
            item["full"] = "/".join([root_label] + rel + [name])
            item["rel"] = "/".join(rel + [name])
            if len(rel) < depth:
                outside.append(item)
                continue
            item["in_work"] = "/".join(rel[depth:] + [name])
            item["work"] = "/".join(rel[:depth])
            works[tuple(rel[:depth])][kind].append(item)
    return works, outside, stats


# --------------------------------------------------------------------------
# 매핑 파일 (--map)
# --------------------------------------------------------------------------
def load_map(path):
    exact, workwide, problems = {}, {}, []
    with open(path, encoding="utf-8-sig") as f:
        for no, line in enumerate(f, 1):
            line = line.rstrip("\r\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            parts = line.split("\t")
            if len(parts) != 2 or not parts[0].strip() or not parts[1].strip():
                problems.append("%s:%d: 'MP3경로<TAB>JPG경로' 형식이 아님" % (path, no))
                continue
            left, right = nfc(parts[0].strip()).strip("/"), nfc(parts[1].strip()).strip("/")
            if left.endswith("/*"):
                workwide[left[:-2]] = right
            else:
                exact[left] = right
    return exact, workwide, problems


# --------------------------------------------------------------------------
# 4) 작품별 매칭
# --------------------------------------------------------------------------
def match_work(work, opts, mapping):
    exact_map, workwide_map = mapping
    mp3s, jpgs = work["mp3"], work["jpg"]
    jpg_by_key = defaultdict(list)
    for j in jpgs:
        jpg_by_key[jpg_key(j["name"])].append(j)
    mp3_by_key = defaultdict(list)
    for m in mp3s:
        mp3_by_key[mp3_key(m["name"])].append(m)
    jpg_by_path = {nfc(j["rel"]): j for j in jpgs}
    jpg_by_path.update({nfc(j["full"]): j for j in jpgs})

    def resolve_path(p):
        return jpg_by_path.get(nfc(p))

    # 트랙 전용 이미지(이름이 어떤 MP3와 일치)는 대표 커버 후보에서 제외
    track_keys = set(mp3_by_key)
    cover_pool = [j for j in jpgs if jpg_key(j["name"]) not in track_keys]
    work_cover, work_cover_rule, work_cover_cands = None, None, []
    ww = workwide_map.get(nfc(work["name"]))
    if ww is not None:
        work_cover = resolve_path(ww)
        work_cover_rule = "매핑 파일: 작품 대표 커버"
        if work_cover is None:
            work_cover_rule = "MAP_ERROR"
            work_cover_cands = [ww]
    elif opts.work_cover:
        names = {norm(n) for n in COVER_NAMES + (opts.cover_name or [])}
        if len(jpgs) == 1:
            # 작품 안 JPG가 1장뿐이면 이름이 트랙과 같아도 그것이 작품 커버
            work_cover, work_cover_rule = jpgs[0], "작품 대표 커버(작품 안 JPG가 1개뿐)"
        elif len(cover_pool) == 1:
            work_cover, work_cover_rule = cover_pool[0], "작품 대표 커버(트랙 전용이 아닌 JPG가 1개뿐)"
        else:
            named = [j for j in cover_pool if norm(split_ext(j["name"])[0]) in names]
            if len(named) == 1:
                work_cover, work_cover_rule = named[0], "작품 대표 커버(커버 이름: %s)" % named[0]["name"]
            else:
                work_cover_cands = named if named else cover_pool

    results = []
    for m in mp3s:
        r = {"mp3": m, "jpg": None, "rule": "", "cands": [], "note": "", "status": None}
        key = mp3_key(m["name"])
        forced = exact_map.get(nfc(m["rel"]), exact_map.get(nfc(m["full"])))
        if forced is not None:
            if forced.upper() == "SKIP":
                r["status"], r["rule"] = "EXCLUDED", "매핑 파일: SKIP"
            else:
                j = resolve_path(forced)
                if j is None:
                    r["status"] = "MAP_ERROR"
                    r["note"] = "매핑된 JPG가 이 작품 안에 없음: %s" % forced
                else:
                    r["status"], r["jpg"], r["rule"] = "EMBED", j, "매핑 파일 지정"
            results.append(r)
            continue

        cands = jpg_by_key.get(key, [])
        same_name_mp3 = mp3_by_key[key]
        if cands:
            if len(cands) == 1 and len(same_name_mp3) == 1:
                r["status"], r["jpg"], r["rule"] = "EMBED", cands[0], "파일명 일치(작품 안 1:1)"
            else:
                same_dir = [c for c in cands if c["dir"] == m["dir"]]
                if opts.prefer_same_dir and len(same_dir) == 1:
                    r["status"], r["jpg"], r["rule"] = "EMBED", same_dir[0], "파일명 일치 + 같은 폴더"
                else:
                    r["status"], r["cands"] = "AMBIGUOUS", cands
                    if len(cands) > 1:
                        r["note"] = "같은 이름의 JPG가 %d개" % len(cands)
                    else:
                        r["note"] = "같은 이름의 MP3가 %d개 폴더에 있어 어느 트랙의 이미지인지 확정 불가" % len(same_name_mp3)
                    if len(same_name_mp3) > 1:
                        r["others"] = [x for x in same_name_mp3 if x is not m]
        elif work_cover is not None:
            r["status"], r["jpg"], r["rule"] = "EMBED", work_cover, work_cover_rule
        elif work_cover_rule == "MAP_ERROR":
            r["status"] = "MAP_ERROR"
            r["note"] = "작품 대표 커버로 지정된 JPG가 이 작품 안에 없음: %s" % work_cover_cands[0]
        elif opts.work_cover and work_cover_cands:
            r["status"], r["cands"] = "AMBIGUOUS", work_cover_cands
            r["note"] = "이름이 일치하는 JPG 없음, 대표 커버 후보가 %d개라 확정 불가" % len(work_cover_cands)
        else:
            r["status"] = "NO_IMAGE"
            if jpgs and not opts.work_cover:
                r["note"] = "이름이 일치하는 JPG 없음 (작품 안 JPG %d개는 자동 적용하지 않음 — --work-cover 참고)" % len(jpgs)
        results.append(r)
    return results


# --------------------------------------------------------------------------
# 이미지
# --------------------------------------------------------------------------
def ffprobe_json(path, *args):
    r = _run(["ffprobe", "-v", "error", *args, "-of", "json", path], timeout=120)
    if r.returncode != 0:
        raise SafError("ffprobe 실패: %s" % _err_text(r))
    return json.loads(r.stdout.decode("utf-8", "replace") or "{}")


def image_dims(path):
    d = ffprobe_json(path, "-select_streams", "v:0", "-show_entries", "stream=width,height,codec_name")
    s = (d.get("streams") or [{}])[0]
    if not s.get("width") or not s.get("height"):
        raise SafError("이미지 크기를 읽을 수 없음")
    return int(s["width"]), int(s["height"]), s.get("codec_name")


class ImageCache:
    def __init__(self, base):
        self.src_dir = os.path.join(base, "src")
        self.cov_dir = os.path.join(base, "cover240")
        os.makedirs(self.src_dir, exist_ok=True)
        os.makedirs(self.cov_dir, exist_ok=True)
        self.dims = {}

    def _id(self, j):
        return short_hash("%s|%s|%s" % (j["uri"], j.get("length"), j.get("mtime")))

    def source(self, j):
        p = os.path.join(self.src_dir, self._id(j) + ".jpg")
        if not os.path.exists(p):
            tmp = p + ".part"
            saf_read_to_file(j["uri"], tmp, j.get("length"))
            os.replace(tmp, p)
        return p

    def size(self, j):
        k = self._id(j)
        if k not in self.dims:
            w, h, _ = image_dims(self.source(j))
            self.dims[k] = (w, h)
        return self.dims[k]

    def cover(self, j):
        """원본 JPG는 SD에서 읽기만 한다. 240x240 결과는 Termux 쪽 캐시에만 만든다."""
        p = os.path.join(self.cov_dir, self._id(j) + "_240.jpg")
        if os.path.exists(p):
            return p
        src = self.source(j)
        tmp = p + ".part.jpg"
        vf = ("scale=%d:%d:force_original_aspect_ratio=increase:flags=lanczos,"
              "crop=%d:%d,setsar=1" % (TARGET, TARGET, TARGET, TARGET))
        r = _run(["ffmpeg", "-nostdin", "-hide_banner", "-loglevel", "error", "-y",
                  "-i", src, "-vf", vf, "-frames:v", "1", "-c:v", "mjpeg",
                  "-pix_fmt", "yuvj420p", "-q:v", "2", "-update", "1", "-f", "image2", tmp],
                 timeout=120)
        if r.returncode != 0 or not os.path.exists(tmp):
            raise SafError("ffmpeg 이미지 변환 실패: %s" % _err_text(r))
        w, h, codec = image_dims(tmp)
        if (w, h) != (TARGET, TARGET) or codec != "mjpeg":
            raise SafError("변환 결과가 %dx%d %s" % (w, h, codec))
        os.replace(tmp, p)
        return p


# --------------------------------------------------------------------------
# MP3 처리 (mutagen: ID3 태그만 다시 쓰고 오디오 바이트는 그대로)
# --------------------------------------------------------------------------
def id3v2_span(path):
    with open(path, "rb") as f:
        h = f.read(10)
    if len(h) == 10 and h[:3] == b"ID3" and h[3] in (2, 3, 4):
        return 10 + syncsafe(h[6:10]) + (10 if h[5] & 0x10 else 0)
    return 0


def tail_has_id3v1(path):
    size = os.path.getsize(path)
    if size < 128:
        return None
    with open(path, "rb") as f:
        f.seek(size - 128)
        t = f.read(128)
    return t if t[:3] == b"TAG" else None


def files_equal_from(a, a_off, b, b_off):
    sa, sb = os.path.getsize(a) - a_off, os.path.getsize(b) - b_off
    if sa != sb:
        return False
    with open(a, "rb") as fa, open(b, "rb") as fb:
        fa.seek(a_off)
        fb.seek(b_off)
        while True:
            x, y = fa.read(1 << 20), fb.read(1 << 20)
            if x != y:
                return False
            if not x:
                return True


def frame_snapshot(tags):
    if tags is None:
        return []
    return sorted((k, repr(v)) for k, v in tags.items() if not k.startswith("APIC"))


def probe_streams(path):
    d = ffprobe_json(path, "-show_entries",
                     "stream=codec_type,codec_name,width,height:stream_disposition=attached_pic:format=duration")
    return d.get("streams") or [], float((d.get("format") or {}).get("duration") or 0)


def has_attached_pic(streams):
    return any(s.get("codec_type") == "video" or (s.get("disposition") or {}).get("attached_pic") for s in streams)


def embed(orig, new, cover_path):
    """반환: ('existing'|'ok', 설명). 실패는 SafError."""
    from mutagen.mp3 import MP3
    from mutagen.id3 import APIC, PictureType

    streams, dur_orig = probe_streams(orig)
    if not any(s.get("codec_type") == "audio" and s.get("codec_name") == "mp3" for s in streams):
        raise SafError("MP3 오디오 스트림이 아님: %s" % [s.get("codec_name") for s in streams])
    if has_attached_pic(streams):
        return "existing", "ffprobe: 기존 이미지 스트림 있음"

    shutil.copyfile(orig, new)
    try:
        audio = MP3(new)
    except Exception as e:
        raise SafError("mutagen 해석 실패: %s" % e)
    if audio.tags is None:
        audio.add_tags()
        ver = 3
        before = []
    else:
        if audio.tags.getall("APIC"):
            return "existing", "ID3 APIC 프레임 있음"
        ver = 4 if audio.tags.version[1] == 4 else 3
        before = frame_snapshot(audio.tags)
    with open(cover_path, "rb") as f:
        cover = f.read()
    audio.tags.add(APIC(encoding=0, mime="image/jpeg", type=PictureType.COVER_FRONT,
                        desc="", data=cover))
    # padding: 기존 여유 공간이 충분하면 그 크기를 그대로 유지 -> 파일이 원본보다 작아지지 않음
    audio.save(v1=1, v2_version=ver, padding=lambda info: info.padding if info.padding >= 0 else 1024)

    # ID3v1 은 원본 128바이트를 그대로 되돌려 놓는다
    v1_orig = tail_has_id3v1(orig)
    if v1_orig is not None:
        if tail_has_id3v1(new) is None:
            raise SafError("ID3v1 태그가 사라짐")
        with open(new, "r+b") as f:
            f.seek(os.path.getsize(new) - 128)
            f.write(v1_orig)

    # ---- 검증 ----
    a2 = MP3(new)
    pics = a2.tags.getall("APIC") if a2.tags is not None else []
    if len(pics) != 1 or pics[0].type != 3 or pics[0].mime != "image/jpeg" or pics[0].data != cover:
        raise SafError("검증 실패: Front Cover APIC가 정확히 1개 들어있지 않음")
    if frame_snapshot(a2.tags) != before:
        raise SafError("검증 실패: 기존 ID3 프레임이 달라짐")
    if not files_equal_from(orig, id3v2_span(orig), new, id3v2_span(new)):
        raise SafError("검증 실패: ID3v2 태그 뒤의 오디오/꼬리 데이터가 원본과 다름")
    if os.path.getsize(new) < os.path.getsize(orig):
        raise SafError("검증 실패: 결과 파일이 원본보다 작음")
    streams2, dur_new = probe_streams(new)
    if not any(s.get("codec_type") == "audio" and s.get("codec_name") == "mp3" for s in streams2):
        raise SafError("검증 실패: 결과에 MP3 오디오 스트림 없음")
    pic = [s for s in streams2 if (s.get("disposition") or {}).get("attached_pic")]
    if len(pic) != 1 or (pic[0].get("width"), pic[0].get("height")) != (TARGET, TARGET):
        raise SafError("검증 실패: ffprobe에서 240x240 attached_pic가 보이지 않음")
    if abs(dur_new - dur_orig) > 0.05:
        raise SafError("검증 실패: 재생 시간 변화 %.3f -> %.3f" % (dur_orig, dur_new))
    return "ok", "ID3v2.%d, %d바이트 -> %d바이트" % (ver, os.path.getsize(orig), os.path.getsize(new))


# --------------------------------------------------------------------------
# SD 반영 (원본은 검증 완료 전까지 Termux 쪽 pending/ 에 백업)
# --------------------------------------------------------------------------
def selftest_truncate(root_uri):
    for e in saf_ls(root_uri):
        if str(e.get("name", "")).startswith(SELFTEST_NAME):
            saf_rm(e["uri"])
    uri = saf_create(root_uri, SELFTEST_NAME)
    tmpdir = os.environ["ASMR_WORK"]
    big, small, back = (os.path.join(tmpdir, n) for n in ("st_big", "st_small", "st_back"))
    with open(big, "wb") as f:
        f.write(b"A" * 8192)
    with open(small, "wb") as f:
        f.write(b"B" * 16)
    try:
        saf_write_from_file(uri, big)
        saf_read_to_file(uri, back)
        if open(back, "rb").read() != b"A" * 8192:
            return "broken", "8192바이트 쓰기 후 읽은 내용이 다름"
        saf_write_from_file(uri, small)
        saf_read_to_file(uri, back)
        got = open(back, "rb").read()
        if got == b"B" * 16:
            return "truncate", "덮어쓰기 시 기존 내용이 잘림(truncate) — 안전"
        if got[:16] == b"B" * 16 and len(got) == 8192:
            return "no_truncate", "덮어쓰기 시 파일이 잘리지 않음(뒤에 옛 데이터가 남음)"
        return "broken", "예상하지 못한 결과 (%d바이트)" % len(got)
    finally:
        try:
            saf_rm(uri)
        except SafError:
            pass
        for p in (big, small, back):
            if os.path.exists(p):
                os.remove(p)


def write_and_verify(uri, path, workdir):
    want_sha = sha256_file(path)
    want_len = os.path.getsize(path)
    saf_write_from_file(uri, path)
    st = saf_stat(uri)
    L = entry_length(st)
    if L is not None and L != want_len:
        raise SafError("반영 후 SAF 크기 %d != %d" % (L, want_len))
    back = os.path.join(workdir, "readback.mp3")
    try:
        saf_read_to_file(uri, back)
        if sha256_file(back) != want_sha:
            raise SafError("반영 후 다시 읽은 내용의 SHA-256 불일치")
    finally:
        if os.path.exists(back):
            os.remove(back)


def restore_original(uri, orig, workdir, trunc_mode):
    if trunc_mode != "truncate":
        cur = entry_length(saf_stat(uri))
        if cur is not None and cur > os.path.getsize(orig):
            raise SafError("SAF 쓰기가 truncate 하지 않아 덮어쓰기로 복원 불가")
    write_and_verify(uri, orig, workdir)


def apply_one(r, ctx):
    m, j = r["mp3"], r["jpg"]
    wd = ctx["work"]
    orig, new = os.path.join(wd, "orig.mp3"), os.path.join(wd, "new.mp3")
    for p in (orig, new):
        if os.path.exists(p):
            os.remove(p)
    try:
        st = saf_stat(m["uri"])
        saf_read_to_file(m["uri"], orig, entry_length(st))
    except SafError as e:
        return "READ_FAIL", str(e)
    try:
        cover = ctx["images"].cover(j)
        kind, info = embed(orig, new, cover)
    except SafError as e:
        return "APPLY_FAIL", "변환/검증 실패(원본 그대로): %s" % e
    if kind == "existing":
        return "SKIP_EXISTING", info

    # 반영 직전: 원본을 pending/ 으로 옮겨 둔다 (중단 시 --restore-pending 으로 복구)
    pid = short_hash(m["uri"])
    pdir = os.path.join(ctx["pending"], pid)
    os.makedirs(pdir, exist_ok=True)
    porig = os.path.join(pdir, "orig.mp3")
    shutil.move(orig, porig)
    orig_sha = sha256_file(porig)
    with open(os.path.join(pdir, "meta.json"), "w", encoding="utf-8") as f:
        json.dump({"uri": m["uri"], "path": m["full"], "sha256": orig_sha,
                   "size": os.path.getsize(porig), "time": datetime.datetime.now().isoformat()},
                  f, ensure_ascii=False)
    err = None
    for attempt in range(2):
        try:
            write_and_verify(m["uri"], new, wd)
            err = None
            break
        except SafError as e:
            err = e
    if err is None:
        if ctx["keep_backup"]:
            dest = os.path.join(ctx["backup"], *m["rel"].split("/"))
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            shutil.move(porig, dest)
        shutil.rmtree(pdir, ignore_errors=True)
        os.remove(new)
        return "APPLIED", info
    try:
        restore_original(m["uri"], porig, wd, ctx["trunc"])
    except SafError as e2:
        raise Fatal("SD 반영 실패(%s) 후 원본 복원도 실패(%s).\n  원본 백업: %s\n  대상: %s"
                    % (err, e2, porig, m["full"]))
    shutil.rmtree(pdir, ignore_errors=True)
    os.remove(new)
    return "APPLY_FAIL", "SD 반영 검증 실패 -> 원본으로 복원 완료: %s" % err


def restore_pending(pending_dir, workdir, out):
    items = sorted(os.listdir(pending_dir)) if os.path.isdir(pending_dir) else []
    if not items:
        out("복원할 pending 항목이 없습니다.")
        return 0
    bad = 0
    for pid in items:
        pdir = os.path.join(pending_dir, pid)
        try:
            meta = json.load(open(os.path.join(pdir, "meta.json"), encoding="utf-8"))
            porig = os.path.join(pdir, "orig.mp3")
            if sha256_file(porig) != meta["sha256"]:
                raise SafError("백업 파일 해시 불일치")
            write_and_verify(meta["uri"], porig, workdir)
            shutil.rmtree(pdir)
            out("복원 완료: %s" % meta["path"])
        except Exception as e:
            bad += 1
            out("복원 실패: %s (%s) — 백업은 %s 에 남아 있음" % (pid, e, pdir))
    return 1 if bad else 0


# --------------------------------------------------------------------------
# 출력 도우미
# --------------------------------------------------------------------------
STATUS_TEXT = {
    "EMBED": "EMBED 예정",
    "SKIP_EXISTING": "SKIP — 기존 썸네일 있음",
    "NO_IMAGE": "NO IMAGE",
    "AMBIGUOUS": "AMBIGUOUS — 적용하지 않음",
    "READ_FAIL": "READ FAIL",
    "MAP_ERROR": "MAP ERROR — 적용하지 않음",
    "EXCLUDED": "EXCLUDED — 매핑 파일에서 제외",
    "APPLIED": "APPLIED — 삽입 및 검증 완료",
    "APPLY_FAIL": "APPLY FAIL — 원본 보존",
}
ART_TEXT = {"yes": "있음", "no": "없음", "unknown": "확인 불가(적용 시 재확인)", None: "미확인(--no-probe)"}


def print_result(out, r, apply_mode):
    m = r["mp3"]
    out("  MP3: %s" % m["in_work"])
    if r["status"] == "AMBIGUOUS":
        out("  대응 JPG: 매칭 후보 %d개" % len(r["cands"]))
        for i, c in enumerate(r["cands"], 1):
            out("    후보 %d: %s" % (i, c["full"]))
        out("    MP3 전체 경로: %s" % m["full"])
        for o in r.get("others", []):
            out("    같은 이름 MP3: %s" % o["full"])
    elif r["jpg"] is not None:
        out("  대응 JPG: %s   [%s]" % (r["jpg"]["in_work"], r["rule"]))
    elif r["status"] in ("EXCLUDED",):
        out("  대응 JPG: -   [%s]" % r["rule"])
    else:
        out("  대응 JPG: 없음")
    out("  기존 썸네일: %s" % ART_TEXT.get(r.get("art"), r.get("art")))
    if r.get("img"):
        w, h = r["img"]
        shape = "정사각형" if w == h else "비율 유지 후 중앙 크롭"
        out("  이미지 크기: %d×%d → %d×%d (%s)" % (w, h, TARGET, TARGET, shape))
    if r.get("note"):
        out("  참고: %s" % r["note"])
    status = r["status"]
    text = STATUS_TEXT.get(status, status)
    if status == "EMBED" and apply_mode:
        text = "EMBED 대상"
    out("  결과: %s" % text)
    out("")


# --------------------------------------------------------------------------
def main():
    ap = argparse.ArgumentParser(
        prog="asmr_cover_embed.sh",
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description="SAF(외장 SD)의 작품 폴더별로 JPG를 240x240 Front Cover로 MP3에 삽입 (기본 DRY RUN)",
        epilog="""매칭 규칙 (작품 = SAF 루트 아래 --work-depth 단계 폴더, 그 안의 모든 하위 폴더 포함):
  0. --map 파일에 적힌 지정이 최우선 (같은 작품 안의 JPG만 허용)
  1. MP3 이름(확장자 제외)과 JPG 이름(확장자 제외, '01.mp3.jpg'면 '.mp3'도 제외)이
     같으면 후보. 비교는 유니코드 NFC + 대소문자 무시.
  2. 작품 안에 후보 JPG가 정확히 1개이고 같은 이름 MP3도 1개뿐이면 매칭.
  3. 후보 JPG가 여러 개이거나 같은 이름 MP3가 여러 폴더에 있으면 AMBIGUOUS
     (--prefer-same-dir 이면 MP3와 같은 폴더의 후보가 딱 1개일 때만 그것을 사용).
  4. 이름이 맞는 JPG가 없으면 NO IMAGE. --work-cover 일 때만 작품 대표 커버 사용:
     트랙 전용이 아닌 JPG가 1개뿐이거나, cover/folder/jacket… 이름의 JPG가 1개일 때.
  다른 작품의 JPG는 어떤 경우에도 후보가 되지 않는다.""")
    ap.add_argument("--apply", action="store_true", help="실제로 SD 카드의 MP3를 수정 (없으면 DRY RUN)")
    ap.add_argument("--yes", action="store_true", help="--apply 시 확인 질문 생략")
    ap.add_argument("--root-uri", default=os.environ.get("SAF_ROOT_URI", DEFAULT_ROOT_URI))
    ap.add_argument("--work-depth", type=int, default=1,
                    help="루트에서 몇 단계 아래 폴더를 '작품'으로 볼지 (기본 1: Asmr/작품). 서클/작품 구조면 2")
    ap.add_argument("--work", action="append", help="이름에 이 문자열이 들어간 작품만 처리 (여러 번 가능)")
    ap.add_argument("--work-cover", action="store_true", help="이름 일치 JPG가 없는 트랙에 작품 대표 커버 적용")
    ap.add_argument("--cover-name", action="append", help="대표 커버로 인정할 파일 이름(확장자 제외) 추가")
    ap.add_argument("--prefer-same-dir", action="store_true", help="같은 이름 후보 중 MP3와 같은 폴더의 것이 1개면 그것을 사용")
    ap.add_argument("--map", help="수동 매핑 TSV: 'MP3경로<TAB>JPG경로' / 'MP3경로<TAB>SKIP' / '작품/*<TAB>JPG경로'")
    ap.add_argument("--reuse-scan", action="store_true", help="이전 스캔 결과(scan.json) 재사용")
    ap.add_argument("--no-probe", action="store_true", help="DRY RUN에서 MP3 기존 썸네일/JPG 크기 확인 생략 (빠름)")
    ap.add_argument("--only-problems", action="store_true", help="AMBIGUOUS/NO IMAGE/실패만 화면에 출력")
    ap.add_argument("--limit", type=int, default=0, help="--apply 시 앞에서부터 N개만 적용 (시험용)")
    ap.add_argument("--keep-backup", action="store_true", help="적용 성공 후에도 원본 MP3를 Termux 쪽 backup/ 에 보관")
    ap.add_argument("--allow-no-truncate", action="store_true", help="SAF 쓰기가 truncate 하지 않아도 진행 (권장하지 않음)")
    ap.add_argument("--restore-pending", action="store_true", help="중단으로 남은 pending 백업을 SD에 되돌림")
    opts = ap.parse_args()

    state = os.environ["ASMR_STATE"]
    dirs = {k: os.path.join(state, k) for k in ("cache", "pending", "backup", "reports", "work")}
    for p in dirs.values():
        os.makedirs(p, exist_ok=True)
    os.environ["ASMR_WORK"] = dirs["work"]
    ts = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    out = Out(os.path.join(dirs["reports"], "report_%s.txt" % ts))

    if opts.restore_pending:
        return restore_pending(dirs["pending"], dirs["work"], out)

    if opts.apply and os.listdir(dirs["pending"]):
        out("중단된 작업의 원본 백업이 남아 있습니다: %s" % dirs["pending"])
        out("  먼저 'bash asmr_cover_embed.sh --restore-pending' 으로 복원하거나 내용을 확인하세요.")
        return 2

    mapping = ({}, {})
    if opts.map:
        exact, ww, problems = load_map(opts.map)
        if problems:
            for p in problems:
                out("매핑 파일 오류: " + p)
            return 2
        mapping = (exact, ww)

    root_label = urllib.parse.unquote(opts.root_uri.rstrip("/").split("/")[-1]).split(":")[-1] or "ROOT"
    try:
        root_label = saf_stat(opts.root_uri).get("name") or root_label
    except SafError as e:
        out("SAF 루트에 접근할 수 없습니다: %s" % e)
        out("  termux-saf-managedir 로 SD 카드의 해당 폴더 권한을 주었는지, termux-saf-dirs 로 URI를 확인하세요.")
        return 2

    out("=" * 60)
    out("모드: %s" % ("APPLY (실제 적용)" if opts.apply else "DRY RUN (변경 없음)"))
    out("SAF 루트: %s  (%s)" % (root_label, opts.root_uri))
    out("작품 단위: 루트 아래 %d단계 폴더 + 그 모든 하위 폴더" % opts.work_depth)
    out("옵션: work-cover=%s prefer-same-dir=%s map=%s" % (opts.work_cover, opts.prefer_same_dir, opts.map or "-"))
    out("=" * 60)

    # 1) 1회 순회
    data, reused = scan(opts.root_uri, os.path.join(dirs["cache"], "scan.json"), opts.reuse_scan)
    out("스캔: %s (%s)" % ("이전 결과 재사용" if reused else "새로 순회", data.get("scanned_at")))
    # 2,3) 그룹
    works, outside, stats = group(data, opts.work_depth, root_label)
    for d in data["dirs"]:
        if d.get("error"):
            out("디렉터리 읽기 실패: %s (%s)" % ("/".join([root_label] + d["rel"]), d["error"]))

    sel = list(works.values())
    if opts.work:
        keys = [norm(w) for w in opts.work]
        sel = [w for w in sel if any(k in norm(w["name"]) for k in keys)]

    # 4) 매칭
    all_results = []
    for w in sel:
        w["results"] = match_work(w, opts, mapping)
        all_results.extend(w["results"])

    # 탐색: 기존 썸네일 / 이미지 크기
    images = ImageCache(dirs["cache"])
    probe_cache_path = os.path.join(dirs["cache"], "probe.json")
    try:
        probe_cache = json.load(open(probe_cache_path, encoding="utf-8"))
    except Exception:
        probe_cache = {}
    if not opts.no_probe:
        n = len(all_results)
        for i, r in enumerate(all_results, 1):
            m = r["mp3"]
            progress("확인 중 %d/%d  %s" % (i, n, m["name"][-40:]))
            if r["status"] in ("EXCLUDED", "MAP_ERROR"):
                continue
            ck = "%s|%s|%s" % (m["uri"], m.get("length"), m.get("mtime"))
            try:
                art = probe_cache.get(ck)
                if art is None:
                    art = probe_art_head(m["uri"])
                    if art in ("yes", "no") and m.get("length") is not None:
                        probe_cache[ck] = art
                r["art"] = art
            except (SafError, OSError) as e:
                r["status"], r["note"] = "READ_FAIL", "MP3 읽기 실패: %s" % e
                continue
            if art == "yes":
                r["status"] = "SKIP_EXISTING"
                continue
            if r["status"] == "EMBED":
                try:
                    r["img"] = images.size(r["jpg"])
                except (SafError, OSError) as e:
                    r["status"], r["note"] = "READ_FAIL", "JPG 읽기/해석 실패: %s" % e
        progress_end()
        with open(probe_cache_path, "w", encoding="utf-8") as f:
            json.dump(probe_cache, f, ensure_ascii=False)

    # 5) 출력
    for w in sel:
        if not w["mp3"]:
            continue
        rows = w["results"]
        if opts.only_problems:
            rows = [r for r in rows if r["status"] not in ("EMBED", "SKIP_EXISTING", "EXCLUDED")]
            if not rows:
                continue
        out("[%s]   MP3 %d개 / JPG %d개" % (w["name"], len(w["mp3"]), len(w["jpg"])))
        if w["failed_dirs"]:
            out("  ! 이 작품의 하위 폴더 일부를 읽지 못함 — 결과가 불완전할 수 있음")
        if w["jpg"]:
            out("  작품 안 JPG:")
            for j in w["jpg"]:
                out("    - %s" % j["in_work"])
        out("")
        for r in rows:
            print_result(out, r, opts.apply)

    if outside:
        out("[작품 폴더 밖의 파일 — 처리하지 않음]")
        for it in outside:
            out("  %s" % it["full"])
        out("")

    # plan / 매핑 템플릿
    plan_path = os.path.join(dirs["reports"], "plan_%s.tsv" % ts)
    with open(plan_path, "w", encoding="utf-8") as f:
        f.write("status\twork\tmp3\tjpg\trule\tnote\n")
        for r in all_results:
            f.write("\t".join([r["status"], r["mp3"]["work"], r["mp3"]["rel"],
                               r["jpg"]["rel"] if r["jpg"] else "", r["rule"], r.get("note", "")]) + "\n")
    amb = [r for r in all_results if r["status"] == "AMBIGUOUS"]
    tmpl_path = None
    if amb:
        tmpl_path = os.path.join(dirs["reports"], "map_template_%s.tsv" % ts)
        with open(tmpl_path, "w", encoding="utf-8") as f:
            f.write("# AMBIGUOUS 항목 수동 지정용. 원하는 줄의 TAB 뒤에 JPG 경로를 남기고 '#' 을 지운 뒤\n")
            f.write("# --map 으로 넘기세요. 'SKIP' 은 제외. 경로는 SAF 루트 기준(작품/...).\n\n")
            for r in amb:
                f.write("# MP3: %s\n" % r["mp3"]["rel"])
                for c in r["cands"]:
                    f.write("#%s\t%s\n" % (r["mp3"]["rel"], c["rel"]))
                f.write("\n")

    # 6) 적용
    applied = failed = 0
    fatal = None
    mode = "truncate"
    if opts.apply:
        targets = [r for r in all_results if r["status"] == "EMBED"]
        if opts.limit > 0:
            targets = targets[:opts.limit]
        if targets:
            mode, why = selftest_truncate(opts.root_uri)
            out("termux-saf-write 덮어쓰기 자가 시험: %s — %s" % (mode, why))
            if mode == "broken" or (mode == "no_truncate" and not opts.allow_no_truncate):
                out("안전한 교체를 보장할 수 없어 적용을 중단합니다. (DRY RUN 결과는 위와 같음)")
                if mode == "no_truncate":
                    out("  결과 파일은 원본보다 작아지지 않게 만들기 때문에 --allow-no-truncate 로 진행할 수는 있지만,")
                    out("  실패 시 덮어쓰기 복원이 불가능하므로 권장하지 않습니다.")
                targets = []
            elif not opts.yes:
                out("%d개 MP3에 썸네일을 삽입합니다. 계속하려면 YES 를 입력하세요: " % len(targets))
                try:
                    ans = input().strip()
                except EOFError:
                    ans = ""
                if ans != "YES":
                    out("취소했습니다.")
                    targets = []
        ctx = {"work": dirs["work"], "pending": dirs["pending"], "backup": os.path.join(dirs["backup"], ts),
               "images": images, "keep_backup": opts.keep_backup,
               "trunc": mode}
        for i, r in enumerate(targets, 1):
            out("[적용 %d/%d] %s" % (i, len(targets), r["mp3"]["full"]))
            out("  JPG: %s" % r["jpg"]["full"])
            try:
                st, info = apply_one(r, ctx)
            except Fatal as e:
                fatal = str(e)
                out("  !!! 치명적 오류 — 즉시 중단합니다.\n  %s" % fatal)
                break
            r["status"], r["note"] = st, info
            if st == "APPLIED":
                applied += 1
            elif st == "APPLY_FAIL":
                failed += 1
            out("  결과: %s  (%s)" % (STATUS_TEXT.get(st, st), info))

    # 요약
    cnt = defaultdict(int)
    for r in all_results:
        cnt[r["status"]] += 1
    planned = cnt["EMBED"] + cnt["APPLIED"] + cnt["APPLY_FAIL"]
    out("")
    out("=" * 60)
    out("요약 (%s)" % ("APPLY" if opts.apply else "DRY RUN"))
    out("  검색 디렉터리 수        : %d%s" % (stats["dirs"], (" (읽기 실패 %d)" % stats["dir_fail"]) if stats["dir_fail"] else ""))
    out("  전체 MP3 수             : %d" % stats["mp3"])
    out("  전체 JPG 수             : %d" % stats["jpg"])
    out("  작품 수                 : %d%s" % (len(works), (" (선택 %d)" % len(sel)) if opts.work else ""))
    out("  처리 대상 수            : %d" % planned)
    out("  기존 썸네일로 건너뜀    : %d" % cnt["SKIP_EXISTING"])
    out("  이미지가 없는 MP3       : %d" % cnt["NO_IMAGE"])
    out("  매칭이 모호한 MP3       : %d" % cnt["AMBIGUOUS"])
    out("  읽기 실패               : %d" % cnt["READ_FAIL"])
    out("  적용 성공               : %d" % applied)
    out("  적용 실패               : %d" % failed)
    if cnt["MAP_ERROR"] or cnt["EXCLUDED"]:
        out("  매핑 오류 / 매핑 제외   : %d / %d" % (cnt["MAP_ERROR"], cnt["EXCLUDED"]))
    if outside:
        out("  작품 폴더 밖 파일       : %d (처리 안 함)" % len(outside))
    if stats["ignored_hidden"]:
        out("  숨김('.'으로 시작) 무시 : %d" % stats["ignored_hidden"])
    if opts.apply and planned > applied + failed and not fatal:
        out("  미적용(취소/limit)      : %d" % (planned - applied - failed))
    out("=" * 60)
    out("리포트: %s" % out.f.name)
    out("매칭표: %s" % plan_path)
    if tmpl_path:
        out("모호한 항목 매핑 템플릿: %s" % tmpl_path)
    if not opts.apply:
        out("DRY RUN 이었습니다. 위 매칭을 확인한 뒤 --apply 를 붙여 실행하세요.")
    return 3 if fatal else 0


if __name__ == "__main__":
    try:
        sys.exit(main())
    except KeyboardInterrupt:
        sys.stderr.write("\n중단됨. --apply 도중이었다면 pending 폴더를 확인하세요 (--restore-pending).\n")
        sys.exit(130)
PYEOF

export ASMR_STATE="$STATE_DIR"
exec "$PY" "$PY_FILE" "$@"
