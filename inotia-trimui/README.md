# 이노티아 연대기 → TrimUI Brick 포팅 (inotia-trimui)

이노티아 연대기(Com2uS)를 TrimUI Brick(Allwinner A133P, aarch64 Linux)에서
**네이티브 실행 파일 형태로** 구동하기 위한 빌드 파이프라인, 패키지, 모딩 레이어,
그리고 APK 분석 도구입니다. 게임 데이터(APK, libgame.so)는 포함하지 않으며,
사용자가 직접 보유한 APK를 사용합니다.

> **결론 요약**
> - 가능한 경로는 **이노티아 3 (Children of Carnia, Android APK v1.3.2)** 입니다.
>   이미 공개된 두 리버스 엔지니어링 결과물(PS Vita 포트, WebAssembly 포트)을
>   조합하면 TrimUI Brick용 aarch64 바이너리가 **그대로 빌드됩니다** (이 레포에서 검증).
> - 이노티아 1·2는 Android 네이티브 빌드가 없고(WIPI/iOS), 이노티아 4는 ARMv7
>   라이브러리라 현재 인터프리터 범위를 벗어납니다. 자세한 건 `docs/alternatives.md`.
> - **실기 검증은 아직 못 했습니다.** 이 세션에는 APK도 기기도 없어서, "빌드가
>   되고 패키지가 만들어진다"까지만 확인했습니다. 첫 실행 시 가장 먼저 볼 것은
>   `inotia3/inotia3.log`입니다 (`docs/feasibility.md`의 체크리스트 참고).

## 왜 "그냥 네이티브 로드"가 아니라 인터프리터인가

| 사실 | 근거 | 결과 |
|---|---|---|
| 이노티아 3의 `libgame.so`는 **ELF32 ARMv5TE / Thumb-1 / soft-float** | Inotia3-wasm의 분석 (`Tag_CPU_arch v5TE`, 96% Thumb) | 32비트 코드 |
| TrimUI Brick 펌웨어는 **aarch64 전용** 유저랜드 (32비트 GL 라이브러리 없음) | PortMaster 문서: Smart Pro/Brick에서 gmloader(armhf)·box86 계열 포트 불가 | 32비트 .so를 그대로 `dlopen`/so-loader로 못 돌림 |
| 게임 자체는 15 fps(66 ms 프레임), 프레임당 약 2.1M 명령 | Inotia3-wasm `docs/PERFORMANCE.md` | 인터프리터로도 예산 안에 들어올 여지 |

따라서 **원본 `libgame.so`를 64비트 호스트 안에서 ARMv5 인터프리터로 실행**하는
suchangv/Inotia3-wasm 엔진을 기반으로 하고, 그 위에 TrimUI용 오버레이(버튼→터치
매핑, 모드 시스템, 런처)를 얹었습니다. gmloader-next 방식(안드로이드 .so를 네이티브
로드)은 **arm64-v8a 라이브러리가 있는 게임에만** 해당하며, 이노티아 3에는 없습니다.

## 디렉터리

```
inotia-trimui/
  README.md              이 문서
  docs/feasibility.md    조사 결과·판단 근거·실기 체크리스트·남은 리스크
  docs/modding.md        밸런스/데이터/히든 콘텐츠 모딩 방법
  docs/alternatives.md   이노티아 4·1·2, 다른 경로(네이티브 로더, AOT, 재구현)
  tools/inotia_tool.py   APK 분석(ABI·ISA·JNI·임포트·에셋), 심볼 덤프, 포트 준비, 모드 스켈레톤
  overlay/               엔진 소스에 덧씌우는 파일과 패처 (pad_map, mods, apply_overlay.py)
  port/                  TrimUI용 런처·설정 (launcher.sh, pad.txt, env.txt, port.json, README.txt)
  mods/mods.txt          기본 모드 파일 (전부 주석; 원본 그대로 실행)
  scripts/               setup / fetch / build / package
```

## 빌드 (x86_64 Ubuntu/Debian 기준, 루트 권한 불필요)

```sh
cd inotia-trimui
scripts/setup.sh          # apt 패키지 + pip(ziglang, pyelftools) + zig 크로스 툴체인 래퍼
scripts/fetch.sh          # Inotia3-wasm(고정 커밋), gl4es, SDL2 2.30.9, 나눔고딕(OFL)
scripts/build.sh          # gl4es(aarch64) → SDL2(KMSDRM+ALSA, aarch64) → 엔진+오버레이
scripts/package.sh --zip  # dist/Inotia3.sh + dist/inotia3/ (+ Inotia3.zip)
```

- 크로스 컴파일러는 **zig cc** (`aarch64-linux-gnu.2.28`)라 별도 sysroot가 필요 없고,
  결과물의 glibc 요구는 2.27 이하로 확인했습니다 (오래된 펌웨어에서도 로드됨).
