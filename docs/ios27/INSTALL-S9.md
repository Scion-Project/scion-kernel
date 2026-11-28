# iOS 27 on a Galaxy S9: build it and install it yourself

This page is for someone outside the project who has a Galaxy S9, a Mac and an iPhone restore image downloaded from Apple, and wants
to see iOS 27 start on the phone. It is experimental. Read "What works" and "Known problems" before you spend an evening on it.

**It erases the phone.** The iOS system is written over Android's data partition (`USERDATA`). Everything Android stored there is
gone. Android's own system partition and the recovery (TWRP) are not touched, and the boot loader is never written.

Nothing of Apple's and nothing of Samsung's is in this repository, and nothing may be added to it. You make the kernel from source
and everything else from **your own** restore image on **your own** Mac. Do not upload or share what you build: the boot image, the
root image and the device tree carry your restore image's identity and Apple's files.

Written 12 November 2026, the S9's features in the root build added on 17 November. Each step below says whether it was run
from a fresh clone of the public repositories ("clean run") or is written down from how the project's own test phones were made.

## What you need

| | |
|---|---|
| Phone | Galaxy S9 **SM-G960F** (Exynos 9810, "starlte"). Not the Snapdragon models (G960U/W/0), not the S9+ (untested on this line). OEM unlock done and **TWRP installed in recovery** the way Samsung and TWRP document it; that step is yours and is not part of this page. Charge it. |
| Mac | Apple silicon. The build machine ran macOS 27.0.1 with 8 cores and 16 GB. About **110 GB free**: the restore image (9 GB), its three unpacked images (20 GB), the kernel build (4 GB), the root build (wants 62 GB free when it starts). |
| Xcode | **Xcode 16.4**, from Apple's developer downloads (`Xcode_16.4.xip`). It need not be the selected one: the commands take `DEVELOPER_DIR`. |
| Kernel Debug Kit | **Kernel Debug Kit 26.5, build 25F71** (`Kernel_Debug_Kit_26.5_build_25F71.dmg`), from Apple's developer downloads (a free account is enough). Unpack it with `pkgutil --expand-full` and point `KDKROOT` at the `.kdk` directory. |
| Restore image | The **iPhone 11** image **iPhone12,1, iOS 27.2.1, build 24B156** (`iPhone12,1_27.2.1_24B156_Restore.ipsw`, 10 977 415 204 bytes), from Apple's own servers (Finder saves it, or `ipsw download ipsw --device iPhone12,1 --build 24B156`, which reads Apple's catalogue and downloads from Apple). It is the build the test phones run and the tools' default (`tools/ios27/builds/DEFAULT`) since 28 November 2026. The earlier 27.0.1 (24A446) still builds with `IOS27_BUILD=24A446` on every command (and on `sudo`'s line); no other build works: the tools check addresses inside these two. Do not use an image from anywhere else. |
| Tools | `jq`, `gum`, `xcodes`, `cmake`, `ninja` (Homebrew has all five), `git`, Android's `adb` (platform-tools), blacktop's [`ipsw`](https://github.com/blacktop/ipsw) (3.1.743 was used), and a Python 3 environment with `numpy` and `ziglang` (`python3 -m venv venv && venv/bin/pip install numpy ziglang`). |
| A USB cable | The phone's console is a serial port over USB-C. It is how you see the log and how you ask the phone to go back to TWRP. |

In the commands below `$PY` is the Python of that environment, `$SERIAL` is your phone's serial as `adb devices` prints it.

    export DEVELOPER_DIR=/Applications/Xcode_16.4.app/Contents/Developer
    export KDKROOT=$HOME/kdk/KDK_26.5_25F71.kdk
    export PY=$PWD/venv/bin/python
    git clone https://github.com/Scion-Project/scion-kernel.git && cd scion-kernel

## 1. The kernel (about 5 minutes; clean run)

    OS_PROFILE=ios-27 make kernel JOBS=4          # -> ./hakios-kernel

This fetches the kernel source (the `scion/xnu-12377` branch of Scion-Project/xnu at the commit `profiles/ios-27.conf` names),
blacktop's darwin-xnu-build at a fixed commit, and Apple's open-source pieces it needs, and builds an iOS-target XNU for the S9.
A clean run on the build machine took 258 seconds at `JOBS=3`. The result has the same 93,206 symbols and the same 2,620,499
instruction words as the kernel the test phones run; the only differences are the build's time stamp and the path of the build
directory in 258 assertion strings, and the addresses that shift because of them.

The S9's Wi-Fi radio driver (Linux's brcmfmac, ISC, on FreeBSD's LinuxKPI) is linked into the kernel. When
`xnu-deps/build/brcmfmac/out/brcmfmac.o` is not there yet, the build makes it first (`tools/mk-brcmfmac.sh`: it fetches
those sources at pinned sums) and stops if it cannot; `NOWIFI=1` builds a kernel without Wi-Fi on purpose. Until 17
November a kernel built without running that script first had no Wi-Fi, and said so only in its boot log. The step that
makes the object here was written on 17 November and has not been run from a clean clone; a kernel linked with the object
was built that day.

## 2. Your inputs from the restore image (about 15 minutes; all but the decryption was a clean run)

    tools/ios27/s9/fetch-apple-inputs.sh ~/Downloads/iPhone12,1_27.2.1_24B156_Restore.ipsw inputs

It reads the image's manifest, checks the build, and writes `inputs/os.dmg`, `inputs/systemos.dmg`, `inputs/appos.dmg` and
`inputs/n104ap.adt` (the iPhone 11's device tree). Two of the three images are Apple Encrypted Archives; the key for each is the
one Apple's own key service returns for the address in the archive's header, and macOS's `aea` does the decryption. The project's
phones were made with these same commands run by hand. The script was run in the clean room on a stand-in image holding the real
manifest and device tree: it reads the manifest, unpacks, and its `n104ap.adt` is the one the test images were made from. The
decryption of the two archives was not repeated (no room for a second copy of the images on the build machine).

## 3. The loader and the boot image (under a minute; clean run)

    tools/ios27/s9/build-loader.sh                                     # zig as the cross compiler: no Linux VM
    tools/ios27/s9/flash.sh $SERIAL pull-template inputs/recovery.img  # the phone in TWRP: a copy of ITS recovery partition
    tools/ios27/s9/flash.sh $SERIAL backup-boot  inputs/android-boot.img   # keep this file: it is how Android boots
    tools/ios27/s9/make-boot-image.sh --kernel hakios-kernel --donor-adt inputs/n104ap.adt \
        --template inputs/recovery.img                                 # -> out/xnu-boot-ios27.img

The boot image carries the sensor hub's firmware (`bcm_sensorhub_a1.bin`) in its device tree, because the kernel loads it
before there is a root. `make-boot-image.sh` does that by default: it fetches the S9's firmware and checks it, as the root
build does (section 5, "The S9's firmware"), and stops if a file is missing or wrong. `--firmware none` makes an image
without it, and then there are no sensors and no rotation.

**The panel's own shape.** By default iOS draws the iPhone 11's 828 x 1792 screen, which is not the panel's shape, and the
kernel scales it to the panel. The test phones draw 828 x 1702, the panel's shape exactly
(`docs/ios27/PRESENT.md`): `--canvas 828x1702` puts that canvas in the device tree (`mkadt_starlte.py --canvas`), and the
line must then give the kernel the same size (`--cmdline 'sbgl=0 sweetspeed=408782847 sweetfb_lh=1702'`; the width 828 is
built in). The script refuses a canvas the line does not match. Checked offline only: without `--canvas` the image is made
as before (the same bytes but the device tree's random seed and the header's checksum); with it the tree's display
properties are those of the 828 x 1702 tree the test phones boot. A boot image made with `--canvas` has not been booted.

**Make the boot image again after the root (section 5).** The root build leaves beside the root image a record of the
libraries it installed for the kernel to insert into programs (`tools/ios27/fullroot/out/root-full.img.inject`: the
rotation's `o.dylib` for backboardd, the status bar's `c.dylib` for SpringBoard). `make-boot-image.sh` reads that record
when it is there (or the one `--inject-record FILE` names; `--inject-record none` ignores it) and writes `sweetinject=`
as the kernel's own three libraries plus exactly the recorded ones (`tools/ios27/s9/cmdline-inject.py`). It refuses a line
that would not fit the image's 607 bytes, a `--cmdline` that names `sweetinject=` itself, and a kernel whose built-in
libraries are not the ones it knows. The record and the boot image must agree: a library named on the line that is not on
the root stops backboardd or SpringBoard from starting at all, and the phone shows nothing. Without a record the image is
made exactly as before.

The boot image is an Android boot image whose "kernel" is the loader; the loader carries the XNU kernel, the S9's device tree in
Apple's format (made from your iPhone 11 tree and the S9's own memory map) and a short command line (`sbgl=0
sweetspeed=408782847` and the time the image was made, `sweetepoch=`; everything else is built into the kernel). The copy of your recovery partition is only a template: its
header and Samsung's device-tree block are kept so that the phone's boot loader accepts the image.

Clean run: the loader built this way is byte for byte the one in the test phones' boot image, and the script given the test
phones' kernel reproduces their boot image except for the 256 random bytes of the device tree's seed and the header's checksum.
On a phone (12 November 2026, a test phone that already held a project-made iOS 27 root): `pull-template`, `backup-boot`, the
boot image made by these commands from the clean-run kernel and loader (command line `sweetspeed=140347391`, the OpenGL ES
compositor that root was set up for), `flash.sh boot ... reboot`: the USB console came up 14 seconds after the flash, the first
picture after 30 seconds, and SpringBoard's lock screen was on the panel with no panic. `to-recovery.sh` brought it back to TWRP.

## 4. The userland the root needs (hours; NOT a clean run, see below)

iOS draws through Apple's GPU drivers, which the S9 does not have. The root therefore gets a few libraries of ours that are built
from source: `sbca` (CoreAnimation's renderer through OpenGL ES on the Mali-G72), `libsbgl` and Mesa's `libOSMesa` under it, and
for the Metal compositor `StarbiteMetal.bundle` with Mesa's PanVK. Their build scripts are in this repository and each says what
it needs:

| Piece | Script | Needs |
|---|---|---|
| Mesa 25.0.7 (OSMesa + Panfrost) | `tools/ios27/mesa-ios/mk-mesa-ios.sh` | meson, mako, pyyaml (a venv), ninja, Homebrew `bison` and `flex` |
| `libsbgl`, `libOSMesa` staged and signed | `tools/ios27/perf/build-sbgl.sh` | the Mesa above |
| `sbca.dylib` | `tools/ios27/ca-gpu/build-sbca27.sh` | the Mesa above |
| Metal: Rust, the bridge, host tools, PanVK, the bundle | `tools/ios27/metal/rust.sh`, `bridge.sh`, `build-host.sh`, `mk-panvk27.sh`, `mk-bundle27.sh` | Rust 1.98, Homebrew LLVM 23, Mesa 26.2.6 (`docs/ios27/METAL.md`) |

Lay the results out like the root (`usr/lib/sbca.dylib`, `usr/lib/s.dylib` as a second copy of it, `usr/local/starbite/lib/libOSMesa.8.dylib`,
`usr/local/starbite/lib/libsbgl.dylib`, and for Metal `System/Library/Extensions/StarbiteMetal.bundle/`) in a directory, here `overlay/`.

**This is the weakest part of the page.** These scripts were written and run on the project's Mac over several days; nobody has
yet run them in order from a fresh clone, and there is no single command for them. To try, you need Homebrew with `bison`, `flex`,
`meson` and LLVM 23, Rust through `rustup` (the script installs it under the build directory), about 10 GB and some hours. The
Metal bundle on the test phones also has two later changes that are only on a draft branch (scion-kernel pull request 281); the
OpenGL ES compositor needs neither and is the one to start with. Three files the test phones also carry have no
public build recipe yet: `m.dylib` (a static wallpaper stand-in; its source is `tools/ios27/metal/sbwall.m`), `b.dylib` (the
brightness slider) and the 24 stock wallpaper layers converted from HEIC to PNG. Without them the phone still starts; expect a
black wallpaper and a brightness slider that does nothing. Without `overlay/` altogether the root builds and the kernel boots it,
but nothing is drawn after the boot console.

## 5. The root image (about an hour; the cache rewrite is a clean run, the rest is not)

    hdiutil attach -readonly -nobrowse -imagekey diskimage-class=CRawDiskImage -mountpoint /tmp/sysos inputs/systemos.dmg
    tools/ios27/s9/prepare-root-inputs.sh /tmp/sysos/System/Library/Caches/com.apple.dyld overlay
    hdiutil detach /tmp/sysos
    python3 tools/hakios-device.py sync starlte     # once: devices/starlte, the S9's repository (the firmware's sums)
    sudo PY=$PY DEVELOPER_DIR=$DEVELOPER_DIR \
        tools/ios27/s9/build-root.sh inputs        # -> tools/ios27/fullroot/out/root-full.img and root-full.img.inject

iPhone software since iOS 17 is built for processors the S9 does not have (pointer authentication, and instructions newer than
its ARMv8.0 big cores). The kernel emulates what traps; to make the phone usable the build rewrites those instructions ahead of
time in a copy of your system's shared library cache and in every program of the root (`docs/ios27/AOT-REWRITE.md`,
`docs/ios27/TEXT-DATA.md`). It also applies five one-word changes to that cache (`tools/ios27/s9/patches-24B156.txt`, each
checked against your cache first), removes the launchd jobs for hardware the port does not drive, marks Setup as done, and
installs `overlay/`. The result is a raw HFS+ volume image, 32 GiB sparse with about 20 GiB used.

`sudo` passes on only the variables named on its own command line: give it every one the build is to see.

### The S9's firmware

The root carries the vendor firmware the S9's drivers load: the Wi-Fi's firmware, NVRAMs and CLM in `/private/var/hakios/wifi`,
and both Bluetooth patches (Murata's and SEMCO's) in `/private/var/hakios/bt`. The boot image carries the sensor hub's firmware
(section 3). The iOS 27 kernel reads nothing from the phone's VENDOR partition: it has no reader for it. These files are
Samsung's and Broadcom's and are in no repository; only their SHA-256 sums are, in `devices/starlte/firmware/<kind>/SHA256SUMS`.

Before it copies anything, the root build runs `tools/ios27/fullroot/firmware.sh`. That runs
`devices/starlte/tools/fetch-firmware.sh` as your own user, which fetches every file the sums name into
`devices/starlte/firmware/<kind>/`, a directory git ignores, and keeps each file for the next build. It takes them from
`FIRMWARE_FROM=DIR` (your phone's `/vendor/firmware`, an unpacked VENDOR, or a vendor tree) or else from the public LineageOS
vendor trees it pins at fixed commits. Then it checks every file against its sum. **A file that is missing or does not match
stops the build**, which names the file and says what to do. A root is never built without a feature's firmware while that
feature is on. `FIRMWARE_OFFLINE=1` uses only `FIRMWARE_FROM` and what is already there. `FIRMWARE_DIR=DIR` uses a
`firmware/` directory of your own, which is checked but never fetched into. `ENABLE_WIFI=0` and `ENABLE_AUDIO_BT=0` leave
those features out on purpose.

At the end the build prints one line for each kind of firmware: present and checked on the root, absent on purpose, or
ABSENT. ABSENT means a feature that is on lacks its firmware, and the build's offline checks then FAIL:

    firmware: audio: checked in .../devices/starlte/firmware/audio, not put on the root: no driver of the iOS 27 kernel loads it
    firmware: bt: present and checked, 2 of 2 in /private/var/hakios/bt
    firmware: sensorhub: checked in .../devices/starlte/firmware/sensorhub, not on the root: tools/ios27/s9/make-boot-image.sh puts it in the boot image's device tree
    firmware: wifi: present and checked, 6 of 6 in /private/var/hakios/wifi

Your phone's own per-device data (the Wi-Fi's MAC address and the Bluetooth address in EFS, the modem's NV data) is not
firmware. It is not bundled, and nothing here writes it: the kernel reads it from the phone, read-only, or does without it
(the Wi-Fi then uses the firmware's own address).

Until 17 November 2026 the Wi-Fi step needed `WIFI_FW_DIR` and the audio and Bluetooth step needed `HCD`. Without them
each step printed one line and the root was built without that feature: no Wi-Fi, and no audio or Bluetooth. Both
variables are still taken: a directory of your own for Wi-Fi, or one `.hcd` or a directory of them for Bluetooth. Both are
checked against the sums in the same way.

### The S9's features in the root

The kernel turns the S9's hardware on by default (below, "The S9's hardware is on by default"); iOS sees a feature only
when its piece is on the root. The root build's last five steps (`tools/ios27/fullroot/steps.d/9N-*.sh`) put them there.
Each is on by default, builds what it needs from this checkout with your Xcode, and says in one line what it did. If
your Xcode or a Python with numpy is missing, it says which variable to set and the root is built without that feature,
and the firmware summary at the end then shows that feature as ABSENT. Missing firmware stops the build instead (above).
`ENABLE_<NAME>=0` leaves one out. A feature that fails is undone (the root holds all of it or none of it) and the build
goes on; `STRICT=1` makes it stop instead.

| Step | Feature | Needs from you | Puts on the root |
|---|---|---|---|
| `90-sensors` (`ENABLE_SENSORS`) | turning the screen with the phone ([SENSORS.md](SENSORS.md)) | Xcode | `/usr/lib/o.dylib`, recorded for backboardd |
| `91-wifi` (`ENABLE_WIFI`) | Wi-Fi in iOS ([WIFI.md](WIFI.md)) | nothing: the Wi-Fi's firmware comes from "The S9's firmware", above (`WIFI_FW_DIR` for a directory of your own); and a kernel with brcmfmac (section 1 links it in) | the files in `/private/var/hakios/wifi`; wifid made loadable on the S9 (`WIFI_WIFID=0`: not); configd's Wi-Fi service when the root has configd's preferences (a root that has never booted has none: DHCP then waits some minutes at the first Wi-Fi boot) |
| `92-audio-bt` (`ENABLE_AUDIO_BT`) | speakers, microphone, Bluetooth ([audio-bt/README.md](../../tools/ios27/audio-bt/README.md)) | `PY` with numpy; both Bluetooth patches come from "The S9's firmware", above (`HCD` for one of your own); the kernel takes the patch for the module the Wi-Fi reads from the chip's OTP (`bt_otp_wait=`, `bt_module=`) | Scion's audio plug-in, the Bluetooth libraries and BlueTool's scripts, both patches in `/private/var/hakios/bt`, the jobs audiomxd, bluetoothd and BlueTool |
| `93-modem` (`ENABLE_MODEM`) | the modem's network in iOS: bars, the operator's name, data ([MODEM-IOS.md](MODEM-IOS.md)) | Xcode; for data, your operator's `cp_apn=` on the boot image's line (`--cmdline 'sbgl=0 sweetspeed=408782847 cp_apn=...'`) | `scion-celld` and its job, `/usr/lib/c.dylib` recorded for SpringBoard |
| `94-camera` (`ENABLE_CAMERA`) | the camera in iOS: the Camera app and its preview, from the camera the kernel drives ([CAMERA.md](CAMERA.md)); NOT run on a phone | Xcode; `cam_s9=7` on the boot image's line (`cam_rear=1` for the rear camera): not a built-in default; optional `CAMERA_TUNING_DIR`, colour tunings made from your own phone's files | `/usr/lib/cam.dylib` recorded for cameracaptured, MobileGestalt's cache removed (made again at the next boot); `make-boot-image.sh` then adds `/arm-io/isp` |

None of them installs a test job (`64-wifi-nettest.sh`, `66-celld.sh ... test`, the USB root shell of `tools/ios27/devsh/`), and
the Wi-Fi step copies a network's name and passphrase only with `WIFI_EXTRAS=1` (a private test root). After the root,
make the boot image again (section 3).

**What has been run.** Each feature's pieces were run on a phone on their own branch (the table at the end says how far).
The four steps were applied, as the builder's own user and without `sudo`, to a copy of one of the project's earlier full
roots, mounted without owners, with made-up firmware and patch files in place of a phone's: each step twice (the second
run changed nothing) and then undone (the root was as before, file by file and launchd job by job); a failure half way
through the audio step was undone the same way; the record was made from that root and composed into a command line with
the S9 kernel this page builds. Not run: the steps inside `build-full-root.sh build` under `sudo` (owners on), a boot
image made with a record, and **any of it on a phone: the root with all four features and the kernel with all of it on
have not been booted together.**

`prepare-root-inputs.sh` makes the build's private directory (`tools/ios27/fullroot/private/`, never in git): instruction tables
from your cache, the checked patch list, and one script of wardevsa's fetched from his public repository at a fixed commit.

Since 18 November 2026 the pipeline rewrites `libcorecrypto` like every other library and then recomputes its FIPS integrity seal
(`tools/ios27/a64e/fips-seal.py`, step 5b): the self-test is HMAC-SHA256 with an all-zero key over the library's `__text`, so the
seal of the rewritten code is made the same way, and only where the stock seal was true (`docs/ios27/CORECRYPTO.md`). Before that the
library was skipped and its 3,538 pointer-authentication words trapped into the kernel's emulation: on 24B156 they were the
phone's hottest traps (about 100,000 in ten seconds while the icons re-render after a switch to dark mode). Its two unloaded variants
(`_noasm`, `_trace`) are still left exactly as Apple built them. The paragraph below describes the builds before that change.

One library is left exactly as Apple built it: `libcorecrypto` (and its two variants). It checks a hash of its own code when it
starts, so rewriting it makes its self-test fail; the project's own phones run a rewritten copy with a local stand-in for the
self-test's helper, and neither the stand-in nor a way to make one is published (`docs/ios27/SPEED.md`, "Not published"). Your root
does not need it: the kernel emulates the pointer-authentication instructions of that library as they trap, which is how every
root booted before that local step existed (the home screens of 8 November and the morning of the 9th). What you pay is time in cryptographic
calls; how much has not been measured on its own.

Clean run: `prepare-root-inputs.sh` (81 seconds) and the whole cache rewrite (`pipeline/cache-pipeline.sh`, 8 minutes), unprivileged,
with their checks passing (the mapping replay prints `FAILS 0`, no protected data word changed). Given the same inputs as the
project's own build of 10 November, the clean run's 79 cache files are that build's byte for byte, except inside the three
`libcorecrypto` images. With your inputs it differs from that build in three stated ways: the CoreImage word of the patch list;
no private review list (145 places that might be data are left as they are, 105 of them small functions that keep trapping); and
instruction tables made from the untouched cache, which protect 1,613 more words.

**Not run from a clean clone:** the copy into the image, the root steps and the offline checks, that is everything
`build-full-root.sh build` does after the cache. It needs an administrator password (`sudo`: the files must keep their owners) and
62 GB free; the build machine could give this work neither. The project's own roots were made by the same scripts in their
private layout, and several mistakes of the move to the public layout were found and fixed while writing this page. Expect to find
another one here, and please report it.

## 6. Install (about half an hour) and first boot

The phone in TWRP (hold Volume Up + Bixby + Power from off), on USB:

    tools/ios27/s9/flash.sh $SERIAL info
    tools/ios27/s9/flash.sh $SERIAL root tools/ios27/fullroot/out/root-full.img    # asks for the word ERASE; erases Android's data
    tools/ios27/s9/flash.sh $SERIAL boot out/xnu-boot-ios27.img reboot

Every write is read back and compared. Only `BOOT` and `USERDATA` are written. The first boot of a fresh root takes about two
minutes to the first picture (later boots 20 to 40 seconds). The log is on the USB console: `screen /dev/cu.usbmodem*` on the Mac.

Of this sequence, `info` and the `boot` line were run on a phone with an image built by this page's commands (section 3). The
`root` line was not: no root image has been built from a clean clone yet (section 5), and the test phones' roots were written
by the same `fastflash-root.py` from the project's private layout.

## Getting back

- **To TWRP from iOS:** `tools/ios27/s9/to-recovery.sh` (it writes the word `return` to the USB console), or Samsung's forced restart
  (Volume Down + Power for about ten seconds) and at once Volume Up + Bixby + Power, held until TWRP shows. The kernel also returns to TWRP by
  itself 24 hours after every boot; that is deliberate while the port can hang.
- **Another try:** from TWRP, flash a new boot image or root image with `flash.sh`.
- **Back to Android:** `tools/ios27/s9/flash.sh $SERIAL boot inputs/android-boot.img`, then in TWRP Wipe > Format Data (Android's
  data was erased by the install; Android starts as after a factory reset). Written down from how the partitions are used; not
  re-tested for this page.

## What works on iOS 27 today, and what does not

| | |
|---|---|
| Works | Boot to the home screen; the display at 60 Hz; touch; the Mali-G72 GPU (OpenGL ES compositor, or CoreAnimation's Metal renderer with Liquid Glass through PanVK); storage (the system on the phone's UFS, read-write); all eight cores; about 3.6 GB of RAM; the panel's brightness (with the brightness library of section 4); the USB console; Settings and the stock apps start (those that want a network have none). |
| In the kernel, and in the root build NOT RUN | The front and rear cameras: sensor, lens, focus, stabiliser and iris are driven and frames can be read from `/dev/hakios-camera` (boot-arguments, off by default). The root build's step `94-camera` puts the capture device of iOS 16.5 into cameracaptured, and with `cam_s9=7` on the boot image's line `make-boot-image.sh` gives the device tree the node by which the Camera app appears ([CAMERA.md](CAMERA.md)); built on 17 November and **not yet run on a phone**: until then take it that no iOS app sees a camera. Without `cam_s9=7` the Camera app does not appear on the home screen. |
| In the root build, on by default | **The accelerometer and the gyroscope, and screen rotation** ([SENSORS.md](SENSORS.md)): the sensor hub's driver and two HID services in the kernel (`shub=6 shub_hid=3`, on by default since 17 November) and a small library in backboardd (step `90-sensors`, recorded for `sweetinject=`). Seen on a phone on 12 November: the orientation followed the hand, and Notes turned to landscape and back, under the OpenGL ES compositor and, with the Metal bundle's turned textures (`frontend-turned.diff`, which `mk-bundle27.sh` applies), under the Metal compositor ([METAL-ROTATION.md](METAL-ROTATION.md)); the turn under Metal was driven from the console, not by turning the phone. The magnetometer, barometer, light and proximity sensors are read by the kernel only (`/dev/hakios-sensors`); iOS does not get them. |
| In the root build, on by default | **Wi-Fi** ([WIFI.md](WIFI.md)): the kernel has the S9's Wi-Fi since 12 November (`wifi_pcie=6 brcmfmac=8`, a default since 17 November), with the radio driver (Linux's brcmfmac, fetched and built by `tools/mk-brcmfmac.sh`) linked in by the kernel build (section 1); the root build's step `91-wifi` puts the Wi-Fi's firmware on the root (section 5, "The S9's firmware") and lets launchd start wifid. What a phone has done: in one boot on 10 November, on the work's own branch, the radio loaded its firmware, joined a WPA2 network, got an address by DHCP and answered `ping` and an HTTP request from the kernel's console; on 13 November, with wifid started through a device-tree node of a test image (not through the step's way, which has not been run on a phone), Settings showed Wi-Fi on and joined. The kernel as merged has been built, with and without the driver, and not booted with Wi-Fi on. |
| In the root build, on by default | **Cellular** ([MODEM-IOS.md](MODEM-IOS.md)): the S9's modem is started by the kernel (`cp=7 cp_nv=2 cp_if=1`, defaults since 17 November; its NV data is read from EFS read-only into RAM and nothing is written to the phone), and registers on the network with a SIM in. iOS shows it through the root build's step `93-modem` (`scion-celld`, and the status bar's `c.dylib` in SpringBoard), and there is no data without your operator's `cp_apn=` on the command line. The phone-call services are removed: no calls, no SMS. **Audio and Bluetooth**: the speakers, the codec, the microphone and the Bluetooth chip are started by the kernel (`abox=8 max98512=3 cs47l93=6 abox_mic=1 abox_rmem=1 bt=7`, defaults since 17 November); iOS uses them through the step `92-audio-bt`, which puts both Bluetooth patches on the root (section 5, "The S9's firmware"); with both there the kernel waits for the Wi-Fi to read the module from the chip's OTP and takes that module's patch, a choice written on 17 November and not yet run on a phone. Merged on 17 November; the kernel with all of it on has been built and **not yet run on a phone in this combination**, nor has a root made with the four steps. |
| Not in this build | A passcode (there is no Secure Enclave; the key store answers as a phone without a passcode). With no network there is no activation, no iCloud and no App Store; Setup is marked done by the build. Anything not named under "Works" should be taken as not working. |

## The S9's hardware is on by default (since 17 November 2026)

The kernel's built-in command line now turns on the S9's own hardware, so that one image has all of it: the
temperatures (`tmu=3`: read, the cores and the GPU's clock slowed by them, the GPU from 85 C, and the phone shut down and
powered off if any zone reads 110 C three times in a row; until 17 November this kernel read none and nothing stopped a
hot run), the sensor hub (`shub=6 shub_hid=3`), restart and power off from iOS (`sweethalt=1`), the battery's gauge and the PMIC's clock, both read
only (`sweetfg=1 rtc_pmic=1`), the package managers' installs (`persona_lax=1`: Zebra and Sileo become root by a spawn into a
persona; see [SILEO.md](SILEO.md), "Stuck at Queued"), Wi-Fi (`wifi_pcie=6 brcmfmac=8`), audio and Bluetooth
(`abox=8 max98512=3 cs47l93=6 abox_mic=1 abox_rmem=1 bt=7`) and the modem (`cp=7 cp_nv=2 cp_delay=40 cp_rfs=1 cp_if=1`).

Since 18 November 2026 the line also has the cameras (`cam_s9=7 cam_switch=1`: front and rear in one boot), the GPU's
job interrupt (`sgu_irq=1`, measured on a phone on 17 November), the reading of files compressed as decmpfs type 16
(`decmpfs_lz4=1`: the app switcher's snapshots) and dyld's `@rpath` for widgets (`sweetatpath=1`). Of these only
`sgu_irq=1` has run on a phone as written here; the others are on so that they are tested together. Left off on purpose:
`sweetfb_dpp` (after its probe boot), `sweetc2`, `gov_policy=3`, `ocp_guard=4` and `sgu_depth`. The built-in line is
now 851 of the command line's 1024 bytes: when the loader's line leaves no room the last ones are left out, and the
boot log names them (`STARLTE BOOTARGS: WARNING`).

- To turn one off for a run, name it on the command line with 0: `--cmdline 'sbgl=0 sweetspeed=408782847 cp=0 bt=0'`.
- The kernel side is on; what iOS needs on top of it (the sensors' library, the Wi-Fi firmware and wifid, the audio and
  Bluetooth pieces, the cellular daemon and the status bar's library) the root build of section 5 puts on the root
  ("The S9's features in the root"), with the firmware fetched and checked by the build itself. Without its step a feature
  is driven by the kernel and not seen by iOS, and the build's last lines say which.
- Nothing here writes to the phone's storage outside USERDATA. The modem's NV data comes from EFS read-only.
- How far each has been on a phone, on its own branch: the sensors and rotation (12 November), Wi-Fi and Sileo's installs
  (13 November), the modem registered and carrying data (13 November), audio and Bluetooth in part (9 November; the
  table above). **Never run on a phone at all:** restart and power off from iOS (`sweethalt`), the battery's gauge
  (`sweetfg`), the PMIC's clock (`rtc_pmic`) and the heat protection (`tmu=3`): written on 14 and 17 November with no
  phone attached, and built. **All of the defaults on at once has been built and not yet run on a phone.**

## sweet_sr_spec (on by default on the S9 since 18 November 2026)

`sweet_sr_spec` puts the neighbour source pages that the shared-region and dyld pagers read onto the speculative queue, so they can be reclaimed instead of staying resident. It is on by default on the Galaxy S9 (STARLTE) since [xnu PR 55](https://github.com/Scion-Project/xnu/pull/55); `sweet_sr_spec=0` turns it off. Other machines stay off by default. The change comes from UniXNU's `native-4k-shared-region-slide.patch` (BSD-2-Clause-Patent lines).

On one S9 boot, wired memory was 709 MiB with it on and 916 MiB without. That is one boot each way, not a clean A/B comparison.

## Known problems

- **It can hang hard.** On the test phones the kernel has a few times stopped dead some minutes into a session, console included,
  and the cause is not found. Samsung's forced restart (Volume Down + Power, about ten seconds) gets out of it. The 24-hour return
  to TWRP exists because of this.
- **It is slow in places.** Swiping the home screen with the Metal compositor keeps 43 to 67 % of frames on time; scrolling inside
  an app about 95 %. The first frame after boot takes 18 to 40 seconds.
- **Icons and Settings symbols** are right only with the rewrite's data-word guard and the CoreImage word in the patch list.
  That last word was proven on a phone only together with a kernel switch, bit 268435456 of `sweetspeed` (IOSurface tells iOS
  its largest sizes). The kernel this page builds has the switch since 12 November, off: the command line above
  until 17 November (`sweetspeed=140347391`) did not set it; the line since then, `sweetspeed=408782847`, does. With it set, and the root's icon steps, the Clock,
  Settings and Safari icons were drawn on a phone on 12 November; what the switch alone changes has not been measured.
  Without it some Settings rows may have no symbol.
- **The lock screen's clock is not drawn** (an empty box where the digits should be; the date line in the test boot read
  "Wed Dec 31"), and the **wallpaper picker** does not work.
- **Control Centre:** four of its controls (Calculator, Timer, Camera, Code Scanner) are empty, because their extensions
  crash three times each at start and stay off, and Airplane Mode shows on (it is on, in the root's stored preference).
  The likely cause of the first is a daemon the root build leaves out (`siriactionsd`); `CC_CONTROLS=1` keeps it, untested.
  `chronod` aborts once about 20 seconds after boot and comes back. Books' and Podcasts' widgets die in dyld unless the
  line has `sweetatpath=1`. See [CONTROL-CENTRE.md](CONTROL-CENTRE.md).
- **Volume:** the buttons change the volume and show the HUD only on a root with the audio step (audiomxd); Control
  Centre's volume slider does not move, and Settings' Wi-Fi row shows no network name. See [VOLUME.md](VOLUME.md).
- **No crash reports** are written (ReportCrash is not in the root's job set); the console's `CRASH` lines are the record,
  and `tools/ios27/crashlog-symbolicate.py` symbolicates them. [CONTROL-CENTRE.md](CONTROL-CENTRE.md) has the details.
- The patches and tables are for builds 24B156 (27.2.1, the default) and 24A446 (27.0.1) only. Another restore image is
  refused. The build the tools use is set in `tools/ios27/builds/` (`DEFAULT` and one `.conf` per build); what a move to
  another iOS build involves is in [UPDATING-IOS.md](UPDATING-IOS.md).

## Who made what

The S9 machine layer of this kernel is the work of Project Starbite and the layers under it (ios-sweet, ios-a5xelte, wardevsa's
Galaxy S7 layer); `NOTICE` and `LICENSES.md` have the names and licences. The loader is Starbite's (GPL-2.0-only). The scripts of
`tools/ios27/s9/` are Scion's (MIT-0).
