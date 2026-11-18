# Settings on iOS 27.2.1 (24B156): the abort in MusicLibrary

Phone 1 moved from iOS 27.0.1 (24A446) to 27.2.1 (24B156) on 18 November 2026. That afternoon Settings (`Preferences`)
aborted with signal 6 in three boots, each time with the same ten frames: boot images c272m at 14:43 and 15:39 (camera test
arguments) and s0 at 15:48 (no camera arguments), about 9 to 40 seconds after Settings was opened. No boot on 24A446 (197
console logs) shows a Settings abort.

## The exception

The console's `CRASH Preferences` lines, symbolicated against 24B156's shared cache (names and logic only; the slide is the
`slide` of the `CRASH` line):

| frame | symbol |
|---|---|
| pc | `__pthread_kill` + 8 (libsystem_kernel) |
| 0 | `abort` + 148 (libsystem_c) |
| 1-2, 4 | libc++abi's terminate path (`__terminate`; x4 holds `"terminating due to %s exception of type %s"`) |
| 3 | `_objc_terminate` + 156 (libobjc) |
| 5-6 | `objc_exception_rethrow` + 44 (libobjc) |
| 7 | `-[ML3MusicLibrary databaseConnectionAllowingWrites:withBlock:]` + 160 (MusicLibrary) |
| 8 | `-[ML3MusicLibrary valueForDatabaseProperty:]` + 192 |
| 9 | `-[ML3MusicLibrary libraryUID]` + 60 |

x22 holds `CLNGC++\0`: an Objective-C exception, uncaught, rethrown by the `@finally` of
`databaseConnectionAllowingWrites:withBlock:` (which checks the connection back in and rethrows). The console keeps ten
frames only, so the Settings code that asks for `libraryUID` is not in the log.

What MusicLibrary raises there (24B156's MusicLibrary, read from its code): when a connection cannot be opened,
`-[ML3DatabaseConnection _raiseConnectionClosedException]` first calls `ML3DatabaseConnectionCheckDatabasePathAndRaiseIfNecessary`,
which checks, in order, the media container (`/var/mobile/Media`), the user-hash container, `iTunes_Control`
(`ML3DatabaseConnectionCheckPathiTunesControl`: it must exist, have attributes, not be owned by root and be owned by the caller's
user), `iTunes_Control/iTunes`, then the database file, and raises for the first that fails
(`ML3ExceptioniTunesControlDoesNotExist`: an `NSException` whose reason is built from `"%@ does not exist."` with the path).

`/private/var/mobile/Media/iTunes_Control` does not exist on the 24B156 root on phone 1 (root v9) nor on the last 24A446 root
(c30), both checked read-only. A restore makes it on an iPhone; this port never restores, so no iOS 27 root has had it.
`medialibraryd` is also not in the root's launchd jobs (`tools/ios27/speed/trim-s1.list`, one of the 86 speed trims).

The same ten frames are `atc`'s (AirTrafficControl) abort, 8 to 17 times a boot on 24A446 and on 24B156: the cause noted on
13-14 November in `UI-NIGHT-1114.md`, whose fix (`tools/ios27/ui/70-ui-stability.sh`) was never part of the full-root build.

Not yet captured on the phone: the exception's name and reason as printed (the console has no line for it in Preferences).
The order of MusicLibrary's checks and the root's contents make `iTunes_Control ... does not exist.` the one that fires; the
test boot below settles it.

Reproduced on the owner image (o272, no camera arguments, root v9) at 16:07 on 18 November: Settings opened and scrolled
without fault; **tapping General > About aborted it one second later** with the same ten frames. About lists the number of
songs and videos, which is where Settings asks MusicLibrary. Reopened, Settings ran on for over a minute. In the same boot,
atc aborted 14 times. On that boot Settings > Bluetooth drew its header and the Bluetooth switch (on), but no Devices section.

## What changed between 24A446 and 24B156

The root side did not change: the directory was missing and `medialibraryd` trimmed on both. The set of images that link
MusicLibrary is the same in both caches (`ipsw dyld imports`), and MusicLibrary's checks are the same. So the difference is in
when 24B156's Settings reaches `-[ML3MusicLibrary libraryUID]` (24A446's Settings evidently did not, in the panes used); which
pane or start-up path asks for it is not known (the backtrace is cut at ten frames). On 24B156 the abort came 9 to 40 seconds
after Settings opened, with or without taps in between, and once 7 seconds after a home swipe.

Ruled out: the camera work (the s0 boot had no camera arguments and the camera library is inserted only into cameracaptured and
Camera); a memory fault (signal 6 from `abort()`, `far` 0); MobileGestalt, IOKit, Bluetooth, Wi-Fi or a sandbox refusal (nothing
on the path asks for them; the raise is MusicLibrary's own path check).

## The change

`tools/ios27/fullroot/steps.d/45-media-library.sh` makes `iTunes_Control`, `iTunes_Control/iTunes` and `iTunes_Control/Music`,
mode 0755, owned by mobile (501), in every full-root build. It is on by default because it corrects a directory every iPhone
has; `ENABLE_MEDIA_LIBRARY_DIRS=0` turns it off, and `45-media-library.sh ROOT undo` takes the directories away when they are
empty. A writer connection (atc, for one) then creates the database itself (`-[ML3DatabaseConnection
_createDatabaseDirectoryIfNonexistent]`, `_createDatabaseFileIfNonexistent`). `check-root.sh` says whether the directory is
there. For a root already on the phone, the step can be run on its mounted image.

**Not run on a phone yet.** The test: the same root with the step, owner image o272, Settings opened, scrolled, **General >
About** (the tap that aborts it), Bluetooth, a home swipe and Settings again; expected 0 `CRASH Preferences` and 0 `CRASH atc`, and
`iTunes_Control/iTunes/MediaLibrary.sqlitedb` made by the phone. If Settings still aborts with the same frames, the next check
in the order above (the database file, which only `medialibraryd` or a writer makes) is the one to look at, and putting
`com.apple.medialibraryd` back (`edit-launchd.py --keep`) is the next test.

## Also seen on 24B156, separate

- About 30 to 70 app-extension aborts a boot (SettingsImportExtension, the Spotlight index extensions of Music, TV, Books,
  Home, Tips, SupportFlow, Contacts): their frames are `objc_exception_throw` from PlugInKit's `checkEnvironment:` + 344, a
  different exception from the one above (PlugInKit refuses the extension's environment after a failed system call). Not
  investigated further; they do not take Settings down.
- Settings > Bluetooth opens as a blank white page on 24B156 (it drew on 24A446). No Settings abort came with it (boot o272,
  13:35). Not investigated.
