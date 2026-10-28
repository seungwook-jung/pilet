# 모딩 가이드: 밸런스 조절, 데이터 수정, 히든 콘텐츠

이 포트에서 게임은 **인터프리터 안의 게스트 메모리**로 존재합니다. 그래서 모딩은
바이너리 패치가 아니라 "로드 후 게스트 메모리에 쓰기"이고, 함수 호출을 가로채는 것도
호스트 C 한 줄입니다. 가능한 것과 아닌 것을 단계별로 나눕니다.

## 0. 준비: 무엇이 어디 있는지

```sh
python3 tools/inotia_tool.py inspect  your.apk --json report.json   # 전체 보고서
python3 tools/inotia_tool.py symbols  your.apk --out symbols.txt    # 5,900여 개 함수 이름/주소
python3 tools/inotia_tool.py symbols  your.apk --grep 'ITEM|MONSTER|EXP|LEVEL|GOLD|DROP|SKILL'
python3 tools/inotia_tool.py assets   your.apk --list                # 에셋 목록
python3 tools/inotia_tool.py modgen   your.apk --grep 'SAVE_' --out mods.txt   # trace 스켈레톤
```

보고서의 `function_prefix_families`가 엔진 모듈 지도입니다 (Vita 포트 기준 예:
`APPINFO_`, `GAMESTATE_`, `STATE_`, `SAVE_`, `UI_`, `GRP_`, `SGL_`, `SOUNDSYSTEM_`,
`CS_knl*`, `TM_`, `NM_http*`, `GAMELOADER_`, `FONT_`, `CHECKTIME_`).

## 1단계. 함수 단위 패치 (지금 바로 가능)

`inotia3/mods.txt`:

```
ret   <심볼> <정수>    # 즉시 반환. 예: 체크 함수 무력화, 상수 반환 함수의 값 변경
nop   <심볼>
poke  <심볼>[+오프셋] u8|u16|u32|f32 <값>   # 전역 변수/상수 테이블 쓰기
trace <심볼> [횟수]     # 호출 로그(r0~r3, lr) — 어떤 함수가 언제 무엇을 들고 불리는지
fps   <ms>
```

- 주소는 `symbols`가 출력하는 **이미지 상대 주소**(로더가 베이스를 더함). 심볼 이름이 더 안전.
- `poke`는 **로드 직후**(엔진 코드 실행 전)에 적용됩니다. `.data`의 초기값·상수 테이블에는
  통하고, 실행 중 코드가 초기화하는 `.bss` 값은 나중에 덮어써지므로 2단계 방식이 필요.
- `ret`는 반환값만 바꾸고 인자 기반 분기는 못 합니다. 예: "경험치 2배"처럼 **인자에
  곱하기**가 필요하면 2단계.

예시 (이름은 본인 `libgame.so`에서 확인 필요):
```
trace GAMESTATE_SetState 200      # 상태 전이 흐름 파악
trace SAVE_SaveData 10            # 세이브 경로 확인
```

## 2단계. 호스트 훅으로 인자·반환값 가공 (C 몇 줄, 재빌드)

업스트림 엔진에는 `patches_replace_with_import(img, "심볼", "host/이름")`가 있어 게스트 함수를
**호스트 C 구현으로 통째로 대체**합니다 (`File::GetFullPathName`, libgcc 나눗셈이 그렇게
되어 있음). 밸런스 훅은 보통 "원본을 부른 뒤 결과를 바꾸기"라서 다음 패턴이 편합니다:

```c
/* overlay/src/mods_hooks.c (예시, 실제 심볼명은 symbols.txt에서) */
static uint32_t orig_GetExp;           /* 원본 진입점 (Thumb 비트 포함) */
static void host_GetExp(void) {
    uint32_t args[4] = { R0, R1, R2, R3 };
    uint32_t v = cpu_call_guest(orig_GetExp, args, 4, 50000000);   /* 원본 실행 */
    R0 = v * 2;                        /* 경험치 2배 */
}
```
`cpu_call_guest()`는 업스트림이 qsort 콜백을 위해 만든 **재진입 호출** API입니다
(`cpu.h`). 원본 코드를 다른 주소에 복사해 두고(프롤로그 덮어쓰기 전 사본) 그쪽을
호출하는 식으로 트램펄린을 만들면 됩니다. 이 레이어는 아직 레포에 없고, `mods.c`에
`hook <심볼> <호스트함수>` 지시문으로 붙이는 게 다음 작업입니다.

## 3단계. 데이터 테이블 수정 (포맷 역분석 필요)

- 게임 데이터는 APK `assets/`에 LZMA 등으로 포장돼 있고, 첫 실행 시 엔진이
  `data/<패키지>/`에 설치합니다 (`i_pack.dat` ≈ 1.36 MB, `i_tile.dat`, `i_mapfeature.dat` 등
  12개). 몬스터/아이템/스킬 수치는 거의 확실히 여기 있습니다.
- 포맷을 아는 가장 빠른 길: 엔진의 `--watch=ADDR`(호스트 쓰기 감시)와 `trace`로
  "레벨업 때 어떤 함수가 어느 주소를 읽는가"를 잡고, 그 주소가 어느 파일의 어느
  오프셋에서 로드됐는지 `fopen/fread` 로그(`host_libc.c`의 I/O 로그)로 역추적.
- 포맷이 잡히면 (a) `data/` 설치본을 직접 편집하거나 (b) `poke`로 로드 후 테이블을 덮어쓰거나
  (c) 호스트 `fread` 훅에서 파일 내용을 교체하는 "가상 패치 파일" 방식 중 택일. (c)가
  세이브 호환·업데이트 내성 면에서 가장 좋음.

## 4단계. 히든 맵·몹 추가 (가장 큰 작업)

- 맵은 `i_tile.dat`/`i_mapfeature.dat` 류, 몬스터 스프라이트는 `i_pack.dat` 내 리소스.
  "기존 맵 복제 + 포탈 연결 + 스폰 테이블 추가"가 현실적인 최소 단위입니다.
- 엔진의 맵 전환 함수(`STATE_`/`MAP_` 계열)를 `trace`로 잡아 인자(맵 ID, 좌표)를 알아내면,
  2단계 훅으로 "특정 좌표에서 A 누르면 맵 X로"라는 **포탈을 코드 없이 데이터 없이**
  먼저 만들 수 있습니다. 맵 데이터 자체를 새로 만드는 건 3단계의 포맷 역분석 이후.
- 현실적인 순서: 포탈 훅(2단계) → 몬스터 스탯 테이블 수정(3단계) → 기존 맵에 스폰 추가
  → 새 맵 데이터.

## 5. 세이브 안전

- 세이브는 `inotia3/data/` 아래 `.dat` 파일. 모딩 전 백업.
- 밸런스 모드로 만든 세이브는 원본에서도 로드되지만 수치 차이로 진행이 깨질 수 있음.

## 6. 자동화 가능 범위 정리

| 작업 | 자동화 |
|---|---|
| 심볼 찾기·trace 스켈레톤 | `inotia_tool.py` (완료) |
| 함수 리턴/상수/메모리 패치 | `mods.txt` (완료) |
| 인자 가공 훅 | C 작성 필요, 패턴 제공 (`cpu_call_guest`) |
| 데이터 포맷 | 역분석 수작업 + 도구 지원(`--watch`, I/O 로그) |
| 신규 맵/몹 | 포맷 역분석 후 에디터 작성 |
