# Gold master 1 (gm-1): the checklist

iOS 27.2.1 (24B156) on the Galaxy S9 (SM-G960F, Exynos 9810) on Scion's XNU fork. Started 28 November 2026 at the owner's
word ("Get gold master ready, we need all support merged in. Rock solid."). Kept by the release coordinator (scion-gm-1128);
each row has an owner and a status: **merged**, **passed** (phone or CI), **needs phone test**, **blocked** (why), or
**deferred** (why; a deferral needs the owner's OK and says so). `gm-1` is tagged only when every row is merged/passed or
an approved deferral. No built image is published: every image carries its builder's IPSW and is built with
scion-installer from the user's own restore image.

Phones: phone 1 = 2ab4dd7d2a037ece, phone 2 = 23edfb2021027ece (both SM-G960F, TWRP 3.7.0_9-0, iOS 27.2.1 roots).

## 1. Pull requests and branches

| Item | Repo | Owner | Status |
|---|---|---|---|
| Regression of everything merged since the last phone run (#398 #408 #416 #418-#422, xnu #70) | scion-kernel, xnu | scion-regress-1128 | **passed**: phone 2, main `adbb15d6` + fork `f2f59ddd`, SpringBoard x3 (21 s), Wi-Fi + BT each boot, home drawn, idle 16-17 %, no panic (28 Nov) |
| #425 profile pin: fork `5d485cfa` -> `f2f59ddd` (a plain build of main had none of xnu #65-#70) | scion-kernel | gm | **needs CI + merge** (phone pass above) |
| xnu #71 phone-tested arguments built in: `bt_vsc=1 bt_addr_efs=1 sweet_priv_ent=1 wifi_delay=10` | xnu | gm | **needs phone test** (GM soak, an image with the short line) |
| #426 make-boot-image: canvas 828x1702 + `sweetfb_lh=1702`, `sweetspeed=442337279` (the phones' tree and line) | scion-kernel | gm | **needs phone test** (GM soak, an image made by make-boot-image.sh) |
| #427 24B156 is the default and supported build; INSTALL-S9.md asks for 27.2.1 | scion-kernel | gm | **needs installer dry run** (below) |
| #428 the ios-27 kernel builds on a stock Mac (mk-brcmfmac.sh: shasum, Xcode's ld, the applied tree) | scion-kernel | gm | **passed** on the mini (mk-brcmfmac rc 0); merge with the dry run |
| #406 HakiOS upstream sync (user space, loader, Wi-Fi layer); licence decided by the owner: APSL-2.0 for the sync's lines, mixed files, NOTICE | scion-kernel | scion-regress-1128 | **needs phone test**: rebased (9190178c); S4a/S4b boot, Wi-Fi up, BT not yet up on its boots (S4c running) |
| hakios-uefi #2, hakios-sm7325 #2 (upstream syncs, same licence decision) | hakios-* | scion-regress-1128 | **merged** 28 Nov (7e6ff6d5, 95e1478b) |
| scion-installer #34 prebuilt by default | scion-installer | scion-regress-1128 | **merged** 28 Nov (e9770869), CI green |
| scion-installer: iOS 27 (`build-installer.sh --ios27`, ios27/build-ios27.sh, README) | scion-installer | gm | **needs dry run + CI** |
| xnu #70 camera preview composited (viewfinder no longer black) | xnu | scion-p1-1128 | **passed with `cam_metal=1`**: live rear viewfinder on phone 1 (28 Nov); without `cam_metal=1` the preview path is not built (see 2) |
| Unmerged branches with no PR: scion-kernel `s9/mfc-idle` (iOS 16.5 MFC idle, never run), `installer/16.5-pin` (11 Nov, superseded by installer #35); xnu `wip/ios27-wifi-rc1`, `wip/m3-residual-loudlog(-r2)` (9-10 Nov WIP, superseded by the merged Wi-Fi defaults and M3 work); scion-installer `ci/fresh-build-mesa` (10 Nov); scion-boot-exynos9810 `web/landing-demo` (site demo) | all | owner | **deferred, not in GM** (stale or iOS 16.5 only; deleting branches is the owner's call) |

## 2. Features that are off by default (iOS 27 kernel: the fork's `starlte_default_bootargs`)

| Feature (argument) | Decision for gm-1 | Evidence |
|---|---|---|
| Bluetooth vendor replies, own BT address, privileges by entitlement, Wi-Fi at 10 s (`bt_vsc bt_addr_efs sweet_priv_ent wifi_delay=10`) | **on** (xnu #71) | on every test phone's line since 18 Nov (78 line files) |
| Panel-shaped canvas (`sweetfb_lh=1702`, ADT canvas 828x1702) | **on** (#426) | every phone since 18 Nov |
| Camera preview through Metal (`cam_metal=1`) | **off: blocked** | the viewfinder is live only with it (phone 1, 28 Nov), but with it cameracaptured takes 99-100 % of a core even in the background, the big cores sit at 51-64 C idle and reach 82-83 C within 10 s of opening Camera; phone 1 dropped off USB during the test. Needs a fix before it can be on; until then Camera opens with a black viewfinder. **Owner's OK needed to ship gm-1 with it off.** |
| Fast charging (`sweetafc=1`) | **off** | live charging tests only with the owner beside the phones (rule) |
| M3s above 1794 MHz (`m3off=`, `gov_policy=3`, `ocp_guard=4`) | **off** | BIG-CORES.md: no faster interface, +6-8 C |
| Display scaler (`sweetfb_dpp`), core power-down (`sweetc2`), `sgu_depth` | **off** | arm_init.c: probe boot pending / never came back on a phone / measured harmful |
| Hang watchdog (`sweetwd`), warm restart (`sweetwarm`), console root shell (`devsh`), traces (`cam_trace`, `sbprof`, frame timeline) | **off by design** | diagnostics or test-only; `devsh` is a root shell for whoever holds the cable |
| microSD (`sdcard=`, #422), charger guard `max77705_chg=` (#421) | **not in the iOS 27 image** | iOS 16.5 kernel drivers; the iOS 27 kernel has its own guard (`sweetchg=2`, on since 24 Nov); microSD write support stays off |
| `keystore_real`, `panel_sleep`, `sleep_cores`, `mfc=5`, web clips | **not in the iOS 27 image** | iOS 16.5 line |

## 3. Known bugs

| Bug | Owner | Status |
|---|---|---|
| Bluetooth does not come up on ~39 % of boots (BlueTool never exec'd; 14 of 36 phone-2 boots 20-28 Nov, every kernel) | gm | **blocked: no fix yet** (needs a cause; the GM soak counts it) |
| Camera viewfinder black without `cam_metal=1`; with it, cameracaptured spins and the cores overheat | p1 / gm | **blocked** (above) |
| chronod and locationd crash at boot (once each, then fine) | - | baseline since 18 Nov; not a respawn storm |
| scion-kernel #410 status bar laid out for the iPhone X notch | - | open; cosmetic |
| INSTALL-S9.md "Known problems": rare hard hang (24 h return to TWRP), lock screen clock not drawn, wallpaper picker, four Control Centre controls, volume slider, no crash reports | - | open; listed in the release notes |
| INSTALL-S9.md section 4 (the root's userland: sbca, Metal bundle, m.dylib, b.dylib, wallpapers) has no single command and two libraries have no public recipe | gm / owner | **blocked**: without it a user's root draws nothing past the console. Needed for "installable by someone else" |
| scion-installer #28 (S9+ won't boot), #16 (64 GB not used): iOS 16.5 | - | not iOS 27; open |
| scion-kernel #402 (no speaker sound, iOS 16.5) | - | not iOS 27; open |

## 4. Installer

| Step | Status |
|---|---|
| `build-installer.sh --ios27 --dry-run` on the Mac mini, 24B156 IPSW, clean clone of scion-kernel gm/candidate | run 1 failed at the kernel (mk-brcmfmac.sh, fixed in #428); run 2 in progress |
| The root from a clean clone (`--overlay`, sudo, 62 GB free) | **blocked**: the mini has 41 GB free; owner asked |

## 5. Soak (rock solid), on a FREE phone, the GM candidate (main + the rows above, defaults as decided)

| Test | Result |
|---|---|
| 2 cold boots to SpringBoard | pending |
| 60 min idle, screen on/off cycles | pending |
| 20 min mixed use (flicks, Settings, Camera open/close, Wi-Fi reconnect, BT on/off) | pending |
| Reboot loop x5 | pending |
| Pass = no panics, no crash loops, no respawn storms, temperatures in range, battery drain recorded | pending |
