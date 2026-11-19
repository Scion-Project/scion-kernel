# 이 포크: 갤럭시 노트9 (`crownlte`, SM-N960N) 실기기 구동

> English: [README.md](README.md) (맨 위 "This fork" 부분). 원본 Scion 설명은 README.md에서 이 포크 설명 아래에 그대로 있어요.

원본 README에는 노트9가 "작성만 됐고 실행된 적 없음"으로 적혀 있어요. 이 포크의 `note9` 브랜치에서는 **실제로 동작해요.**
- 한국판 갤럭시 노트9 SM-N960N(Exynos 9810)에서 scion-installer의 TWRP 설치 파일로 iOS 16.5가 부팅되고, 매일 테스트에 쓰고 있어요.
- 모든 변경은 이 폰 한 대에서 한 회차씩 만들고 확인했어요. 근거는 커널 로그(TWRP에서 받은 `/proc/last_kmsg`)예요.
- 이 저장소에는 Apple 파일, 키, 기기 식별 정보가 없어요. iOS 시스템 이미지는 원본과 같이 각자 준비한 IPSW로 빌드해요.
- 빌드로 만들어지는 설치 zip은 **본인 전용이고 절대 공유하면 안 돼요.**

## 노트9에서 동작하는 것

| 항목 | 상태 | 파일 |
|---|---|---|
| 부팅, 8코어, GPU(Mali), Wi-Fi, 배터리 | 동작 | 원본 S9 드라이버 + `crownlte` 부트 인자 |
| 디스플레이(S6E3HA8 "crown") | 동작, 밝기 슬라이더 | `drivers/gpu/hakios.s6e3ha8.c` |
| **측면 버튼으로 화면 끄기**(완전히 꺼짐)와 켜기, 줄·얼룩 없음 | 동작 | `hakios.s6e3ha8.c` (아래 설명) |
| **자동 잠금**(설정 > 디스플레이) | 동작 | `userland/src/inject_sb.c` |
| 터치, S펜이 가까울 때 손바닥 무시 | 동작 | `hakios.s6sy761.c` |
| **S펜**(Wacom W9018), 필압 | 동작 | `drivers/input/hakios.w9018.c`, `inject.c`(손가락 11번 슬롯) |
| 버튼: 측면, 볼륨, 빅스비(홈 버튼 역할) | 동작 | `hakios.exynos_keys.c` |
| **10 W 충전**(전에는 1~2 W) | 동작 | `drivers/power/hakios.max77705.c` |
| 스피커(MAX98512), 마이크(CS47L93) | 동작 | `hakios.max98512.c`, `audio_plugin.c` |
| 블루투스: 설정·제어 센터에서 켜고 끄기, 페어링, A2DP | 동작 | `inject_bluetoothd.c`, `inject_bluetool.c`, `inject_sb.c` |
| 전면 카메라: 미리보기, 사진 | 동작 | `hakios.exynos_camera.c`, `camera_device.c` |
| **동영상 촬영**(하드웨어 H.264 인코더, MFC) | 저장·재생됨, 녹음된 **소리가 끊김** | `hakios.exynos_mfc.c`, `mfc_encoder.c`, `inject_media.c` |
| 하드웨어 동영상 디코딩(MFC) | 동작 | `hakios.exynos_mfc.c` |
| USERDATA(HFS+)에 `/private/var` 유지, Sileo | 동작 | `hakios.ufs.c`, `hakios.ufshc.c` |
| 센서(센서 허브: 가속도, 자이로, 조도, 근접, 나침반), 화면 회전 | 동작 | `inject.c` |

**안 되거나 시도하지 않은 것**
- 셀룰러: Shannon 모뎀이고 CommCenter가 없어요.
- 후면 카메라, NFC, Face ID
- Apple ID 로그인: Apple의 기기 확인에서 거부돼요. 이 저장소는 기기 정보를 위조하지 않아요.
- FairPlay 보호 콘텐츠: 의도적으로 구현하지 않았어요.

## 무엇을 왜 고쳤나

