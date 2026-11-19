# 이 포크: 갤럭시 노트9 (`crownlte`, SM-N960N) 실기기 구동

> English: [README.md](README.md) (맨 위 "This fork" 부분). 원본 Scion 설명은 README.md에서 이 포크 설명 아래에 그대로 있어요.

원본 README에는 노트9가 "작성만 됐고 실행된 적 없음"으로 적혀 있어요. 이 포크의 `note9` 브랜치에서는 **실제로 동작해요.**
- 한국판 갤럭시 노트9 SM-N960N(Exynos 9810)에서 scion-installer의 TWRP 설치 파일로 iOS 16.5가 부팅되고, 매일 테스트에 쓰고 있어요.
- 모든 변경은 이 폰 한 대에서 한 회차씩 만들고 확인했어요. 근거는 커널 로그(TWRP에서 받은 `/proc/last_kmsg`)예요.
- 이 저장소에는 Apple 파일, 키, 기기 식별 정보가 없어요. iOS 시스템 이미지는 원본과 같이 각자 준비한 IPSW로 빌드해요.
- 빌드로 만들어지는 설치 zip은 **본인 전용이고 절대 공유하면 안 돼요.**

**2026년 11월 19일: 원 프로젝트의 최신 `main` 위로 옮겼어요.**
- 원 프로젝트가 이력을 새로 써서, 노트9 작업(118회차까지의 커밋 123개)을 새 구조에 파일 단위로 다시 얹었어요.
- 그 결과 원 프로젝트의 Metal(Vulkan 변환), S9+ 카메라 드라이버(노트9가 그대로 써요) 등이 함께 들어왔어요.
- 옮기기 전 브랜치는 `note9-old`로 남겨 뒀어요.

## 노트9에서 동작하는 것

| 항목 | 상태 | 파일 |
|---|---|---|
| 부팅, 8코어, GPU(Mali), Wi-Fi, 배터리 | 동작 | 원본 S9 드라이버 + `crownlte` 부트 인자 |
| 디스플레이(S6E3HA8 "crown") | 동작, 밝기 슬라이더 | `drivers/display/panel/hakios.s6e3ha8.c` |
| **측면 버튼으로 화면 끄기**(완전히 꺼짐)와 켜기, 줄·얼룩 없음 | 동작 | `hakios.s6e3ha8.c` (아래 설명) |
| **자동 잠금** | 동작 | `userland/src/inject_sb.c` |
| 터치, S펜이 가까울 때 손바닥 무시 | 동작 | `drivers/input/touch/hakios.s6sy761.c` |
| **S펜**(Wacom W9018), 필압 | 동작 | `drivers/input/touch/hakios.w9018.c`, `inject.c` |
| 버튼: 측면, 볼륨, 빅스비(홈 버튼 역할) | 동작 | `hakios.exynos_keys.c` |
| **10 W 충전**(전에는 1~2 W) | 동작 | `drivers/power/max77705/hakios.max77705.c` |
| 스피커(MAX98512), 마이크(CS47L93) | 동작 | `hakios.max98512.c`, `audio_plugin.c` |
| 블루투스: 설정·제어 센터에서 켜고 끄기, 페어링, A2DP | 동작 | `inject_bluetoothd.c`, `inject_bluetool.c`, `inject_sb.c` |
| **카메라: 후면 광각(S5K2L3), 후면 망원(S5K3M3), 전면(S5K3H1)** | 동작, 자동 초점(소프트웨어 방식)이 느림 | `drivers/camera/exynos/hakios.exynos_camera_star2.c`(S9+ 드라이버), `camera_device.c` |
| **동영상 촬영**(하드웨어 H.264 인코더, MFC) | 저장·재생됨, 녹음된 **소리가 끊김** | `hakios.exynos_mfc.c`, `mfc_encoder.c`, `inject_media.c` |
| 하드웨어 동영상 디코딩(MFC, H.264) | 동작 | `hakios.exynos_mfc.c` |
| **Metal**(원 프로젝트의 Metal → Vulkan 변환, Mali-G72의 PanVK 위) | 앱에서 동작, 화면 합성은 더 빠른 OpenGL ES 유지 | `userland/src/metalvk/`, `METAL=vk` |
| **App Store 앱**(본인 기기에서 복호화, AppSync로 설치): 전체 화면, 키체인, 로그인 창 | 동작: 유튜브(구글 로그인 포함), 인스타그램(Metal 변환 때문에 다소 느림) | 아래 설명 |
| USERDATA(HFS+)에 `/private/var` 유지, Sileo | 동작 | `drivers/storage/ufs/` |
| 센서(가속도, 자이로, 조도, 근접, 나침반), 화면 회전 | 동작 | `inject.c` |

