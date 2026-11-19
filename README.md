# This fork: the Galaxy Note9 (`crownlte`, SM-N960N) brought up

> 한국어: [README.ko.md](README.ko.md)

The original README (below) says the Note9 was "written, never run". On this fork's `note9` branch it **runs**: iOS 16.5 boots on a
Galaxy Note9 SM-N960N (Korean model, Exynos 9810) through scion-installer's TWRP installer, and is used day to day for testing.
Every change here was made and checked on that one phone, a round at a time, from the kernel's own log (`/proc/last_kmsg`
pulled in TWRP). Nothing here carries Apple's files, keys or any device's identity: the iOS system image is built, as upstream,
from an IPSW you supply yourself, and the installer zip that build makes is **yours only and never to be shared**.

## What works on the Note9

| Area | State | Where |
|---|---|---|
| Boot, 8 cores, GPU (Mali), Wi-Fi, battery | works | upstream S9 drivers, `crownlte` boot-args |
| Display (S6E3HA8 "crown") | works, brightness slider | `drivers/gpu/hakios.s6e3ha8.c` |
| **Screen off with the side button** (really dark) and back on, no lines or smudges | works | `hakios.s6e3ha8.c` (below) |
| **Auto-Lock** (Settings > Display) | works | `userland/src/inject_sb.c` |
| Touch screen, palm rejection while the S Pen is near | works | `hakios.s6sy761.c` |
| **S Pen** (Wacom W9018) with pressure | works | `drivers/input/hakios.w9018.c`, `inject.c` (finger slot 11) |
| Keys: side, volume, Bixby (as Home) | works | `hakios.exynos_keys.c` |
| **Charging at 10 W** (was 1-2 W) | works | `drivers/power/hakios.max77705.c` |
| Speakers (MAX98512), microphone (CS47L93) | works | `hakios.max98512.c`, `audio_plugin.c` |
| Bluetooth: on/off from Settings and Control Center, pairing, A2DP | works | `inject_bluetoothd.c`, `inject_bluetool.c`, `inject_sb.c` |
| Front camera: preview, photos | works | `hakios.exynos_camera.c`, `camera_device.c` |
| **Video recording** (hardware H.264 encoder, MFC) | saves and plays; recorded **audio stutters** | `hakios.exynos_mfc.c`, `mfc_encoder.c`, `inject_media.c` |
| Hardware video decoding (MFC) | works | `hakios.exynos_mfc.c` |
| Persistent `/private/var` on USERDATA (HFS+), Sileo | works | `hakios.ufs.c`, `hakios.ufshc.c` |
| Sensors (sensor hub: accelerometer, gyro, light, proximity, compass), rotation | works | `inject.c` |

**Does not work / not attempted:** cellular (Shannon modem; no CommCenter), rear cameras, NFC, Apple ID sign-in (Apple's
device check rejects this port; nothing here forges a device identity), FairPlay-protected content (not implemented, by choice),
Face ID.

## What was changed, and why

### Screen off and on (`drivers/gpu/hakios.s6e3ha8.c`)
- Display-off alone (DCS 28) left the panel faintly lit. Off is now 28 + sleep-in, with the DECON's frames held
  (`hakios_scanout_hold`, `drivers/gpu/hakios.exynos_dpu.c`) so nothing is sent to a sleeping panel.
- This panel cannot be woken from a bare sleep-in: it came back with black horizontal lines and smudges. Samsung's own kernel
  (and so TWRP) never does that: every blank is a power-down, every unblank a full power-up and init. The wake now does the
  same as Samsung's `_dsim_enable` / `panel_drv.c`:
  1. the reset line (`gpa3-4`) is held low while the panel sleeps;
  2. at the wake, the DSI clock lane is taken to LP-11 (stop state) **before** the reset is released;
  3. it goes back to HS, then Samsung's whole init follows (DSC + PPS, sleep-out, scaler/CASET/PASET, SET_AREA, TE, the
     boot loader's FFC/ISC/edge-dimming/brightness registers);
  4. `disp_det` (`gpg2-2`) and DCS 0A are checked. If the panel is not up it is reset again, up to three times, and if it
     never comes up the run goes back to the loader with the log kept.