### 화면 끄기·켜기 (`drivers/gpu/hakios.s6e3ha8.c`)
- **끄기:** 디스플레이 끄기(DCS 28)만으로는 패널이 희미하게 켜져 있었어요. 지금은 28에 sleep-in까지 보내요. 그동안 DECON의 프레임 전송을 멈춰서(`hakios_scanout_hold`, `drivers/gpu/hakios.exynos_dpu.c`) 자는 패널에 아무것도 가지 않게 해요.
- **켜기:** 이 패널은 sleep-in 상태에서 그냥 깨우면 가로 검은 줄과 얼룩이 생겨요. Samsung 커널(그래서 TWRP도)은 화면을 끌 때 항상 전원을 내리고, 켤 때 전원을 올려 처음부터 다시 초기화해요. 깨우기를 Samsung의 `_dsim_enable` / `panel_drv.c`와 같은 순서로 바꿨어요.
  1. 패널이 자는 동안 리셋 핀(`gpa3-4`)을 low로 잡아 둬요.
  2. 깨울 때 리셋을 풀기 **전에** DSI 클럭 레인을 LP-11(정지 상태)로 내려요.
  3. 다시 HS로 올린 뒤 Samsung 초기화 전체를 보내요. DSC + PPS, sleep-out, scaler/CASET/PASET, SET_AREA, TE, 부트로더가 설정한 FFC·ISC·에지 디밍·밝기 레지스터예요.
  4. `disp_det`(`gpg2-2`)와 DCS 0A로 패널이 켜졌는지 확인해요. 안 켜졌으면 최대 3번 다시 리셋하고, 끝내 안 되면 로그를 남긴 채 로더(TWRP)로 돌아가요.
- **레지스터 읽기 버그:** 이 패널은 레지스터를 읽으면 에러 보고 패킷을 먼저 보내고, 데이터는 잠시 뒤에 보내요. 읽기가 너무 일찍 포기해서 남은 데이터를 다음 읽기가 가져갔고, 값이 한 칸씩 밀렸어요. 밀린 값을 리셋된 패널에 써 넣으면 패널은 "켜짐"인데 화면은 까만 상태가 됐어요. 애플 로고 뒤 검은 화면의 원인이에요.
  - 지금은 데이터가 올 때까지 기다려요.
  - 부트로더 레지스터는 프레임 전송이 없을 때 두 번 읽어서 같을 때만 보관해요.
  - 써 넣은 값은 다시 읽어 확인해요.
- **중복 끄기:** 이미 자는 중에 끄기가 또 오면(부팅 중 backboardd가 다시 시작될 때 생겨요) 아무것도 보내지 않아요. 전에는 이때 상태가 꼬여서 패널을 다시 깨우지 못했어요.
- **잔상 문제:** `MiniIOMobileFramebuffer.cpp`에서는 패널이 자는 동안 TE가 없어 화면 전환이 안 되는 것을 더 이상 고장으로 판단하지 않아요. 전에는 이걸 고장으로 보고 스케일러 경로를 영구히 포기해서, 화면을 끌 때마다 잔상과 끊김이 생겼어요.

### 자동 잠금 (`userland/src/inject_sb.c`)
셀룰러가 없어서 CommCenter가 없으면 SpringBoard의 `SBTelephonyManager`가 1초마다 재연결을 시도해요. 그때마다 idle 타이머가 리셋돼서 화면이 잠기지 않았어요.
- 호출 순서: `_handleTelephonyDaemonRestart` → `handleCallControlFailure:` → `updateCalls` → `resetIdleTimer`
- 수정: 그 재시도 안에서 나오는 리셋만 걸러요. 재시도 자체와 다른 리셋(터치, 버튼)은 그대로예요.
- SpringBoard idle 타이머를 기록만 하는 훅은 남겨 뒀어요(`hakios: idle:` 로그).

### 충전 (`drivers/power/hakios.max77705.c`)
충전 칩을 읽기만 하고 설정하지 않아서 iOS에서 1~2 W로만 충전됐어요.
- 지금은 CHGPROT를 풀고 CNFG_09(입력 제한)와 CNFG_02(충전 전류)를 2000 mA로 설정해요.
- 워치독을 끄고, 0~45 °C 밖에서는 500 mA로 제한해요.
- `max77705_chg=0`으로 끌 수 있어요.

