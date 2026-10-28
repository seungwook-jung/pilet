# 타당성 조사와 판단 근거

조사일: 2026-10-28. 이 문서는 "어디까지 자동화가 가능한가"에 대한 답과, 그 답을 만든
근거, 그리고 **아직 확인하지 못한 것**을 적습니다.

## 1. 시리즈별 실행 파일 현황

| 작품 | 원 플랫폼 | 네이티브 코드 | TrimUI 경로 |
|---|---|---|---|
| 이노티아 연대기 1 (Legend of Feanor) | WIPI/피처폰, 후에 iOS | Android 네이티브 빌드 없음 | 없음 (iOS 바이너리는 Vita 포트 `Inotia2-vita`가 iOS→Vita 방식으로 다룸; Linux에선 별도 iOS 로더 필요) |
| 이노티아 2 (A Wanderer of Luone) | iOS/Android(2009) | Android 버전은 Java 중심으로 추정, 미확인 | 미확인 (APK가 있으면 `inotia_tool.py inspect`로 판정) |
| **이노티아 3 (Children of Carnia)** | Android/iOS(2010) | `lib/armeabi/libgame.so`, ARMv5TE Thumb-1, soft-float, 1.36 MB, `.dynsym` 5,962 함수 | **인터프리터 경로 (이 레포)** |
| 이노티아 4 (Assassin of Berkel) | Android/iOS(2012), F2P | `libgame.so` 계열로 추정, 2025년 3월에도 업데이트(1.3.7) → Play 정책상 arm64 포함 가능성 높음 | `docs/alternatives.md` 참고. ARMv7/Thumb-2면 인터프리터 확장, arm64면 네이티브 로더(gmloader-next식) 신규 작성 |

