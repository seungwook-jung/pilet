# 다른 경로들: 이노티아 4, 1·2, 그리고 인터프리터가 아닌 방식

## 이노티아 4 (Assassin of Berkel)

- Google Play에 2025년 3월까지 업데이트(1.3.7, 1.4.0)가 올라온 F2P 게임. Play 정책상
  2019년 8월 이후 업데이트는 arm64 라이브러리를 포함해야 하므로 **arm64-v8a + armeabi-v7a**
  구성일 가능성이 높습니다(미확인; APK를 `inotia_tool.py inspect`에 넣으면 즉시 판정).
- 두 가지 경로:
  1. **armeabi-v7a → 인터프리터 확장**: Inotia3-wasm 디코더는 ARMv5TE/Thumb-1에 이노티아 3이
     쓰는 일부 Thumb-2만 구현. ARMv7 빌드는 Thumb-2 전체, IT 블록, VFPv3, (있다면) NEON이
     필요. 디코더 작업 + 호스트 표면(이노티아 4는 Hub/IAP/광고 JNI가 훨씬 많음) 재작성.
  2. **arm64-v8a → 네이티브 로더(gmloader-next 방식)**: 32비트 문제가 사라지므로
     `libgame.so`를 aarch64 프로세스에 직접 매핑하고 bionic/JNI/GLES를 쉬밍. 인터프리터
     오버헤드 0. 단, 이노티아 4 엔진의 JNI 표면(수백 개)을 FalsoJNI 식으로 다시 구현해야
     하고, gmloader-next는 GameMaker 전용 프런트엔드라 **새 프런트엔드를 작성**해야 함.
     이 레포의 `inotia_tool.py`가 출력하는 임포트/JNI 문자열 목록이 그 작업의 체크리스트.
- 온라인 요소(로그인, 상점, 광고)는 Vita 포트가 이노티아 3에서 했듯 무력화 패치 필요.
  서버가 내려가 있으면 오프라인 우회가 필수.

## 이노티아 1·2

- 1은 WIPI 피처폰 게임이 원본. iOS판("Inotia: Legend of Feanor")만 존재. Linux에서 iOS
  바이너리를 돌리려면 Mach-O 로더 + UIKit/OpenGL ES 쉬밍이 필요(Vita에선 MetalSyntax의
  `Inotia2-vita`가 iOS→Vita로 함). Brick용으로는 사실상 새 프로젝트.
- 2 Android판은 2009년작이라 Java(Dalvik) 비중이 클 수 있음. APK가 있으면
  `inspect`로 네이티브 라이브러리 유무부터 확인.

## 인터프리터가 아닌 방식들

| 방식 | 평가 |
|---|---|
| qemu-user(32비트 프로세스 전체 에뮬레이션) + armhf 루트FS + so-loader | 32비트 GL이 없어 소프트웨어 GLES 필요, 패키지 수백 MB, 성능 열세. 비추천 |
| Thumb → aarch64 정적 변환(AOT) | 성능 최고. 함수 경계가 모두 알려져 있어 가능하지만 수 주 단위. 인터프리터로 15 fps가 안 나올 때의 다음 단계 |
| 엔진 재구현(에셋만 재사용) | 수개월. 모딩 자유도는 최고. 포맷 역분석(`docs/modding.md` 3단계)이 선행 |
| Android 컨테이너/호환 레이어 | Brick(1 GB RAM, PowerVR 폐쇄 드라이버)에선 비현실적 |

## WebAssembly 경로 (참고)

Inotia3-wasm은 브라우저에서도 돕니다. 브라우저가 있는 기기(스마트폰, PC)라면 빌드 없이
<https://suchangv.github.io/inotia3-wasm/>에 APK를 올려 바로 확인할 수 있어, 모딩 실험
(mods.txt 대신 `?framems=` 등의 쿼리)과 세이브 테스트에는 가장 빠른 환경입니다.
