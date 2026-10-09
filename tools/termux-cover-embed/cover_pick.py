"""JPG가 2개 이상인 작품의 커버 후보를 그림으로 보여 주고, 고른 번호로 매핑 파일을 만든다.

  python3 cover_pick.py              # 후보 미리보기를 Download/cover_pick 에 만들기
  python3 cover_pick.py W1=2 W3=1    # 고른 번호로 ~/cover_map.tsv 작성

SD 카드의 파일은 읽기만 한다. 아무것도 옮기거나 지우지 않는다.
"""
import json
import os
import shutil
import subprocess
import sys

DEPTH = int(os.environ.get("WORK_DEPTH", "1"))
STATE = os.path.expanduser(os.environ.get("ASMR_COVER_HOME", "~/.asmr_cover"))
SCAN = os.path.join(STATE, "cache", "scan.json")
OUT = os.path.expanduser(os.environ.get("PICK_DIR", "~/storage/downloads/cover_pick"))
LIST = os.path.join(STATE, "cover_pick.json")
MAP = os.path.expanduser("~/cover_map.tsv")


def works_with_many_jpgs():
    d = json.load(open(SCAN, encoding="utf-8"))
    works = {}
    order = []
    for dd in d["dirs"]:
        rel = dd["rel"]
        if len(rel) < DEPTH:
            continue
        name = "/".join(rel[:DEPTH])
        if name not in works:
            works[name] = []
            order.append(name)
        for f in dd["files"]:
            n = f["name"]
            if not n.startswith(".") and n.lower().endswith((".jpg", ".jpeg")):
                works[name].append({"rel": "/".join(rel + [n]), "uri": f["uri"]})
    return [(w, works[w]) for w in order if len(works[w]) >= 2]


def make_previews():
    if not os.path.isdir(os.path.dirname(OUT)):
        sys.exit("다운로드 폴더가 없습니다. 먼저 termux-setup-storage 를 실행하세요.")
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)
    picks = {}
    for i, (work, jpgs) in enumerate(works_with_many_jpgs(), 1):
        wid = "W%d" % i
        picks[wid] = {"work": work, "jpgs": [j["rel"] for j in jpgs]}
        print("[%s] %s" % (wid, work))
        for k, j in enumerate(jpgs, 1):
            src = os.path.join(OUT, "_src.jpg")
            with open(src, "wb") as fh:
                r = subprocess.run(["termux-saf-read", j["uri"]], stdout=fh, stderr=subprocess.DEVNULL)
            dst = os.path.join(OUT, "%s_%d.jpg" % (wid, k))
            ok = r.returncode == 0 and subprocess.run(
                ["ffmpeg", "-nostdin", "-loglevel", "error", "-y", "-i", src, "-vf",
                 "scale=480:480:force_original_aspect_ratio=increase,crop=480:480",
                 "-frames:v", "1", "-update", "1", dst]).returncode == 0
            os.remove(src)
            in_work = j["rel"][len(work) + 1:]
            print("    %d: %s%s" % (k, in_work, "" if ok else "   (미리보기 실패)"))
        print()
    json.dump(picks, open(LIST, "w", encoding="utf-8"), ensure_ascii=False)
    try:  # 갤러리에 바로 보이게 (없어도 파일 앱에서는 보임)
        subprocess.run(["termux-media-scan", "-r", OUT], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    except OSError:
        pass
    print("미리보기: 갤러리(또는 파일 앱)의 Download/cover_pick 폴더")
    print("  W1_1.jpg = W1 작품의 1번 후보를 커버로 잘랐을 때 모습")
    print("고른 뒤:  python3 ~/bin/cover_pick.py W1=번호 W2=번호 ...")


def write_map(args):
    picks = json.load(open(LIST, encoding="utf-8"))
    chosen = {}
    for a in args:
        wid, _, num = a.upper().partition("=")
        if wid not in picks or not num.isdigit() or not 1 <= int(num) <= len(picks[wid]["jpgs"]):
            sys.exit("잘못된 선택: %s" % a)
        chosen[wid] = int(num)
    missing = [w for w in picks if w not in chosen]
    with open(MAP, "w", encoding="utf-8") as f:
        for wid, num in chosen.items():
            p = picks[wid]
            f.write("%s/*\t%s\n" % (p["work"], p["jpgs"][num - 1]))
            print("%s %s  ->  %s" % (wid, p["work"], p["jpgs"][num - 1]))
    print("저장: %s" % MAP)
    if missing:
        print("아직 고르지 않은 작품: %s (이 작품들은 적용되지 않음)" % ", ".join(missing))


if __name__ == "__main__":
    if len(sys.argv) > 1:
        write_map(sys.argv[1:])
    else:
        make_previews()