**안 되거나 시도하지 않은 것**
- 셀룰러: 원 프로젝트의 Shannon 모뎀 드라이버는 들어와 있지만, 노트9에서는 아직 시도하지 않았어요.
- NFC, Face ID
- Apple ID 로그인: Apple의 기기 확인에서 거부돼요. 이 포크는 기기 정보를 위조하지 않아요.
- FairPlay 보호 콘텐츠: 의도적으로 구현하지 않았어요.

## 무엇을 왜 고쳤나

### 화면 끄기·켜기 (`drivers/display/panel/hakios.s6e3ha8.c`)
- **끄기:** DCS 28만으로는 패널이 희미하게 켜져 있었어요. 지금은 28에 sleep-in까지 보내고, 그동안 DECON 프레임 전송을 멈춰요(`hakios_scanout_hold`).
- **켜기:** 이 패널은 sleep-in에서 그냥 깨우면 가로 검은 줄과 얼룩이 생겨요. Samsung 커널(TWRP 포함)은 끌 때 전원을 내리고 켤 때 처음부터 초기화해요. 그 순서를 그대로 따랐어요.
  1. 자는 동안 리셋 핀(`gpa3-4`)을 low로 둬요.
  2. 깨울 때 리셋을 풀기 **전에** DSI 클럭 레인을 LP-11(정지 상태)로 내려요.
  3. HS로 올린 뒤 Samsung 초기화 전체를 보내요(DSC + PPS, sleep-out, scaler/CASET/PASET, SET_AREA, TE, 부트로더 레지스터).
  4. `disp_det`(`gpg2-2`)와 DCS 0A로 확인하고, 안 켜지면 최대 3번 다시 리셋해요. 끝내 안 되면 로그를 남기고 로더로 돌아가요.
- **레지스터 읽기 버그:** 패널이 에러 보고 패킷을 먼저 보내고 데이터를 늦게 보내서, 읽은 값이 한 칸씩 밀렸어요. 밀린 값을 리셋된 패널에 써 넣으면 애플 로고 뒤 검은 화면이 됐어요. 지금은 데이터를 기다리고, 두 번 읽어 비교하고, 써 넣은 뒤 다시 읽어 확인해요.
- **중복 끄기:** 이미 자는 중에 끄기가 또 오면 아무것도 보내지 않아요.
- 원 프로젝트의 S9용 `panel_sleep=1`(단순 sleep-in/out)은 노트9에서 쓰지 않아요. 같은 진입점(`hakios_s6e3ha8_power()`)이 위 순서로 연결돼요.
- XNU 포크의 `MiniIOMobileFramebuffer.cpp`: 패널이 자는 동안 TE가 없어 화면 전환이 안 되는 것을 고장으로 보지 않아요(예전 잔상·끊김의 원인).

### 자동 잠금 (`userland/src/inject_sb.c`)
셀룰러가 없어서 SpringBoard의 `SBTelephonyManager`가 1초마다 재연결을 시도했고, 그때마다 idle 타이머가 리셋돼 화면이 잠기지 않았어요. 그 재시도 안의 리셋만 걸러요.

### App Store 앱
- **레터박스**(`inject_sb.c`): 이 화면(1125×2313)을 아는 앱이 없어서, SpringBoard가 모든 서드파티 앱을 16:9 "클래식 모드"로 띄웠어요. 클래식 모드를 끄고 전체 화면으로 띄워요.
- **키체인**(XNU 포크 `mach_loader.c`, `kern_cs.c`): 복호화한 앱은 첫 페이지 해시가 서명과 안 맞아서, 커널이 코드 서명 플래그 0으로 실행했고 키체인 권한을 넘기지 않았어요(-34018). 서명을 강제하지 않는 이 포트에서는 "유효" 표시를 지우지 않게 했어요.
- **로그인 창**(`inject_sb.c`): FrontBoard가 "신뢰할 수 없는 앱"이라며 SafariViewService를 열어 주지 않았어요. 이 포트의 LaunchServices 기록에 시스템 앱 권한 정보가 없어서예요. Info.plist에 뷰 서비스 이름(`com.apple.uikit.viewservice.<id>`)을 가진 시스템 앱을 뷰 서비스로 인정해요. 이후 유튜브 구글 로그인이 돼요.
- **새 SDK 앱용 Metal 응답**(`HakiosMetalVK.mm`): `recommendedMaxWorkingSetSize` 등 4개 질문에 답해요. 인스타그램이 첫 번째 질문에서 죽었어요.
- `libhakios.app.dylib`(`inject_app.c`)를 인스타와 유튜브에 주입해, 앱이 죽는 이유를 남겨요. 시그널 위치와 레지스터, NSException, 키체인 실패, 코드 서명 상태예요.

### 카메라
S9+와 노트9의 Samsung 디바이스 트리(rev 26)는 카메라 노드가 phandle 번호 말고는 모두 같아요. 핀(MCLK `gpc1-4/5`, 리셋 `gpg2-5/6/7`, 플래시 `gpg3-5`), I2C, 전원이 같아서 노트9 커널이 S9+ 드라이버를 그대로 써요. 후면과 망원은 기본으로 켜져 있어요(`cam_s9_rear=0`, `cam_s9_tele=0`으로 끔).

