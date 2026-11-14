# iOS 27 on the Galaxy S9: UI and stability findings, night of 13-14 November 2026

Phone 1 was unplugged for the whole run, so everything here was found in the night's console logs, the root image and the code, and
built; **nothing below has run on a phone yet** unless it says so. All kernel behaviour is behind boot arguments, off by default.
Kernel: Scion-Project/xnu `s9/night-ui-ios27` (draft PR #23) on `s9/p1-everyday-ios27`. Tools: this branch, `tools/ios27/ui/`.

| What was seen | Cause | Change | Switch |
|---|---|---|---|
| backboardd SIGSEGV twice on 12 Nov (whole UI restarts, ~20 s black on the Metal line) | PanVK reads `vs->rsd` when no vertex shader is bound (`panvk_shader_hw_variant()` is NULL), in `panvk_cmd_draw` / `panvk_cmd_draw_indirect`, while the Metal runtime replays a pass | the draw is skipped, as the comment there already says | `tools/ios27/mesa-ios/mesa26-panvk-null-vs.patch` (build-time) |
| a restart or power-off from iOS freezes on the last frame | this tree has no `PE_halt_restart`; `halt_all_cpus()` spins | restart = PMU reset into the normal boot; power off = PS_HOLD low (restart if a charger keeps the phone up); never during a panic | `sweethalt=1`; test `sysctl kern.sweethalt_test=1/2` |
| every boot starts on 1 January 1970 (lock screen "Wed Dec 31") until the network sets the time | the platform expert answers 0 for the time of day | a floor (`sweetepoch=`, the image's build time) and the S2MPS18 RTC read only through ACPM | `sweetepoch=SECONDS`, `rtc_pmic=1` |
| ... and the time is lost at every boot | no clock iOS reads | `com.scion.lasttime` keeps the last known time on the data volume and puts the clock forward at start-up | `70-ui-stability.sh` |
| the root's time zone is US/Pacific | as installed | optional `SCION_TZ=Area/City` | `70-ui-stability.sh` |
| battery always 100 %, on AC | `SweetBattery` on STARLTE is a static fallback | the MAX77705 fuel gauge read only, on the bus the USB-switch code drives (shared under a lock) | `sweetfg=1` |
| atc aborts 12-17 times a boot with Wi-Fi up | MusicLibrary throws when `/var/mobile/Media/iTunes_Control` is missing | the directories, owned by mobile | `70-ui-stability.sh` |
| Books' and Podcasts' widgets die in dyld at every boot ("security policy does not allow @ path expansion") | the AMFI stand-in's dyld-policy answer lacks `AMFI_DYLD_OUTPUT_ALLOW_AT_PATH`; any app or extension with its own frameworks is refused | the bit set | `sweetatpath=1` |
| the wallpaper picker never makes a poster | PosterBoard's store: APFS purgeable `fsctl()`s fail on HFS (existing fix, `sweetspeed&0x1000000`, not in the everyday line), then CoreVideo aborts: MobileGestalt `HardwarePlatform` is NULL because the ADT root has no `platform-name` | `adt-platform-name.py` (t8030, the SoC of the model the S9 presents as) | ADT + `sweetspeed` bits |
| home screen: a black wedge top left | the stop-gap wallpaper is Apple's background layer, whose top rows are covered by the foreground layers in the real poster | the same layer cropped | root step |
| status bar time missing at the 1702 canvas, present at 1792 | layout: the forced `_UIStatusBarVisualProvider_Split828` class at 828x1702 | to test: `Phone`, `CustomSplit`, unset | root step |
| no keyboard in Spotlight on the Metal line | not known; iOS 27's keyboard host is InputUI (not a Metal client, nothing injected) | diagnostic: InputUI in the injection group + its os_log captured | boot line |
| the panel never powers off (no "display off" in any log) | not known | to test with the console's `button lock` | - |

Also seen, not changed: locationd SIGSEGV with pc 0x200 from a libdispatch callout on the modem / Wi-Fi boots (a call through a
PAC-signed function pointer that came out wrong: the PAC emulation); Control Centre control extensions (Calculator, World Clock,
Launcher, Code Scanner) hit a Swift assertion in WidgetKit three times a boot; chronod aborts once a boot in QuartzCore's
`query_displays` (start-up race). backboardd's GPU buffers grow to ~430 MiB at idle against a 514 MiB per-client cap, and on 12 Nov
reached the cap after about five minutes of use ("REFUSED for backboardd"): the leading candidate for the reported hangs; the front end
answers `currentAllocatedSize` 0, so nothing in CoreAnimation sees the pressure. To measure with `STARBITE_DRM_LEAKS=1` in a soak.