### 블루투스
- `inject_bluetoothd.c`: H4 전송 콜백이 비어 있어서 블루투스를 끌 때 bluetoothd가 크래시했어요. 아무것도 안 하는 함수로 채웠어요.
- `userland/bluetool/D22AP.deepsleep.script`: 이 파일이 없어서 끌 때마다 타임아웃까지 기다렸어요.
- `inject_bluetool.c`: 느린 MobileGestalt 응답을 캐시해서 켜기가 약 0.7초로 빨라졌어요.
- `inject_sb.c`: 제어 센터 블루투스 버튼이 연결 안 된 자기만의 BluetoothManager를 읽어서 회색이었어요. 공용 인스턴스를 넣어 줘요.

### 동영상 촬영 (`mfc_encoder.c`, `inject_media.c`, `hakios.exynos_mfc.c`)
- 소프트웨어 HEVC 인코더로는 속도가 안 나왔어요. VideoToolbox를 `__interpose`나 GOT로 가로채는 방법은 공유 캐시 안의 호출에 닿지 않아요. 그래서 MFC를 `avc1`/`hvc1`용 진짜 VideoToolbox 인코더 플러그인으로 등록했어요.
- 촬영을 멈춘 뒤 mediaserverd의 캡처 그래프가 크래시하지 않게 막았어요. 1초보다 긴 오디오 버퍼(오디오 노드의 엉터리 타임스탬프)는 버려요. 이제 영상 파일이 끝까지 저장되고 재생돼요.
- 남은 문제: 녹음된 소리가 틱틱 끊겨요. 작업 중이에요.

### 기타
- **S펜:** `inject.c`가 손가락을 10개까지만 처리했는데 펜은 11번째예요. 펜이 가까울 때 펜 끝 주변의 유령 터치는 무시해요.
- **저장장치:** UFS 슬롯 버퍼를 키우고, 대기를 바쁜 대기 대신 잠자기로 바꾸고, 30초마다 쓰기 통계를 남겨요(`hakios.ufshc.c`).
- **비상 탈출:** **빅스비 + 볼륨 다운을 3초** 누르면 로그를 남기고 로더로 돌아가요. 화면이 까맣고 터치도 안 될 때 쓰는 방법이에요. 볼륨 다운 + 전원은 강제 재부팅이라 로그가 사라져요.
- **진단:** `kern.hakios.ustacks`(`hakios.input.c`)에 프로세스 이름을 쓰면 그 프로세스의 사용자 스택을 로그에 남겨요.

## 빌드

원본(scion-installer)과 같은 방법이에요. 커널은 이 포크의 `note9` 브랜치를 쓰고, 노트9용 부트 인자를 줘요.

```sh
./build-installer.sh --ipsw <iPhone10,3/10,6 16.5 IPSW> --device all --kernel-ref <이 브랜치의 커밋> \
  -- -N -t 0 -a "crownlte=exynos_keys=1 s6sy761=1 mali=9 fb_zc=1 fb_dpp=1 cpus=8 m3_prep=3 isa_floor=1 pan=0 \
max77705=1 max77705_haptic=1 wifi=1 wifi_pcie=6 brcmfmac=8 brcmfmac_poll=1 brcmfmac_tcm_idx=1 mali_gov=1 cpufreq_gov=1 \
tmu_stop=90 ufs=3 panel=1 panel_gamma=0 backlight=2 abox=8 max98512=3 bt=7 cam=5 cam_s9=7 mfc=5 cs47l93=6 abox_mic=1 \
shub=6 ufs_write=1 var_hfs=ufs_USERDATA_data"
```

- 환경 변수는 `SYSTEM_FULL=1 FRAGMENTS="panel9 mfc9 sense9"`이에요.
- `var_hfs`는 `/private/var`를 USERDATA에 두는 옵션이에요. USERDATA를 한 번 HFS+로 포맷해야 하고, 그러면 **안드로이드 데이터가 지워져요**(scion-installer의 `--format-userdata` 참고).
- 먼저 TWRP에서 Boot, System, EFS를 백업하세요.
- 로더의 부트 인자 줄에는 길이 제한이 있어서, 넘으면 소리 없이 잘려요. 인자를 바꾼 뒤에는 로그의 "Boot args" 줄을 확인하세요.

**테스트 방법**
- 매 실행 후 TWRP에서 `/proc/last_kmsg`를 받으세요.
- 실행을 끝낼 때는 화면에 다섯 손가락을 1.5초 올려 두거나, 빅스비 + 볼륨 다운을 3초 누르세요.
- 강제 재부팅을 하면 로그가 사라져요.