- The panel answers a DCS read with an acknowledge-and-error report first and its data a moment later. Reads gave up too early,
  and the next read took the leftover data, so values were shifted one read along. Those wrong values, written into a freshly
  reset panel, left it "up" and black after the Apple logo. Reads now wait for the data. The boot loader's registers are read
  with no frame going out, twice, and kept only when both reads agree, and every write-back is read back.
- A second screen-off while already asleep (seen after a backboardd restart at boot) now sends nothing; before, it broke the
  asleep state and the panel was never woken.
- `MiniIOMobileFramebuffer.cpp`: while the panel sleeps there is no TE, so a flip not taken is no longer taken for a broken
  scan-out (which used to drop the scaler path for good: ghosting and stutter after every screen-off).

### Auto-Lock (`userland/src/inject_sb.c`)
With no CommCenter (no cellular), SpringBoard's `SBTelephonyManager` retried every second
(`_handleTelephonyDaemonRestart` → `handleCallControlFailure:` → `updateCalls` → `resetIdleTimer`), and every retry reset
the idle timer, so the screen never locked. The resets made inside that retry are dropped; the retry itself, and every other
reset (touch, buttons), are left alone. Log-only hooks of SpringBoard's idle timer stay in (`hakios: idle:` lines).

### Charging (`drivers/power/hakios.max77705.c`)
The charger was only read, never set, so iOS charged at 1-2 W. The driver now sets CNFG_09 (input limit) and CNFG_02 (charge
current) to 2000 mA behind CHGPROT, clears the watchdog, and limits the charge to 500 mA outside 0-45 °C.
`max77705_chg=0` turns it off.

### Bluetooth
- `inject_bluetoothd.c`: a missing H4 transport callback crashed bluetoothd at power-off; it is filled with a no-op.
- `userland/bluetool/D22AP.deepsleep.script`: there was none, so bluetoothd waited for a timeout after every power-off.
- `inject_bluetool.c`: answers to slow MobileGestalt questions are cached, which takes power-on to about 0.7 s.
- `inject_sb.c`: Control Center's Bluetooth button read a BluetoothManager of its own that never attached (grey button). It is
  given the shared one.

### Video recording (`mfc_encoder.c`, `inject_media.c`, `hakios.exynos_mfc.c`)
- The software HEVC encoder could not keep up, and hooking VideoToolbox through `__interpose` or the GOT never reaches
  shared-cache callers. The MFC is registered as a real VideoToolbox encoder plugin for `avc1`/`hvc1` instead.
- mediaserverd's capture graph is kept from crashing after the stop, and audio buffers longer than a second (garbage
  timestamps from the audio node) are dropped, so the movie is finished and plays.
- Known: the recorded sound ticks/stutters. Still being worked on.

### Also
- S Pen: `inject.c` handled 10 fingers and the pen is finger 11. Ghost touches around the pen tip are dropped while the pen is
  near.
- Storage: larger UFS slot buffers, sleeping waits, and 30-second write statistics (`hakios.ufshc.c`).
- Keys: **Bixby + volume-down held for 3 s** ends the run and goes back to the loader with the log kept. This is the way
  out when the screen is dark and the touch screen dead. (Volume-down + power is the phone's forced restart, which loses the
  log.)
- `kern.hakios.ustacks` (`hakios.input.c`): write a process name to dump its user stacks to the log.

## Building it

As upstream (scion-installer), with this fork's `note9` branch as the kernel and the Note9's boot-args:

```sh
./build-installer.sh --ipsw <your iPhone10,3/10,6 16.5 IPSW> --device all --kernel-ref <commit of this branch> \
  -- -N -t 0 -a "crownlte=exynos_keys=1 s6sy761=1 mali=9 fb_zc=1 fb_dpp=1 cpus=8 m3_prep=3 isa_floor=1 pan=0 \
max77705=1 max77705_haptic=1 wifi=1 wifi_pcie=6 brcmfmac=8 brcmfmac_poll=1 brcmfmac_tcm_idx=1 mali_gov=1 cpufreq_gov=1 \
tmu_stop=90 ufs=3 panel=1 panel_gamma=0 backlight=2 abox=8 max98512=3 bt=7 cam=5 cam_s9=7 mfc=5 cs47l93=6 abox_mic=1 \
shub=6 ufs_write=1 var_hfs=ufs_USERDATA_data"
```