- 외부 소스는 모두 빌드 시점에 받습니다(레포에 벤더링하지 않음). Inotia3-wasm은
  저장소에 LICENSE 파일이 없어서 재배포 대신 "고정 커밋 fetch + 오버레이" 방식을
  택했습니다. `docs/feasibility.md`의 라이선스 항목 참고.

## 설치 (TrimUI Brick)

1. 본인 APK에서 파일 두 개를 꺼냅니다 (PC에서):
   ```sh
   python3 tools/inotia_tool.py inspect  inotia3-1-3-2.apk        # 판정: route=interpreter 인지 확인
   python3 tools/inotia_tool.py prepare  inotia3-1-3-2.apk dist/inotia3
   ```
   → `dist/inotia3/libgame.so`, `dist/inotia3/assets.apk`
2. `dist/Inotia3.sh`와 `dist/inotia3/`를 SD 카드의 Ports 폴더(PortMaster가 보는 곳)에 복사합니다.
   - MinUI/NextUI + [minui-portmaster](https://github.com/ben16w/minui-portmaster): `Roms/Ports (PORTS)/`
   - Knulli: `/userdata/roms/ports/`
   - muOS: `/mnt/mmc/ROMS/Ports/` 등 (런처가 PortMaster 폴더를 여러 경로에서 찾습니다)
3. 실행 후 안 뜨면 `inotia3/inotia3.log`를 봅니다.

## 조작 (Brick: 터치스크린·스틱 없음)

게임은 터치 전용이라, 모든 버튼은 **화면 HUD의 고정 좌표를 누르는 가상 손가락**입니다
(PS Vita 포트의 좌표표). `port/pad.txt`에서 바꿀 수 있습니다.

| 입력 | 동작 |
|---|---|
| 십자키 | 이동 (화면 십자키 탭) |
| A(남) / B(동) / X(서) / Y(북) | 공격·확인 / 스킬1 / 스킬2 / 아이템·방패 |
| L1 / R1 / L2 | 퀵슬롯 1/2/3 |
| Start | 일시정지 메뉴 |
| **SELECT 홀드** | 커서 모드: 십자키로 포인터 이동, A로 클릭 (메뉴 조작용) |
| R2 | 포인터 위치 클릭 |
| SELECT + START | 종료 |

## 모딩

`inotia3/mods.txt`에 한 줄씩 지시문을 적습니다. 심볼 이름은 Com2uS가 스트립하지 않은
`.dynsym`에서 나오며(`SAVE_*`, `GAMESTATE_*`, `APPINFO_*`, `UI_*`, `GRP_*` 등 약 6천 개),
`tools/inotia_tool.py symbols your.apk --grep 'MONSTER|ITEM|EXP'`로 찾습니다.

```
ret   <심볼|0x주소> <정수>        함수가 즉시 그 값을 반환
nop   <심볼|0x주소>              함수가 즉시 0 반환
poke  <심볼|0x주소>[+오프셋] u8|u16|u32|f32 <값>   로드 후 메모리 쓰기
trace <심볼|0x주소> [횟수]        호출 시 r0~r3·lr 로그 (리버싱용)
fps   <ms>                       프레임 주기 (66=원본; 게임 속도가 바뀜)
```

밸런스(경험치·드랍·체력), 히든 맵·몬스터 추가 같은 **데이터 레벨 모딩의 구체적 방법과
한계**는 `docs/modding.md`에 정리했습니다. 요지는: 함수 단위 패치는 지금 바로 가능,
데이터 테이블 수정은 `i_pack.dat` 등 설치 데이터 포맷을 `trace`/`--watch`로 역분석한
뒤 가능, 신규 맵 추가는 에셋 포맷 역분석이 선행되어야 함.

## 출처·감사

- [MetalSyntax/Inotia3-vita](https://github.com/MetalSyntax/Inotia3-vita) (MIT): 엔진 수명주기, JNI 표면, 안티탬퍼 우회, 터치 좌표
- [suchangv/Inotia3-wasm](https://github.com/suchangv/Inotia3-wasm): ARMv5 인터프리터, 호스트 계층, SDL/PortMaster 빌드
- [ptitSeb/gl4es](https://github.com/ptitSeb/gl4es) (MIT), [SDL2](https://github.com/libsdl-org/SDL) (zlib), 나눔고딕 (SIL OFL 1.1)
- [PortsMaster/gmloader-next](https://github.com/PortsMaster/gmloader-next) (GPLv2): arm64 라이브러리용 대안 경로의 참고 구현
- 이노티아 연대기는 Com2uS의 저작물입니다. 이 레포는 비공식이며 게임 데이터를 포함하지 않습니다.