이노티아 3 APK는 공식 스토어에서 내려갔다는 리뷰가 많습니다(App Store 리뷰 "I wish
inotia 3 was still available"). **정품으로 보유한 APK v1.3.2**가 필요하며, 이 레포는
다운로드를 돕지 않습니다.

## 2. TrimUI Brick의 제약

- SoC Allwinner A133P (4× Cortex-A53 1.8 GHz), PowerVR GE8300, 1 GB RAM, 1024×768 3.2" 패널,
  스틱 없음, 터치 없음.
- 커널/유저랜드 aarch64. PortMaster 문서는 같은 SoC의 Smart Pro에 대해 "32비트 드라이버가
  없어 gmloader·box86 계열 포트가 안 돈다"고 명시. Brick도 동일 계열.
  → 32비트 `libgame.so`를 so-loader(Vita 방식)로 직접 로드하는 길은 **막혀 있음**.
- TrimUI는 Brick 커널 소스를 공개하지 않음(Knulli 문서). 32비트 호환을 커널에서 켜는
  것도 불가.
- GLES1 고정 파이프라인(`glOrthox`, `glColor4x` …)은 현대 드라이버에 없음 → gl4es로
  GLES2 변환. PowerVR GE8300은 GLES 3.x 지원이라 gl4es의 ES3 컨텍스트 요청은 문제없을 것.

## 3. 선택한 경로: Inotia3-wasm 인터프리터 + TrimUI 오버레이

Inotia3-wasm(2026-10)은 이노티아 3의 `libgame.so`를 **명령어 단위로 해석**하는 C 엔진입니다.
183개 임포트와 JNI 표면 전체를 호스트에서 답하고, Unicorn 기반 차분 테스트로 5,904개
함수 진입점의 명령 실행을 검증했으며, 브라우저·Android·SDL2(RK3566 PortMaster) 타깃이
있습니다. 이 레포가 한 일:

1. **크로스 빌드 체계**: Debian cross-gcc 대신 zig cc를 써서 sysroot 없이 glibc 2.28
   타깃으로 gl4es, SDL2(KMSDRM+offscreen+ALSA), 엔진을 빌드. 결과 바이너리의 glibc
   요구는 2.27 이하 (검증됨).
2. **오버레이** (`overlay/`):
   - `pad_map.c`: 터치 없는 기기용 버튼→HUD 탭 매핑, SELECT 홀드 커서 모드, 스틱→십자키,
     L2/R2 트리거 엣지 처리. `pad.txt`로 조정.
   - `mods.c`: `mods.txt` 기반 함수 리턴 패치/메모리 poke/호출 추적/프레임 주기.
   - `apply_overlay.py`: 고정 커밋의 `main.c`/`cpu.c`/`platform_sdl.c`에 앵커 문자열 검증을
     하며 최소 수정 삽입. 업스트림이 바뀌면 조용히 깨지지 않고 즉시 실패.
3. **런처** (`port/launcher.sh`): PortMaster 폴더를 TrimUI(MinUI/NextUI pak, Knulli, muOS,
   stock) 경로에서 탐색, PortMaster가 제공하는 기기용 SDL2를 우선 사용(없으면 동봉본),
   비디오 드라이버 프로브, 로그.
4. **도구** (`tools/inotia_tool.py`): APK를 넣으면 ABI·ARM 빌드 속성(CPU arch, Thumb ISA,
   FP ABI, NEON)·JNI 수출·임포트 분류·Com2uS 래퍼 지문·에셋 인벤토리를 뽑고,
   "인터프리터 / 네이티브 로더 / 불가" 판정을 냅니다. 합성 APK(ARMv5/ARMv7/arm64)로 테스트.

## 4. 검증된 것 / 안 된 것

검증됨 (이 세션):
- gl4es aarch64 정적 라이브러리 빌드 (`gl4es_gl*` 991개 심볼).
- SDL2 2.30.9 aarch64 공유 라이브러리: KMSDRM·offscreen·dummy 백엔드, `libEGL.so.1`/`libGLESv2.so.2`/`libdrm`/`libgbm` 런타임 dlopen, ALSA 링크.
- 엔진 + 오버레이 aarch64 링크 (`inotia3.aarch64`, rpath `$ORIGIN/libs`, GLIBC_2.27).
- 오버레이 패처 멱등성, 오버레이 소스의 x86_64 컴파일.
- `scripts/*` 파이프라인 전체를 빈 디렉터리에서 실행.
- `inotia_tool.py`의 inspect/symbols/modgen/prepare (합성 APK).

검증됨 (사용자 APK 수령 후, 2026-10-28):
- 제공된 APK의 `lib/armeabi/libgame.so`는 1,359,908 바이트, ARMv5TE/Thumb-1/VFPv2 soft-float,
  JNI 수출 40개(래퍼 수명주기 10/10), 엔진이 이름으로 찾는 심볼 32개와 HUD 숨김용 `VP_Draw*` 9개
  모두 존재, 프레임 주기 패치 지점(`APPINFO_Initialize`+0x4f: `movs r0,#0x42`) 일치 → 두 포트가
  기준으로 삼은 v1.3.2 빌드와 동일. `armeabi-v7a` 쪽 라이브러리도 바이트 단위로 같음.
- x86_64 인터프리터(헤드리스)로 실제 `libgame.so` 부팅: 안티탬퍼 우회, 데이터 12개 설치
  (`i_character/i_effect/i_item/i_mapfeature/i_monster0-4.dat` …), 로더 34단계, 메뉴 진입,
  스크립트 탭으로 새 게임 → 오프닝 → 첫 맵까지 `GAMESTATE 0→13→2`. 1,271 엔진 프레임 평균
  1,607,197 명령(업스트림 기준선과 같은 프레임 수), 플레이 중 프레임 2.08M 명령 / 13.2 ms (157 MIPS).
  미구현 임포트 호출 0건.
- `prepare`가 만든 에셋 전용 `assets.apk`(553개 항목, 전부 STORED, dex·서명 제외)로도 동일하게 부팅.
- aarch64 `inotia3.aarch64`를 qemu-user + Ubuntu arm64 sysroot에서 실행: 동적 링크·SDL 초기화·
  드라이버 프로브까지 정상(GL 라이브러리가 없는 sysroot라 창 생성에서 종료, 예상된 결과).

**검증 안 됨** (실기 부재):
- TrimUI 실기에서의 화면 출력·입력·사운드. (Inotia3-wasm은 PC Mesa에서 플레이까지 확인했고,
  RK3566 실기는 "검증 전"이라 적음. TrimUI는 그보다 한 단계 더 미지.)
- PowerVR + KMSDRM에서 SDL2 창 생성. TrimUI stock OS는 컴포지터가 없어 KMSDRM이 맞지만,
  PowerVR의 GBM 구현 유무는 실기에서 봐야 함. 실패 시 런처 로그의 `--vdprobe` 출력이
  어느 단계에서 막혔는지 알려줌. PortMaster가 제공하는 SDL2를 우선 쓰는 이유.
- 성능. Inotia3-wasm 측정: 데스크톱 168 MIPS에서 프레임당 12.8 ms, RK3566(A55)에서
  약 84 ms(libgcc 호스트 훅 이전). Brick의 A53은 A55보다 느리므로 **66 ms 예산을 넘길
  가능성이 큼** → 체감 10 fps 안팎. 대책은 아래.
- 사운드(SDL2 오디오 백엔드는 업스트림이 "다음 단계"로 적어둔 상태), 세이브 왕복.
- 한국어 텍스트: `SPIKE_LANG=ko-KR`가 `memorytext.dat`(접미사 없음)로 이어지는지. 안 되면 `en-US`.

## 5. 성능 대책 (실기 측정 후 순서대로)

1. `INOTIA_DEBUG=1`로 프레임당 명령 수와 인터프리터 ms 확인 (`inotia3.log`).
2. 엔진 빌드 플래그 `-O3 -flto`, `cpu.c` 핫루프의 per-instruction 체크 호이스팅
   (업스트림 PERFORMANCE.md "Tier 1"). `scripts/build.sh`의 `-O2`를 바꾸면 됨.
3. 가장 뜨거운 가드 함수(`GRP_FillRect`, `GRP_FilterAlpha` 등 소프트웨어 블리터, LZMA/
   inflate/PNG)를 `patches_replace_with_import`로 호스트 C로 대체.
4. 그래도 모자라면 Thumb-1 → aarch64 정적 변환(AOT). 함수 경계가 모두 알려져 있어
   (5,962 심볼) 구현 가능하지만 수 주 단위 작업.

## 6. 라이선스·법적 정리

- 게임 데이터(APK, `libgame.so`, 에셋)는 Com2uS 저작물. 레포·패키지에 포함하지 않음.
- Inotia3-vita: MIT. Inotia3-wasm: 저장소에 LICENSE 파일이 없음(REUSE.md에 "MIT 호환" 의도만
  적혀 있음). 그래서 소스를 벤더링하지 않고 빌드 시 고정 커밋을 받아 오버레이를
  적용합니다. 배포용 바이너리를 공개하려면 저자에게 라이선스 확인을 요청하세요.
- gl4es MIT, SDL2 zlib, 나눔고딕 OFL 1.1(재배포 가능, `FONT-LICENSE.txt` 동봉).

## 7. 첫 실기 테스트 체크리스트

1. `inotia_tool.py inspect your.apk` → `route=interpreter`, `renderer natives 10/10`, `CPU=v5TE`, `Thumb-1` 확인.
2. `prepare`로 두 파일 배치, SD 카드에 복사, Ports 메뉴에서 실행.
3. `inotia3.log` 확인 순서: `sdl: video driver` → `gl:` 컨텍스트 → `loaded libgame.so` →
   `patches:` → `mods:` → `pad:` → 엔진 로그(`GAMELOADER_Install`, 타이머 등록) → 프레임 통계.
4. 화면이 돌아가 있으면 `env.txt`에 `SPIKE_ROTATE=0/90/270`.
5. 글자가 비면 `font.ttf` 존재 확인, `SPIKE_LANG` 변경.
6. 느리면 5절.
