# iOS 27 on the Galaxy S9: Control Centre's blank controls, airplane mode, and crash reports

Written 17 November 2026 from phone 1's console logs of that day (the 13 November known-good image and the kernel of 17
November, both show the same), a copy of phone 1's root, and the restore image's shared cache (build 24A446), read on the
build Mac. **Nothing on this page has been run on a phone** except what the first section reports as seen. The changes are
off by default; the "Boots" section is the plan that decides them.

## What is seen

Control Centre opens. Airplane Mode is shown on (orange) while Wi-Fi is on and works. Of the round controls under the
sliders only Screen Mirroring has its symbol; the other four are empty platters. At every boot, about 30 seconds after
SpringBoard, the console has, three times each, a crash with signal 5 (SIGTRAP) of `CalculatorWidget`, `WorldClockWidget`,
`LauncherControlE` and `BarcodeScannerWi` (process names cut to 16 characters by the kernel); after the third the system
stops starting them. Also at every boot: `chronod` signal 6 once (it comes back), `SelectPerson_iOS` signal 6 once, and
Books' and Podcasts' widget extensions die in dyld. `/private/var/mobile/Library/Logs/CrashReporter` on the root is empty.

## The four blank controls

They are the four Control Centre controls whose extensions crash, one for one:

| Control | Extension (app) | Its intent (from the extension's own `Metadata.appintents`) |
|---|---|---|
| Calculator | `CalculatorWidget.appex` (Calculator) | `LaunchCalculatorOpenIntent`, an `OpenIntent` |
| Timer | `WorldClockWidget.appex` (Clock, `MobileTimer.app`) | `StartTimerControlIntent`, an `AppIntent` |
| Camera | `LauncherControlExtension.appex` (Camera) | in its package metadata (`extract.packagedata`), not read |
| Code Scanner | `BarcodeScannerWidgetExtension.appex` (Code Scanner) | `BarcodeScannerIntent`, an `OpenIntent` |

All four are WidgetKit extensions (`NSExtensionPointIdentifier` `com.apple.widgetkit-extension`) with no
`UIRequiredDeviceCapabilities`. Screen Mirroring is one of Control Centre's own modules, not an extension: that is why it
alone is drawn. Flashlight is not shown at all; that was not looked into (this port drives no torch).

### Where they die (certain)

The kernel's CRASH lines carry the user registers, a frame-pointer backtrace and the shared cache's slide (see "Crash
reports" below). Symbolicated against the restore image's cache with `tools/ios27/crashlog-symbolicate.py`, all twelve
crashes of a boot have the same stack:

    libswiftCore    _assertionFailure                        (Swift's fatal error: SIGTRAP)
    WidgetKit       ControlAction.init(_: AppIntent)
    WidgetKit       ControlWidgetButton.init(action:label:)  (the OpenIntent variant for Calculator and Code Scanner,
                                                             the AppIntent variant for Timer and Camera)
    the extension   its control's body                       (offset 0x1f40 in CalculatorWidget, 0x21d4 in LauncherControlExtension)
    WidgetKit, libswift_Concurrency

So each extension starts, builds its control, and WidgetKit stops the process while turning the control's intent into the
action Control Centre would run. The extension never hands Control Centre a control, and Control Centre draws an empty
platter. This is not a missing glyph: the symbol is never asked for.

### What the fatal error says (likely)

In `ControlAction.init`, WidgetKit makes an `INAppIntent` (Intents) from the App Intent and asks ChronoServices for a
`CHSIntentReference` of it (`+[CHSIntentReference referenceFromIntent:error:]`); when that call fails, the message of the
fatal error is a fixed prefix, the intent's type name, and the error. The CRASH line's `x0` points at the message: its
first eight characters and its length show. The lengths are 73, 70 and 67 characters for Calculator, Timer and Code
Scanner, which is exactly the prefix, `LaunchCalculatorOpenIntent` / `StartTimerControlIntent` / `BarcodeScannerIntent`,
": " and `nilError` (Swift's name for an Objective-C call that returned nil without an error), and 62 for the Camera
(a 15-character intent name). So the reference came back nil with no error at all. To be certain, read the whole message
(Boot 0).

### Why it is nil (likely; not observed)

Reading the code of the 24A446 cache (names and structure only):

1. `-[INAppIntent initWithAppBundleIdentifier:linkAction:linkActionMetadata:]`, which WidgetKit calls, asks the Shortcuts
   data store for the intent's serialized parameters (`VCVoiceShortcutClient`, `synchronousRemoteDataStoreWithErrorHandler:`
   then `getSerializedParametersForLinkAction:actionMetadata:completion:`), a synchronous XPC call to the Mach service
   `com.apple.siri.VoiceShortcuts.xpc`. When it cannot get them it returns **nil** (it also returns nil when no Shortcuts
   client is set up in the process).
2. With a nil intent, ChronoServices' reference is nil and no error is set, which is the `nilError` above.
3. `com.apple.siri.VoiceShortcuts.xpc` is served by `siriactionsd`
   (`/System/Library/PrivateFrameworks/VoiceShortcuts.framework/Support/siriactionsd`), and the root build removes that job:
   `tools/ios27/fullroot/lists/daemons-bp.keys` (the default `DAEMON_SET=bp`) has `com.apple.siriactionsd.plist`, and
   `tools/ios27/starlte-disabled.list` has `com.apple.siriactionsd` in the S7 port's "only waste CPU" group. On phone 1's
   root it is in `launchd-disabled.plist`, not in `launchd.plist`. A lookup of a service no job provides fails at once.

Every App Intent control goes through step 1, which is why all four die the same way while nothing else in Control Centre
does. Stock iOS has `siriactionsd`.

`linkd` (App Intents' metadata daemon) and `siriappintentsd` are removed too; the path above does not call them, but if
the controls still die with `siriactionsd` on, they are next (Boot A's second half).

### What it is not

- **chronod's abort.** `chronod` (which hosts the controls' side of Control Centre) dies once, about 30 s before the
  controls, in QuartzCore's `query_displays` under `+[UIScreen initialize]` (`abort()`: asked for the displays before the
  render server answers; a start-up race already noted in UI-NIGHT-1114.md). launchd starts it again (`KeepAlive`
  `Crashed`). The controls die in their own processes with their own stack. Certain.
- **MobileGestalt, the camera or the torch.** The controls die building their action, before any hardware question; the
  Calculator and Timer controls have no hardware to ask about. Likely.
- **The icon steps and `sweetspeed` bit 268435456.** Phone 1 ran with the bit set; the controls never ask for a symbol.
  Whether their symbols draw once they live is not known: Boot A's Control Centre picture says.

## Airplane mode shown on

`/private/var/preferences/SystemConfiguration/com.apple.radios.plist` on phone 1's root holds `AirplaneMode` = true. The
file is dated 1 January 1970: it was written on the phone by iOS at a boot without the time set (the root build writes no
such file; a root that has never booted has none). Control Centre shows that preference, so the switch is not lying:
airplane mode is on. iOS lets Wi-Fi be switched on in airplane mode, which is why Wi-Fi works. What set it is not known: a
touch on the switch in an earlier session, or iOS itself (the phone-call services, `CommCenter*`, are removed by
`daemons-hw.list` and `daemons-bp.keys`). Certain that the preference is on; its origin unknown. Boot 0 switches it off
and checks that it stays off.

## Books' and Podcasts' widgets

Still dying in dyld on 17 November: their exit reason is "Library not loaded" for `@rpath/...` frameworks of their own.
The kernel's fix is in the fork (`bsd/kern/sweet_amfi.c`, `sweetatpath=1`: the AMFI stand-in's dyld-policy answer gets
the allow-`@`-path bit that AMFI gives an ordinary signed process), but it is off, not in the kernel's built-in line
(`osfmk/arm/arm_init.c`, `starlte_default_bootargs`) and not on the line phone 1 boots. Boot E tests it.

## SelectPerson_iOS

Contacts' person picker extension aborts once a boot from an uncaught exception (`objc_terminate` -> `abort`); the
exception's reason is not in the log. Boot 0's heap scan reads it. Harmless as far as is known.

## Crash reports

### Why there are none (certain)

- The root build removes `com.apple.ReportCrash` (its Mach service `com.apple.ReportCrash` is an `ExceptionServer`: launchd
  makes it the receiver of crash exceptions) and `com.apple.osanalytics.osanalyticshelper`: both are in `daemons-bp.keys`,
  and `com.apple.ReportCrash` is on `daemons-hogs.list` too. With no exception server, a crash goes nowhere and no `.ips`
  is written. Nothing in the kernel stops it: corpses are made unless the boot line has `-no_corpses`
  (`osfmk/corpses/corpse.c`), and no line of the project has it.
- `diagnosticd`, `spindump` and `crashreportcopymobile` are on; `logd` is removed (so iOS's own log is not kept either).
- Whether ReportCrash works once it is back has not been tried: it runs as `mobile` and needs to read the dead task, which
  the AMFI and sandbox stand-ins must allow, and the CrashReporter directory, which exists.

### What a developer has today (always on)

The kernel prints, for every user process that dies of a signal (`bsd/kern/kern_exit.c` -> `sweet_crash_log` in
`osfmk/kern/exception.c`, from ios-sweet):

    CRASH name[pid] sig S code C sub X slide SLIDE mh MH   (signal, Mach exception, shared cache slide, main binary's header)
    CRASH name pc .. lr .. sp .. fp .. far ..
    CRASH name bt ...                                     (up to 10 frame-pointer return addresses)
    CRASH name x0: ..  ... x28                            (registers)
    CRASH name x0 ... / x19 ...                           (which mapping they point into, and 32 bytes there)
    CRASH name reason ns N code C: TEXT                   (the exit reason: dyld's and abort_with_payload's messages)

`sweetcrashterse=1` cuts that to one line per crash; `sweetscan=NAME,...` (default `backboardd,SpringBoard`) adds, for a
named process that dies of SIGABRT or SIGTRAP, the readable text in its heap (an uncaught exception's reason, a Swift
fatal error's message). That is enough to symbolicate anything in the shared cache or the main binary offline:

    python3 tools/ios27/crashlog-symbolicate.py CONSOLE.log --dsc .../dyld_shared_cache_arm64e --ipsw ipsw [--only NAME] [--first 1]

Use the cache of the restore image's system cryptex (a mount of your own): the root's copy has no `.symbols` file, which
`ipsw` needs. The script prints each frame as `dsc 0x<unslid>` with its image and symbol, or as an offset in the
process's own binary.

### What was added

| | What | Costs | Default |
|---|---|---|---|
| `tools/ios27/crashlog-symbolicate.py` | the console's CRASH lines symbolicated offline | nothing on the phone | - |
| `sweetcrashimg=N` (xnu fork) | for the first N crashes of a boot (at most 1000), one line per pc, lr and backtrace address: `dsc 0x<unslid>`, or the mapped file's path and the offset in it, or anonymous / not mapped. For frames in an app's own frameworks and libraries outside the cache, which the lines above cannot place. Addresses, names, offsets: no memory contents | a map lookup and a path lookup per address at crash time; 12 short lines a crash | off |
| `CRASH_REPORTS=1` (root build, `20-launchd.sh`) | ReportCrash and osanalyticshelper left in `launchd.plist`: iOS's own `.ips` reports, with every image's UUID and load address | nothing at idle (both start on demand); per crash a ReportCrash run reading the dead task and writing a report to USERDATA. With Wi-Fi up, `atc` and `locationd` crash about 13 times a boot each, and ReportCrash was measured as a CPU user on this hardware (`daemons-hogs.list`) | off |

Choice: the kernel's lines stay the record (free, always there, already proven); `sweetcrashimg=` fills their one gap and
stays off until a boot shows its lines; ReportCrash is the stock path and the better report, but it is off until Boot C
shows it works here and what it costs in a boot with Wi-Fi's crash loops.

## The change to the root build

`tools/ios27/fullroot/steps.d/20-launchd.sh` and `edit-launchd.py` keep jobs on that the job set would remove:
`CC_CONTROLS=1` (`com.apple.siriactionsd`), `CRASH_REPORTS=1` (`com.apple.ReportCrash`,
`com.apple.osanalytics.osanalyticshelper`), `KEEP_JOBS="LABEL ..."` (any job, by exact label or job key). All are off;
with none of them the step does what it did. The step is declarative, so running it again without them is the undo.
Checked on a copy of phone 1's root, as the user (with `chown` stubbed): the default run leaves the three plists' contents
as they were (612 jobs on, 116 off); `CC_CONTROLS=1 CRASH_REPORTS=1` gives 615 on, 113 off, the three jobs on; a second run
changes nothing; a run without them gives back the original contents; a `KEEP_JOBS` name no job has is said. Not run
under `build-full-root.sh` or `apply-step.sh` (both need root), not booted.

What turning `siriactionsd` on costs at idle is not measured. It has no `RunAtLoad` and no `KeepAlive`: launchd starts it
when a client looks it up (here: the four controls at boot, and Control Centre's actions) or on its launch events (an app
installed, app-protection preference changes, and background tasks that `dasd`, also removed, would schedule), and it
may exit when idle (`EnablePressuredExit`). It was removed with the S7 port's list of daemons that "only waste CPU" with no
network, iCloud or Siri, not by a measurement on the S9. Boot A's `STARLTE TOP` lines measure it.

## Boots (for the phone agent)

One boot each, from TWRP, with the boot line the phone ran on 17 November (today's kernel, the 17 November defaults as
that session had them) plus what each line says. Root changes are made on a clone of the phone's current root with
`sudo tools/ios27/fullroot/apply-step.sh ROOT.img tools/ios27/fullroot/steps.d/20-launchd.sh VAR=1` and pushed the way the
session pushes a root; undo is the same command without the variable. After each boot, `crashes.sh` on the log, and
`crashlog-symbolicate.py` for anything new.

**Boot 0: the message, and airplane mode.** Root unchanged. Add
`sweetscan=CalculatorWidget,WorldClockWidget,LauncherControlE,BarcodeScannerWi,SelectPerson_iOS` (it replaces the
default `backboardd,SpringBoard`: keep those out for a short log). Read the `STR` lines after each of these processes'
CRASH lines.
- The controls' lines contain WidgetKit's message with the intent's name and `nilError`: the reading above holds; go on
  to Boot A. Another error: write it down; it names the failing piece (and Boot A may still be right).
- SelectPerson_iOS: the exception's reason.
- Then open Control Centre (`cc.sh`), tap Airplane Mode once (`con.sh 'touch X Y X Y 80'` at its centre as the picture
  shows), take a picture, return to TWRP, boot again with the same line, picture again, and pull `com.apple.radios.plist`
  from the root (or read it with the root pulled). Off and stays off: the stored preference was the whole story. Turns
  itself back on: something writes it at boot (look for `CommCenter` in the log; write down what does).

**Boot A: `siriactionsd` (the controls).** Root with `CC_CONTROLS=1`. Same line as Boot 0.
- The four extensions do not crash: cause confirmed. Then `cc.sh`: the four controls drawn = fixed; platters still blank
  while the extensions live = a separate symbol problem (read the CRASH lines of `chronod` and SpringBoard, and
  try the line with and without `sweetspeed` bit 268435456).
- They still crash with the same message: the nil comes from elsewhere. Boot A2: root with
  `KEEP_JOBS="com.apple.siriactionsd com.apple.linkd com.apple.siriappintentsd"`. Fixed there: one of the two others is
  needed (try each alone).
- They crash with a different message: `siriactionsd` answered with an error: the message names it.
- `siriactionsd` itself crashes (CRASH lines with its name): symbolicate them; that is the next problem.
- CPU: the `STARLTE TOP` lines over five idle minutes after the home screen; `siriactionsd` in them at more than a few
  percent is the cost of turning it on. Write the numbers into this page.

**Boot C: iOS's crash reports.** Root with `CRASH_REPORTS=1` (and `CC_CONTROLS=1` if Boot A fixed the controls), Wi-Fi on
as usual. Wait three minutes after the home screen, return to TWRP, pull the root and list
`/private/var/mobile/Library/Logs/CrashReporter` (do not copy the reports out of the private folder: they carry the
phone's identity).
- `.ips` files for the processes the console shows crashing: crash reports work; note how long ReportCrash ran
  (`STARLTE TOP`) and how many reports a boot makes.
- No files and CRASH lines of `ReportCrash`: it starts and dies; symbolicate.
- No files and `ReportCrash` never started (no line of it at all, also not in `STARLTE TOP`): the exception is not
  reaching it. Then the kernel side is next (exception ports and corpse delivery on this port).

**Boot D: `sweetcrashimg=` (needs the kernel with the fork's change).** Root unchanged. Add `sweetcrashimg=40`.
- After the controls' and Books' CRASH lines, `CRASH <name> img ...` lines: `pc`, `lr` and the `bt` frames in the cache
  as `dsc 0x...` equal to the address minus the slide, and the extension's own frame as its binary's path with an offset
  (e.g. `.../CalculatorWidget.appex/CalculatorWidget +0x1f40`). Then the switch works.
- At most 40 crashes get `img` lines (count them over a boot with Wi-Fi's crash loops). No `img` lines at all with the
  argument set: the change is not in this kernel.

**Boot E: `sweetatpath=1` (Books and Podcasts).** Root unchanged. Add `sweetatpath=1`.
- `BooksWidgetExten` and `PodcastsWidget` no longer in `crashes.sh`'s count, or crash later with another reason: the dyld
  refusal is fixed by the switch; then consider it for the kernel's built-in line (it gives back what AMFI answers on a
  stock phone).
- Still "Library not loaded" with the `@rpath` refusal: the answer is not reaching dyld. The dyld-policy answer is call
  90 of `bsd/kern/sweet_amfi.c` (the console says `SWEET AMFI: launchd op 90` once a boot when it is used); check that
  the bit is set in what that call returns on this kernel.
- Boots D and E change different processes and can be one boot if a boot is short.

## What remains

- The Camera control's intent name, and whether `linkd` is also needed: Boot A.
- Why Flashlight is not offered at all.
- Whether the four symbols draw once the extensions live.
- What set airplane mode on.
- Whether ReportCrash works under the AMFI and sandbox stand-ins: Boot C.
- `chronod`'s start-up abort (asking for displays too early) is still there; it costs one restart and has not been seen to
  matter.
