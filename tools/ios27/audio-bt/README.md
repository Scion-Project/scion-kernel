# iOS 27 on the Galaxy S9: audio and Bluetooth, the iOS side

The kernel side is in the fork (Scion-Project/xnu, branch `wip/ios27-s9-audio-bt`): ABOX + MAX98512 (`abox=N`,
`max98512=N`, `abox_rmem=1`) and the BCM4361's Bluetooth (`bt=N`). This directory is what iOS 27's root needs on top,
as one idempotent root step for ios27-fullroot's build (`tools/steps.d/`), OFF unless `AUDIO_BT=1`.

    tools/ios27/audio-bt/build.sh                       # -> build/ios27-audio-bt/files (needs Xcode + iPhoneOS SDK)
    AUDIO_BT=1 OS_MNT=<IPSW OS image mounted> HCD=<the phone's own bcm4361B2_semco.hcd> \
        tools/ios27/audio-bt/60-audio-bt.sh <mounted iOS 27 root>

What the step does:
- `/System/Library/Audio` from the user's own IPSW if the root lacks it (stage-ios-root.sh leaves it out; it has the
  HAL plug-ins, among them Apple's `BTAudioHALPlugin.driver` for A2DP/HFP, the tunings and the UI sounds).
- `HakiosAudio.driver` (Scion's AudioServerPlugIn, `userland/src/audio_plugin.c`, unchanged source, built for arm64e) in
  `/System/Library/Audio/Plug-Ins/HAL/`: the "Speaker" device writing to `/dev/hakios-audio` (kernel `abox=8`).
- The jobs back into `launchd.plist`: `com.apple.audiomxd` (iOS 27's audio server, which hosts the HAL),
  `com.apple.bluetoothd` with `BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,4000000` (the kernel's UART control at
  `bt=7`), and `com.apple.BlueTool`; bluetoothd and BlueTool start through `/usr/libexec/hakios-inject` with their
  library (launchd drops DYLD_ variables and the 608-byte command line has no room for more `sweetinject`).
- BlueTool's scripts for the S9 in `/etc/bluetool/` (`N104AP.*.script`: the iOS 27 root's HW model), the phone's own
  Broadcom patch in `/private/var/hakios/bt/` (read by the kernel at `bt=6`; never in git: it is the phone's vendor file).
- `AUDIO_BT_LOG=1` (tests): each of the three jobs' stderr into `/private/var/local/oslog/<name>.err`, with
  `OS_ACTIVITY_DT_MODE=YES` so their os_log lines go there too (logd is off on this root).

Status (9 November 2026): the kernel's speaker tone played on phone 1 (heard). This step has not run on a phone yet:
whether iOS 27's audiomxd runs on this kernel (on iOS 26 it looped on a SIGTRAP in caulk), loads the plug-in and builds
a speaker route from it under the N104 (iPhone 11) routing tables is the next test.
