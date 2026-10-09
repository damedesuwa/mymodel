# asmr_cover_embed.sh — Termux SAF MP3 썸네일 삽입기

외장 SD 카드(SAF)의 작품 폴더마다 JPG를 240×240 **Front Cover**로 만들어
같은 작품의 MP3에 넣는다. 기본 실행은 **DRY RUN**이며 `--apply`를 줘야만 파일이 바뀐다.

## 새 작품을 추가했을 때 (권장)

```bash
curl -fL -o ~/bin/asmr_cover_update.sh \
  "https://raw.githubusercontent.com/damedesuwa/mymodel/ccr-248d2e90-8vttzu/tools/termux-cover-embed/asmr_cover_update.sh"
bash ~/bin/asmr_cover_update.sh
```

`asmr_cover_update.sh`는 아래를 차례로 한다.

1. SD 카드를 새로 스캔한다.
2. 커버가 없는 MP3만 작품 단위로 요약한다.
3. `y`라고 답하면 그 곡들에만 적용한다.

규칙은 `--work-cover --other-images`와 같다. 작품 안 이미지가 1장이면 그 작품의 커버 없는 곡 전부에 넣는다. 여러 장이면 후보만 보여 주고 넣지 않는다.

## 설치

```bash
pkg update
pkg install python ffmpeg termux-api
pip install mutagen
# Termux:API 앱은 Termux와 같은 출처(F-Droid 또는 GitHub)에서 설치

# SD 카드 Asmr 폴더 권한 (이미 줬다면 생략) 과 URI 확인
termux-saf-managedir
termux-saf-dirs

# 스크립트 저장
mkdir -p ~/bin && cp asmr_cover_embed.sh ~/bin/ && chmod +x ~/bin/asmr_cover_embed.sh
```

## 실행

```bash
# 1) DRY RUN: 매칭표 확인 (아무것도 바꾸지 않음)
bash ~/bin/asmr_cover_embed.sh

# 문제(AMBIGUOUS / NO IMAGE / 실패)만 보기. 스캔 결과는 재사용
bash ~/bin/asmr_cover_embed.sh --reuse-scan --only-problems

# 2) 작품 하나, 3곡만 시험 적용
bash ~/bin/asmr_cover_embed.sh --reuse-scan --work "작품 A" --apply --limit 3

# 3) 전체 적용
bash ~/bin/asmr_cover_embed.sh --reuse-scan --apply
```

루트 URI 기본값은 `content://com.android.externalstorage.documents/tree/0000-0000%3AAsmr`이고,
`--root-uri` 또는 환경 변수 `SAF_ROOT_URI`로 바꿀 수 있다.
리포트, 매칭표(`plan_*.tsv`), 모호한 항목용 매핑 템플릿은 `~/.asmr_cover/reports/`에 저장된다.

## 매칭 규칙

**작품**은 SAF 루트 바로 아래 폴더다(`Asmr/작품 A`). 서클/작품처럼 한 단계 더 깊다면 `--work-depth 2`를 쓴다.
작품 폴더와 **그 아래 모든 하위 폴더**에서 MP3와 JPG를 모으고, 매칭은 그 작품 안에서만 한다.
다른 작품의 JPG는 어떤 규칙으로도 후보가 되지 않는다. 루트에 바로 있는 파일은 처리하지 않는다.

| 순서 | 조건 | 결과 |
|---|---|---|
| 0 | `--map` 파일에 이 MP3가 지정됨 (JPG가 같은 작품 안에 있을 때만 허용) | EMBED / SKIP |
| 1 | 이름 키가 같은 JPG가 작품 안에 **정확히 1개**이고, 같은 이름 키의 MP3도 1개뿐 | EMBED |
| 2 | 같은 이름의 JPG가 여러 개, 또는 같은 이름의 MP3가 여러 폴더에 있음 | **AMBIGUOUS**: 후보 전체 경로를 출력하고 적용하지 않음 |
| 2' | `--prefer-same-dir`이고, 후보 중 MP3와 **같은 폴더**의 JPG가 딱 1개 | EMBED |
| 3 | 이름이 맞는 JPG 없음 | **NO IMAGE** |
| 3' | `--work-cover`: 작품 안 JPG가 1개뿐(이름 무관)이거나, 트랙 전용이 아닌 JPG가 1개뿐이거나 `cover`/`folder`/`jacket`/`ジャケット`… 이름의 JPG가 1개 | EMBED (작품 대표 커버) |
| 3'' | `--work-cover`인데 대표 커버 후보가 여럿 | AMBIGUOUS |