### Metal과 화면 합성
원 프로젝트의 Metal(Vulkan 변환)이 노트9에서 동작해요. MetalTri 테스트가 그려지고 인스타도 Metal로 그려요. 화면 합성까지 Metal로 하면 UI는 더 정확하지만 이 폰에서는 전체가 느려져서, 빌드는 화면 합성을 OpenGL ES로 둬요(`METAL_WS=0`, `HakiosMetal.clients`에 `-backboardd`). 폰의 `/private/var/db/HakiosMetal.clients`에 `backboardd` 한 줄을 넣으면 재빌드 없이 Metal로 돌릴 수 있어요.

### 충전, 블루투스, 동영상, 기타
- **충전:** 충전 칩을 읽기만 해서 1~2 W였어요. CNFG_09와 CNFG_02를 2000 mA로 설정하고, 워치독을 끄고, 0~45 °C 밖에서는 500 mA로 제한해요(`max77705_chg=0`으로 끔). 원 프로젝트의 진단과 워치독 처리도 함께 들어 있어요.
- **블루투스:** 끌 때 크래시하던 H4 콜백을 채우고, `D22AP.deepsleep.script`를 추가하고, MobileGestalt 응답을 캐시하고, 제어 센터 버튼에 공용 BluetoothManager를 넣었어요.
- **동영상 촬영:** MFC를 `avc1`/`hvc1`용 VideoToolbox 인코더 플러그인으로 등록하고, 1초보다 긴 오디오 버퍼를 버려 파일이 끝까지 저장되게 했어요. 캡처 정지 처리는 원 프로젝트 쪽을 써요.
- **S펜:** `inject.c`가 손가락 10개까지만 처리했는데 펜은 11번째예요.
- **비상 탈출:** 빅스비 + 볼륨 다운을 3초 누르면 로그를 남기고 로더로 돌아가요. 화면이 까맣고 터치도 안 될 때 써요.
- **맥 빌드용 원 프로젝트 수정:** `hakios.drm.c`의 함수 선언 순서, `userland/build.sh`의 `uuidgen`

## 빌드

- 커널 쪽 변경은 XNU 포크 [cooblg/xnu](https://github.com/cooblg/xnu/tree/note9)의 `note9` 브랜치에 있어요. `profiles/ios-16.5.conf`가 이걸 가리켜요.
- 원 프로젝트의 [scion-installer](https://github.com/Scion-Project/scion-installer)를 쓰되, `METAL_USERLAND`를 정하는 `case`에 `crownlte`를 추가해 주세요. 거기서는 Metal을 S9+용으로만 빌드해요.

```sh
METAL_WS=0 SYSTEM_FULL=1 FRAGMENTS="panel9 mfc9 sense9" ./build-installer.sh --ipsw <iPhone10,3/10,6 16.5 IPSW> \
  --device crownlte --kernel-ref <이 브랜치의 커밋> -- -N -t 0 -a "crownlte=exynos_keys=1 s6sy761=1 mali=9 fb_zc=1 \
fb_dpp=1 cpus=8 m3_prep=3 isa_floor=1 pan=0 max77705=1 max77705_haptic=1 wifi=1 wifi_pcie=6 brcmfmac=8 brcmfmac_poll=1 \
brcmfmac_tcm_idx=1 mali_gov=1 cpufreq_gov=1 tmu_stop=90 ufs=3 panel=1 panel_gamma=0 backlight=2 abox=8 max98512=3 bt=7 cam=5 \
cam_s9=7 mfc=5 cs47l93=6 abox_mic=1 shub=6 ufs_write=1 var_hfs=ufs_USERDATA_data metal=2"
```

- 맥에서는 Metal 빌드에 Xcode(여기서는 26)와 Metal 도구가 필요해요(`xcodebuild -downloadComponent MetalToolchain`). Homebrew의 llvm도 `PATH`에 있어야 해요.
- `var_hfs`는 `/private/var`를 USERDATA에 두는 옵션이에요. USERDATA를 한 번 HFS+로 포맷해야 하고, 그러면 **안드로이드 데이터가 지워져요**(scion-installer의 `--format-userdata` 참고). 먼저 TWRP에서 Boot, System, EFS를 백업하세요.
- 로더의 부트 인자 줄은 약 620자에서 소리 없이 잘려요. 인자를 바꾼 뒤에는 로그의 "Boot args" 줄을 확인하세요.

**테스트 방법**
- 매 실행 후 TWRP에서 `/proc/last_kmsg`를 받으세요.
- 실행을 끝낼 때는 다섯 손가락을 1.5초 올려 두거나, 빅스비 + 볼륨 다운을 3초 누르세요.
- 강제 재부팅을 하면 로그가 사라져요.