with the environment `SYSTEM_FULL=1 FRAGMENTS="panel9 mfc9 sense9"`. `var_hfs` keeps `/private/var` on USERDATA, which must
have been formatted as HFS+ once (this **erases Android's data**; see scion-installer's `--format-userdata`). Back up Boot,
System and EFS in TWRP first. The loader's boot-argument line has a length limit past which it is cut silently, so check the
log's "Boot args" line after changing it.

Testing: pull `/proc/last_kmsg` in TWRP after each run. End a run with five fingers held on the screen for 1.5 s, or with
Bixby + volume-down for 3 s. A forced restart loses the log.

---

*The original Scion README follows, unchanged.*

# Scion

English · [中文](README.zh.md) · [Українська](README.uk.md)

**Scion runs Apple's open-source XNU kernel on Android phones, with our own drivers: iOS 16.5 and iOS 27 on the Galaxy S9, with no emulator, from one kernel tree.**
This is an experiment, not a finished ROM: you build it yourself from your own copy of iOS, and nothing of Apple's is in these repositories.

This repository holds the shared drivers, the loader, the tools and the documentation for both lines: it builds the iOS 16.5
kernel (xnu-8796) and is the recipe for the iOS 27 one, whose source is the fork [Scion-Project/xnu](https://github.com/Scion-Project/xnu)
(`scion/xnu-12377`, where the Galaxy S9 and the Pixel 10 are two machine configurations of one tree; [docs/two-bases.md](docs/two-bases.md)).
The aim is one kernel tree for every phone Scion runs on, on the phones themselves, with Android kept beside it; choosing iOS or
Android at power-on is designed for the S9 and not yet run ([docs/dual-boot-s9.md](docs/dual-boot-s9.md)).
Website: **[scion-project.github.io](https://scion-project.github.io)**.

Scion is built on [HakiOS](https://github.com/oosexgang/hakios-xnu-kernel), and this repository is a fork of it.

## Phones

Each state below is one of three: **seen on a phone**, **built, not yet run**, **not built**.

| Phone | iOS 16.5 | iOS 27 |
|---|---|---|
| **Galaxy S9** (SM-G960F, Exynos) | **Seen on a phone.** All eight cores: touch, GPU, Wi-Fi, speakers, microphone, front camera, Bluetooth, sensors, vibration, battery. Tested on only a few phones so far. The modem: built in since 17 November, **not yet run** under 16.5 | **Seen on a phone**: the home screen, eight cores, touch, the GPU (OpenGL ES, or Metal with Liquid Glass), storage. On their own branches: the modem registered with a SIM, Wi-Fi joined from iOS, rotation, Sileo. Everything on by default since 17 November: **built, not yet run** together ([docs/ios27/INSTALL-S9.md](docs/ios27/INSTALL-S9.md)) |
| **Galaxy S9+** (SM-G965F, Exynos) | **Seen on a phone**: run by Anan Jaser on his own phone (an SM-G965N) from his own build (see [Contributors](#contributors)). The installer's S9+ image: **not yet run**; its first run collects a log for 2 minutes, then returns to TWRP | Not tried |
| **Galaxy Note9** (SM-N960F, Exynos) | **Built, not yet run.** The same first run as the S9+ | Not tried |
| **Pixel 10** | In a VM on the phone's own hypervisor, through the [Scion app](https://github.com/Scion-Project/scion): **seen on a phone**, the home screen; touch was still being fixed at its last status, and there is no ready-made install for other owners. On the bare metal: **built, not yet run** (QEMU only) | On the bare metal, machine configuration FRANKEL of the S9's tree: **built, not yet run**; in QEMU iOS 27's `launchd` runs its boot tasks and stops at `mount-phase-1` ([docs/ios27/PIXEL10.md](docs/ios27/PIXEL10.md)) |

Snapdragon models are not supported. The installer refuses the Korean (`N`) models unless it is built with `-N`, since their
boards' device trees were never read; the S9+ that has run is a Korean model, from its owner's own build.

## Install on a Galaxy S9

You need an unlocked bootloader, TWRP, a computer with about 20 GB free, and your own `iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw`. No ready-made download exists, because the installer contains iOS's own files and is yours alone.

1. **Back up.** In TWRP, back up Boot, System and EFS, and copy the backup to a computer. EFS holds your IMEI.
2. **Build the installer** with [scion-installer](https://github.com/Scion-Project/scion-installer):
   ```sh
   git clone https://github.com/Scion-Project/scion-installer && cd scion-installer
   ./build-installer.sh --ipsw /path/to/iPhone10,3,iPhone10,6_16.5_20F66_Restore.ipsw
   ```
3. **Flash it.** Copy `out/scion-exynos9810-installer.zip` to the phone, install it in TWRP, and reboot to System. Don't use `adb sideload`, which can't send a file over 2 GB.

To leave iOS, hold five fingers on the screen for 1.5 seconds; the phone restarts into TWRP and keeps the log. Restore your Boot and System backups to go back to Android.
The installer's [README](https://github.com/Scion-Project/scion-installer#readme) and [step-by-step notes](https://github.com/Scion-Project/scion-installer/blob/main/universal/TESTING.txt) have the detail.

## iOS 27 on a Galaxy S9

There is no installer for iOS 27. You build the kernel, the boot image and the root yourself, from this repository and your
own iPhone 11 restore image, and write them from TWRP; it erases Android's data and it can hang.
[iOS 27 on a Galaxy S9: build it and install it yourself](docs/ios27/INSTALL-S9.md) has the steps, what works, the known
problems and how to get back.

## Known issues

- **Unexpected restarts:** one tester has reported the phone rebooting by itself. The cause isn't known yet, so please send a log if it happens to you.
- **No calls or SMS.** On iOS 16.5 the modem is in this repository's kernel since 17 November and has not yet been run; the
  installer's pinned kernel (10 November) does not have it. Use Wi-Fi.
- **iOS 27 can hang hard** some minutes into a session, and is slow in places ([its known problems](docs/ios27/INSTALL-S9.md#known-problems)).
- **No rear cameras:** only the front camera works. Headphones don't work either.
- **No App Store or Apple services:** no Apple ID, iCloud, App Store or iMessage, and that won't change. Sileo is included, but refreshing sources and installing packages haven't been tested.
- **Settings may not survive a restart:** if USERDATA still holds Android's data, `/private/var` lives in memory and is emptied at every boot. Keeping it needs a separate installer that formats USERDATA, which erases Android's data.
- **The passcode doesn't protect your files:** there is no Secure Enclave and files aren't encrypted. The passcode protects keychain items only, a 6-digit code can be guessed offline by anyone who reads the flash, and forgetting it loses the keychain ([details](docs/keystore.md)). Don't enable Erase Data.
- **The clock may be wrong at first:** the installer leaves the hardware clock out.

## Send logs

Save the logs in TWRP right after the problem, before anything else, because a full power-off loses them:

```sh
cp /sys/fs/pstore/* /sdcard/ 2>/dev/null; cat /proc/last_kmsg > /sdcard/last_kmsg.txt
```

If `/sdcard` isn't there, use `/external_sd`, or `/tmp` and `adb pull`. Then [report a bug](https://github.com/Scion-Project/scion-kernel/issues/new?template=bug.yml) with `last_kmsg.txt` and your phone's model, or [send a tester report](https://github.com/Scion-Project/scion-kernel/issues/new?template=tester-report.yml) if it worked. Never attach the installer, the system image or your IPSW: they contain Apple's files.

---

# For developers

Everything below is about the kernel itself. Scion's code still uses HakiOS's names (`hakios_*`, `/dev/hakios-*`, `kern.hakios.*`) so that it keeps merging with upstream, so "HakiOS" below means this kernel.

**Apple's open-source Darwin kernel (XNU, xnu-8796.121.2) on Android phones (Qualcomm, Samsung Exynos)**, the way Android is Linux on
phones: a boot loader of its own, drivers of its own, a configuration system and a version of its own. The same kernel runs in
QEMU with nothing of Apple's and nothing of any phone's (`make qemu-open`).

**Documentation: [docs/](docs/index.md)**, also on the project's website (`hakios-web`). Start with [Getting started](docs/getting-started/index.md), [Port a device](docs/porting/index.md) or the
[Architecture](docs/concepts/architecture.md).

## What works

| | POCO X5 Pro 5G (`redwood`) | QEMU `virt` |
|---|---|---|
| Lock screen, home screen, Control Center, the stock apps | yes | yes (slow: the CPU is emulated) |
| Touch, multitouch, buttons | yes | yes |
| All 8 CPU cores, the big one at 2.4 GHz | yes | no |
| GPU rendering on Adreno (OpenGL ES through Mesa) | yes | no |
| Speakers, microphone, the battery's level | yes | sound only |
| Wi-Fi, cameras, vibration | yes | network only |
| `/private/var` kept across reboots, Sileo, SSH | yes | no |
| Metal | partly: a device Metal loads; buffers and libraries; nothing drawn yet | partly |
| Cellular | partly: the modem is up, the SIM bound, the network seen; no registration yet | no |
| Bluetooth, charging under this kernel, Apple's services | no | no |

Every part, as each device's own `device.conf` records it: [Devices](docs/devices/index.md). The rows about the lock screen and the
apps describe what happens when you add layer 2 below yourself.

### Galaxy S9 (`starlte`, SM-G960F, Exynos 9810)

This repository is Scion-Project's fork of HakiOS. Its main addition is the Galaxy S9 port, which boots the iOS 16.5 user space
you build from your own IPSW, and, through the fork Scion-Project/xnu, the iOS 27 one ([docs/ios27/](docs/ios27/INSTALL-S9.md)). The device repository is [scion-starlte](https://github.com/Scion-Project/scion-starlte), and the
TWRP installer comes from [scion-installer](https://github.com/Scion-Project/scion-installer).
[scion-starlte's README](https://github.com/Scion-Project/scion-starlte#readme) says what has been seen on the phone and how to
build and install by hand, with each part's boot-arguments. [docs/devices/starlte.md](docs/devices/starlte.md) is generated from
the device's own `device.conf` at the revision `devices/manifest.conf` pins.

### Galaxy S9+ (`star2lte`, SM-G965F): run by a contributor. Galaxy Note9 (`crownlte`, SM-N960F): never run

The same SoC and, by Samsung's device trees, the same board as the S9's for everything the drivers touch, so they share the
S9's drivers and the loader's S9 platform under their own names (`loader_platform=`, written by their image scripts).
**Until 12 November 2026 nobody had run any of it on either phone.** Since then the S9+ has been run and worked on by
Anan Jaser on his own phone (see [Contributors](#contributors)); the Note9 is still untried. There is no UEFI of their own (Silicium has no target for them): their images
borrow the S9's, only when asked (`mkimage.sh -U`). Their device repositories,
[scion-star2lte](https://github.com/Scion-Project/scion-star2lte) and [scion-crownlte](https://github.com/Scion-Project/scion-crownlte),
say what differs from the S9, where each fact comes from, and what to try first; scion-installer's `universal/` installer
takes all three phones.

## The three layers

1. **The open core**: the kernel port, the loader, the drivers, the tools. All source, in these repositories.
2. **What you put on top**: the kernel can start a user space that you supply. It was built so that it can start the Darwin userland 16.5
   user space, which you lay out yourself from files you download from Apple; nothing of it is shipped, and nothing is
   circumvented to get it. Whether you may do that, and whether you want to, is your decision.
3. **Apple's services** (activation, Apple ID, iCloud, the App Store): not supported and not a goal.

By default the loader goes into RAM through `fastboot boot`, the system lives in RAM for a session, and a reboot brings the phone
back to its own Android. The one thing written by default is the loader's own log (a few KB) to the `logdump` partition; with
`ufs_write=1` the kernel writes only to a partition `darwin` that you create yourself. This is a research bench, not a ROM.

## Quick start

```sh
git clone https://github.com/Scion-Project/scion-kernel.git   # this fork; upstream is oosexgang/hakios-xnu-kernel
cd scion-kernel
make doctor           # what this machine lacks, and what to do
make qemu-open        # the open kernel in QEMU: nothing of Apple's, nothing of a phone's
```

Darwin in QEMU (Linux, 12 GB of disk, Apple's IPSW downloaded for you), and a session on the phone:
[QEMU](docs/getting-started/qemu.md), [On the phone](docs/getting-started/phone.md).

```sh
make redwood_defconfig   # the phone's configuration: the common kernel + the SC7280's + the phone's own
make kernel              # the kernel with the drivers that configuration builds
make phone SECS=600      # a 10-minute session on the phone, in RAM
```

## Repositories: common, SoC, device, UEFI

| Repository | What |
|---|---|
| [scion-kernel](https://github.com/Scion-Project/scion-kernel) (upstream: [hakios-xnu-kernel](https://github.com/oosexgang/hakios-xnu-kernel/tree/main)) | **common**: the kernel port, every common driver, the loader, user space, the tools, this documentation, and `devices/qemu` |
| [hakios-sm7325](https://github.com/oosexgang/hakios-sm7325) | POCO X5 Pro 5G: its defconfig, status, UEFI platform, firmware script, and its SoC's layer (`soc/`: the Qualcomm SC7280 family) |
| [hakios-uefi](https://github.com/oosexgang/hakios-uefi) | the firmware that starts the loader: Mu-Silicium with HakiOS's boot path, banner and name, and HakiOS's U-Boot (`u-boot/`) |
| [hakios-metal](https://github.com/oosexgang/hakios-metal) | Metal's command model on the Adreno, through the kernel's GPU driver |
| [scion-starlte](https://github.com/Scion-Project/scion-starlte) | Galaxy S9 (Scion fork): its defconfig, status, UEFI platform and prebuilt UEFI, image script, and its SoC's layer (`soc/`: the Exynos 9810) |
| [scion-star2lte](https://github.com/Scion-Project/scion-star2lte), [scion-crownlte](https://github.com/Scion-Project/scion-crownlte) | Galaxy S9+ and Note9: their defconfigs, status (the S9+'s as seen on a contributor's phone; nothing seen on a Note9), image script, and the same `soc/` as the S9's; no UEFI yet |
| [scion-installer](https://github.com/Scion-Project/scion-installer) | Scion fork: the S9's TWRP installer (boot image to `BOOT`, the system image you build to `SYSTEM`) |

`devices/manifest.conf` in the common repository names each one and the revision it is at (in this fork, the Scion-Project forks of them); `tools/hakios-device.py sync` clones them
into `devices/` and `uefi/`. One repository per device: it carries its SoC's layer, and `tools/hakios-device.py new` makes one. [How the repositories fit](docs/porting/repositories.md).

```sh
git clone https://github.com/Scion-Project/scion-kernel.git && cd scion-kernel
tools/hakios-device.py sync      # the SoC, device and UEFI repositories, at the revisions the manifest names
```

A device of your own is a repository over the common one: `tools/hakios-device.py new` makes it from the phone's own device tree,
with every phone driver left out until it is ported. Each driver is an option of the kernel's own configuration (`CONFIG_HAKIOS_*`,
XNU's `options` and `conf/files`), so a device's defconfig builds exactly the drivers it has.
[Port a device](docs/porting/index.md), [Drivers in the configuration](docs/drivers-config.md).

## What is in the repository, and what never is

In it: only our own code and data: our changes to Apple's open-source XNU (the fork `Scion-Project/xnu`, APSL 2.0) and our files, the boot
loader, our programs and libraries, the tools, `prebuilt/` (built from this source; the loader **without** any Apple data:
`make loader` makes the one that starts Darwin, on your machine, from your own IPSW), and the device and SoC directories.

This fork adds:

| Directory | Contents |
|---|---|
| `avf/` | XNU as a guest of Android's own hypervisor (AVF/crosvm) for [Scion](https://github.com/Scion-Project/scion); see `avf/README.md` |
| `tools/xnu-tree.py` | the kernel's sources as the fork holds them (a cached checkout of the profile's commit), for the tools that read them |

Never in it: any file of Darwin (you build the system image on your machine from an IPSW you get yourself and hand to
`sysimage/unpack.sh`; nothing is downloaded for you, decrypted or circumvented); the phone's firmware (pulled from your own phone into
`fw-*/`, which git ignores); keys, certificates, activation, or anything that would make the phone pass as an iPhone to Apple.

## Independent, and Apple-free at the core

HakiOS is an independent research project. It is **not affiliated with or endorsed by Apple**;
"Darwin", "iPhone", "XNU" and "Apple" are named only to describe what the code works with. The core
kernel, the boot loader and the open tools build and run with nothing of Apple's:

```sh
make qemu-open      # the open kernel + a placeholder loader + a tiny raw-syscall user space, in QEMU
```

No IPSW, no Darwin user space, no firmware. Booting the stock Darwin user space (`make qemu`,
`make phone`) is an optional path that needs the IPSW and the phone's firmware, which you supply
from your own device.

## Contributors

- **Anan Jaser** ([@ananjaser1211](https://github.com/ananjaser1211)), the author of the Apollo kernel for the Exynos 9810.
  The Galaxy S9+ on real hardware: its extra RAM in the loader, its own panel tables, its sensor hub's mounting and
  calibration, its firmware set in the build. For both phones: the APM's DVFS tables read out and used for the processor,
  the Mali and the buses, the speakers' tone, Location Services and its tools, the GNSS bring-up of the BCM4775
  (`docs/gnss.md`, `/dev/hakios-gnss`, `gnsstool`), a kept data volume that survives a reset, and a long list of fixes to
  the build and the user space. Since then: the GNSS receiver for iOS (`gnssd`, locationd's GNSS through it, the GPS app,
  `docs/gnssd.md`), Metal on the Mali through PanVK and metal2vulkan (`docs/metal.md`: the window server and real
  applications on it, HakiosMali's host memory and cache requests, the PanVK and metal2vulkan patches), the display's
  layers composited as the iPhone's are and on two DECON windows (`fb_planes`), CoreTelephony's server for a phone with no
  SIM (`telephonyd`), the sensor hub's rates set by its clients and its board setup, and what the system image keeps
  (`docs/system-selection.md`). His commits are in the history under his name. Thank you, Anan.
- **Nils** ([@dwaris](https://github.com/dwaris)), a Galaxy S9+ owner and one of the installer's testers. Two fixes for the
  speakers found on his own phone: the first sound no longer lost when the stream starts before the amplifiers' driver has
  registered, and the MAX98512's enable given a moment before it is read back, with a pause between a bus transfer's
  retries. Then four for the S9+'s cameras: the S5K3M3 telephoto's two virtual channels told apart so its frames are
  real, the telephoto's availability read again after a failure, and the capture graph's audio ended cleanly when the
  camera is switched (end-of-stream markers sent, late samples and stale notifications dropped). His commits are in the
  history under his name. Thank you, Nils.
- **deLiseLINO** ([@deLiseLINO](https://github.com/deLiseLINO)), who runs Scion in AVF on a Pixel 7: the boot hang there
  found and fixed (FP/SIMD turned on before the jump to XNU), CoreAnimation's masks drawn on the Mali-G710 (GLSL ES 1.00
  extension lines dropped from ES 3.00 shaders), and the guest's network as a stream on the virtio socket device, served by
  libslirp in the Scion app (`virtio_net_vsock=PORT`). His commits are in the history under his name. Thank you.

## Licenses and the legal side

My code is under the license of the files it changes: the XNU patch under the Apple Public Source
License 2.0 (Apple publishes XNU under it; the terms are met: changes are published as source,
notices are kept), the rest as the file headers say. Darwin, its images, Apple's trademarks and the
phone's firmware belong to their owners, are not in this repository and are not distributed by me;
everyone takes them from their own phone and their own IPSW. "Darwin", "iPhone" and "Apple" are
named only to describe what the code works with. If you believe a file here infringes anyone's
rights, open an issue and I will remove it.

The detail is in separate files: `LEGAL.md` (what is and is not distributed, and the limits of
these statements), `COMPLIANCE.md` (how the APSL 2.0 and other terms are met, and the items still
to verify), `THIRD_PARTY_LICENSES.md` (every third-party component with origin and license), and
`NOTICE` (the short canonical list). None of this is legal advice.

hakios is an experimental project focused on bringing darwin to android hardware. our goal is to make hakios free and accessible to everyone.

owners:
@oodze - founder
@netusd - adversting / ceo-dev
@wannqn - dev 
@hxfuxyyXD - dev
Rhaenja Hun（杭润嘉） — Developer / Sponsor

tym sicilium for the sources!