- 이름 키는 확장자를 뺀 파일명을 유니코드 NFC로 정규화하고 대소문자를 무시한 것이다. `01.mp3.jpg`처럼 생긴 이미지는 `01`로 본다.
- 이름이 어떤 MP3와 일치하는 JPG는 "트랙 전용"이므로 대표 커버 후보에서 빠진다.
- JPG가 하나뿐이어도 `--work-cover` 없이는 자동 적용하지 않는다.
- `._01.mp3` 같은 숨김 파일은 무시한다.
- 이미 그림이 들어 있는 MP3(APIC/PIC 프레임 또는 attached_pic 스트림)는 SKIP한다.

`--map` 파일은 TAB으로 구분하며, 경로는 SAF 루트 기준이다. DRY RUN이 만든 `map_template_*.tsv`를 고쳐서 쓰면 된다.

```
작품 A/audio/02.mp3	작품 A/jacket/02.jpg
작품 A/bonus/03.mp3	SKIP
작품 B/*	작품 B/scans/front.jpg
```

맨 아래 줄 형식(`작품 B/*`)은 그 작품에서 이름이 맞는 JPG가 없는 트랙에 대표 커버를 지정한다.

## 안전장치

- 원본 JPG는 읽기만 한다. 240×240 이미지는 Termux 캐시에서 만든다. ffmpeg로 비율을 유지한 채 확대·축소한 뒤 중앙을 잘라 baseline JPEG로 저장한다.
- MP3는 mutagen으로 **ID3 태그만** 다시 쓴다. 오디오 바이트는 재인코딩하지도, 리먹싱하지도 않는다. 기존 프레임(TRCK, TPOS 등)과 ID3 버전(v2.3/v2.4)은 그대로 두고, ID3v1 128바이트는 원본 그대로 되돌려 놓는다.
- 삽입 결과는 아래를 모두 통과해야만 SD 카드에 반영한다.
  - Front Cover APIC가 정확히 1개 있고, 그 데이터가 만든 이미지와 같다.
  - 기존 프레임이 바뀌지 않았다.
  - ID3v2 태그 뒤의 모든 바이트가 원본과 같다.
  - ffprobe에서 240×240 attached_pic가 보인다.
  - 재생 시간이 바뀌지 않았다.
- `--apply`를 하면 먼저 `termux-saf-write`가 덮어쓸 때 파일을 truncate하는지 SD 카드 루트의 임시 파일로 자가 시험한다. truncate하지 않으면 적용을 중단한다.
- 결과 파일은 원본보다 작아지지 않게 만든다. 그래서 같은 URI에 덮어쓰기만 하면 되고, 파일 삭제나 재생성(디렉터리 순서 변경)은 하지 않는다.
- 반영하기 전에 원본을 `~/.asmr_cover/pending/`으로 옮겨 둔다. 반영한 뒤에는 SAF 크기와 다시 읽은 내용의 SHA-256을 확인한다. 실패하면 원본을 다시 써 넣고 검증한다. 그 복원마저 실패하면 즉시 중단하며, 백업은 pending에 남는다.
- 중간에 끊겼다면 `bash asmr_cover_embed.sh --restore-pending`을 실행한다. pending이 남아 있으면 `--apply`는 시작하지 않는다.
- `--keep-backup`을 쓰면 성공한 원본도 `~/.asmr_cover/backup/<시각>/`에 보관한다. 용량에 주의해야 한다.

## 알려진 한계

- JPEG의 EXIF 회전 정보는 반영하지 않는다.
- 파일 끝에 붙은 ID3v2 태그나 APEv2 커버는 DRY RUN에서 보지 못한다. 적용할 때 ffprobe로 다시 확인한다.
- termux-saf-* 호출이 느리므로, 처음 전체 스캔과 기존 썸네일 확인에는 수십 분이 걸릴 수 있다. 이후에는 `--reuse-scan`과 확인 결과 캐시를 쓴다.
