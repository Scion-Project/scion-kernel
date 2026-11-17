# iOS 27 on the Galaxy S9: audio and Bluetooth, the iOS side

The kernel side is in the fork (Scion-Project/xnu, branch `wip/ios27-s9-audio-bt`): ABOX + MAX98512 (`abox=N`,
`max98512=N`, `abox_rmem=1`) and the BCM4361's Bluetooth (`bt=N`). This directory is what iOS 27's root needs on top,
as one idempotent root step. The root build runs both of the commands below by default since 17 November
(`tools/ios27/fullroot/steps.d/92-audio-bt.sh`: both modules' patches from scion-starlte's `firmware/bt/`, fetched and
checked by `tools/ios27/fullroot/firmware.sh`, and a patch missing or wrong stops the build; until 17 November it skipped
without `HCD`, and the root then had neither audio nor Bluetooth; `ENABLE_AUDIO_BT=0` leaves it out; docs/ios27/INSTALL-S9.md,
"The S9's firmware"). By hand, `60-audio-bt.sh` does nothing unless
`AUDIO_BT=1`:

    tools/ios27/audio-bt/build.sh                       # -> build/ios27-audio-bt/files (needs Xcode + iPhoneOS SDK)
    AUDIO_BT=1 OS_MNT=<IPSW OS image mounted> HCD=devices/starlte/firmware/bt \
        tools/ios27/audio-bt/60-audio-bt.sh <mounted iOS 27 root>
    tools/ios27/audio-bt/60-audio-bt.sh <mounted iOS 27 root> undo     # all of it off again, the jobs back where they were

`HCD` is one patch (`bcm4361B2_semco.hcd` or `_murata`) or a directory, of which every patch `HCD_SUMS` names is put on the
root. Each is checked against `devices/starlte/firmware/bt/SHA256SUMS` (`HCD_SUMS` for another list) when the checkout has
it. With both on the root, the kernel (`hakios.bcm4361_bt.c`, xnu-12377) waits up to `bt_otp_wait=` seconds (120) for the
Wi-Fi to read the module from the chip's OTP and takes that module's patch; `bt_module=` names it outright. Written on 17
November and not yet run on a phone. Where the three jobs were before the first run is kept in `/usr/local/share/scion/audio-bt-jobs.plist` for `undo`.

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

Status (9 November 2026, phone 1, silent runs at night):
- Kernel: `abox=7 max98512=3 abox_rmem=1` + console `abox tone 3` played through both speakers (heard by the owner).
- With this step and `abox=8 bt=7 cs47l93=6 abox_mic=1`: iOS 27's `audiomxd` runs (no crash loop on this kernel) and
  loads `HakiosAudio.driver`; VirtualAudio makes the Speaker (`pspk`), Hawking, Actuator and Codec ports from it and
  activates routes on them. `hal_probe` (as the root's `/private/var/herolte/autorun`) loads the plug-in, sees its four
  devices and runs 3 s of output and 3 s of microphone input through it. That run had no `/dev/hakios-audio` (a kernel
  bug, fixed in scion-kernel `fork/ios27-audio-bt`) and no codec power (PMIC calls stubbed; fixed in the fork).
- Bluetooth: kernel `bt=1..7` all pass on iOS 27 (the phone's own SEMCO patch from `/private/var/hakios/bt/`, 4 Mbaud);
  BlueTool runs `N104AP.boot/init.script` (result 0); bluetoothd reaches STACK_READY, and Apple's BTAudioHALPlugin in
  audiomxd connects to it; bluetoothd then restarts (reason 701) when the chip rejects one of Apple's LE-scan vendor
  commands: kernel boot-arg `bt_vsc_ok=` (scion-kernel `fork/ios27-audio-bt`) answers those.
- 17 November, phone 1 (the full root of that day): bluetoothd restarts every 30 s (exit 1, "Bluetooth error - restarting")
  without `bt_vsc_ok`, and aborts every 30 s with `bt_vsc_ok=all`. The abort is bluetoothd's own assertion: the reply to
  0xFDDA (Read Proprietary Local Features) is read as a status and a 32-bit mask, and `bt_vsc_ok` gave a status of 00 with
  no mask. Rejected on this chip, in that order: 0xFDFD (HDR Control), 0xFFF2 (a vendor command of sub-commands), 0xFDDA.
  `bt_vsc=1` (drivers/bluetooth/bcm4361/hakios.bt_vsc.h) answers from a table instead, each answer a whole reply that is
  true of this chip (0xFDDA: no Apple features; 0xFDFD off: done), and passes the rest as the chip answers them; `bt_vsc=log`
  only reports the rejections. `bt_addr_efs=1` gives the chip the phone's own address from EFS (read only); without it
  every S9 on this kernel has Broadcom's default address.
- Later on 17 November (`bt_vsc=1`, phone 1): the abort is gone; bluetoothd goes through its start and the chip takes
  Apple's 0xFCE2 sub-commands, but bluetoothd takes the chip for one with Apple's scan core and LP core and sends their
  commands (0xFE37 scan parameters, 0xFE3B LP-core statistics). `bt_vsc=1` sends 0xFE37 and 0xFE29 as the standard LE
  scan commands (the chip took them and LE advertising reports came); bluetoothd still restarted. `BT_NO_SCAN_CORE=1`
  (60-audio-bt.sh, `bt-prefs.py`) sets bluetoothd's own overrides EnableScanCore=NO and DisableLowPowerCoreScan=YES so
  that it does not use them at all; not yet run on a phone.
