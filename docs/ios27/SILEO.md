# iOS 27 on the S9: Sileo and the package manager's prefix

The 16.5 line has had this by default for a while (`tools/mk-jb.sh`, `userland/src/jb_seed.c`, [apps](../apps.md)). The
iOS 27 root did not: `tools/ios27/fullroot/` had no step for it, so a full root had no `/var/jb`, no Sileo and nothing in
`/bin` but `df` and `ps`. It was never installed; nothing crashed and nothing wiped it. This page is the step that puts
it there, what it leaves on the phone, how it is kept, and what "secure user data" means on this port today.

"Jailbreak" is the word people use for this; here it only means the rootless prefix `/var/jb` with the
[Procursus](https://github.com/ProcursusTeam/Procursus) bootstrap (dpkg, apt, a shell and the usual tools) and
[Sileo](https://github.com/Sileo/Sileo) on a system whose kernel is ours. Nothing is broken into.

## What the build does

    tools/ios27/sileo/prepare.sh                  # no root; once; also run by tools/ios27/s9/prepare-root-inputs.sh
    sudo tools/ios27/s9/build-root.sh inputs      # steps.d/60-sileo.sh is one of its steps; ENABLE_SILEO=0 leaves it out

`prepare.sh` fetches two files from `apt.procurs.us`, each pinned by SHA-256 in `tools/mk-jb.sh` (the bootstrap for
`iphoneos-arm64-rootless/1800`, and `org.coolstar.sileo` 2.5.1), unpacks them into `jb/` of the checkout (never in git) and
builds two small programs of ours for iOS. A changed file upstream stops the build. It is the installer's flavour of
`mk-jb.sh` (`PREINSTALL= ELLEKIT=`): the bootstrap and Sileo and nothing else.

`steps.d/60-sileo.sh ROOT` then puts on the root:

| What | Why |
|---|---|
| `/jb` | The prefix as the image carries it. Never run from. `jb_seed` copies it to `/private/var/jb` at the first boot; after that the copy is the phone's own and a later root does not touch it, unless the stamp in `/jb/.scion-stamp` differs: then the old copy is moved aside (`/private/var/jb.old-<time>`) and the new one seeded |
| `/usr/local/bin/jb_seed` | `userland/src/jb_seed.c`, the same source as on 16.5. Once a boot: seeds if needed, then `/var/jb/prep.sh` (dpkg's `firmware` record, `pwd_mkdb`, Sileo and every app in `/var/jb/Applications` registered with `uicache`) |
| `/usr/local/bin/var_sync` | `userland/src/var_sync.c`: `sync()` and `F_FULLFSYNC` every 5 seconds. The phone is reset, never shut down, so what was only in memory at that moment would be gone |
| `/bin/chmod`, `cp`, `mkdir`, `mv`, `rm` | Links into the prefix. Sileo runs them by these names as root and iOS has none of them |
| two launchd jobs | `com.scion.jbseed`, `com.scion.varsync`, as entries of `/System/Library/xpc/launchd.plist` (iOS's launchd reads its jobs from that file, not from the directories; `tools/ios27/sileo/jobs.py`) |

The step also changes the prefix before it goes on the root, and refuses to go on if the result is not what it expects:

- the only repository is Procursus. The project's own (`repo/` of scion-installer) is added when it is hosted somewhere
  (`SCION_REPO_URL=https://...` at the build); the 16.5 line's two third-party stores are not listed here;
- the bootstrap ships a password hash for `mobile`: it is replaced by `!`, which matches nothing (`root` has `!` already);
- `usr/sbin/shshd` (set-user-ID, for a jailbreak's kernel access) and its job are deleted;
- it stops if the prefix has OpenSSH or tweak injection in it, a file anyone may write, an account with a password, a
  Mach-O that is not plain arm64, or a set-ID program other than the bootstrap's seven and Sileo's `giveMeRoot`.

**Instructions.** Every Mach-O of the prefix (388 files) is plain `arm64` built for the baseline, the same files the 16.5
line runs on the same cores; none is `arm64e`, so the PAC rewrite (`10-macho-a64e`) has nothing to do there. An LSE
atomic, should a package bring one, is handled where the rest of the system's are: the kernel's emulation on the M3 cores.

**Owners.** Built as root (the full build), everything is `root:wheel` on the image, `var/mobile` is `mobile`'s. A root
edited without root (below) carries the builder's user ID on those files; `jb_seed` is the first thing that runs any of it,
as root, and makes them root's before it does anything else. The copy in `/private/var/jb` is `root:wheel` either way.

**Undo.** `60-sileo.sh ROOT undo` takes the five things off the root again and leaves `launchd.plist` equal to what it
was (checked: the parsed file is identical). The phone's copy `/private/var/jb`, with what was installed into it, stays
unless `PURGE_PREFIX=1`.

## A phone that is installed already

No new root is needed: the step is one more reversible edit of the root that is on the phone, with the tools the other
steps use (the phone in TWRP, the root pulled as the pieces that differ from the image it was flashed from, edited on the
Mac, the changed 64 MiB chunks pushed and read back). `tools/ios27/sileo/README.md` has the commands.

## Apps

A package with an app puts it in `/var/jb/Applications` and its `postinst` runs `uicache -p` (the packages of
scion-installer's `repo/` do); `prep.sh` registers whatever is there at each boot as well. The kernel has no code-signing
policy, so an app needs no signature and no account. What this is for: packages, the project's own apps, open-source
apps built from source ([apps](../apps.md)). What it is not for, here or anywhere in Scion: decrypted or pirated apps,
signature-bypass packages, anything that defeats an app's DRM.

## Keeping it

The root of an S9 is one HFS Plus volume on USERDATA with `/private/var` on it, so the prefix, dpkg's database, apt's
lists, Sileo's sources and an app's data are on the flash like everything else, and a later boot finds them. Two things
make that true in practice: the copy is made under another name and renamed when it is whole (a reset in the middle costs
that copy and no more), and `var_sync` bounds what a reset can lose to the last five seconds.

## Secure user data: what there is today

Said plainly, because the word is bigger than the thing:

| | iOS 27 on the S9 today |
|---|---|
| Encryption at rest | **None.** HFS Plus without content protection; whoever reads USERDATA (TWRP does) reads every file |
| Protection classes | Labels only. A file marked class A is as readable as one marked D (the fork's `SweetKeyStore.cpp` and `sweet_hfs_pclass.c`) |
| Passcode | **Cannot be set.** The key store answers as a device without one; nothing is gated by a passcode |
| Keychain | Works; its keys are not protected at rest |
| Sandbox | **None.** The kernel's stand-in allows every operation: an app is held back by Unix permissions only |
| Code signing | Not enforced. Any Mach-O runs |
| What does hold | Users and modes: apps run as `mobile` (501), the system and the prefix are `root`'s, `mobile` cannot write either |

The 16.5 line is further: a passcode with a real key bag for the keychain ([keystore](../keystore.md),
[passcode](../passcode.md)); files are not encrypted there either. Bringing that to this tree is separate work.

What the package manager adds to that picture, and what was done so that it does not make it worse:

- **A way to root, by design.** Sileo installs packages by running `giveMeRoot`, a set-user-ID program that checks that
  its parent is Sileo. With no sandbox, that check is the whole fence between a program running as `mobile` and root.
  This is what a package manager is on any rootless system; on a port with no sandbox it is worth saying.
- **No password anywhere.** `root` and `mobile` are locked (`!`), so `su`, `sudo` and `login` let nobody in.
- **Nothing listens.** No OpenSSH in the image (the step stops if the prefix has one); the two jobs open no socket. An
  SSH server is an opt-in of the builder's, key only, and not part of this step.
- **Owners and modes** are checked at the build (`check-root.sh`) and can be checked on the phone (the self-test):
  the prefix is root's, nothing in it is writable by everybody, the set-ID list is the expected one.
- **What is fetched** is pinned by SHA-256, from Procursus's own server.
- A repository added in Sileo is trusted like any apt source: its packages run their scripts as root.

## What the phone showed (13 Nov 2026, phone 1)

- **apt and the system cfprefsd.** Procursus' apt runs its download methods as its sandbox user `_apt` (uid 1001). iOS's
  own `/etc/passwd` has no such uid, and the system `cfprefsd` crashes (a `CFEqual` on NULL in `-[CFPrefsDaemon
  handleSourceMessage:...]`) on the first preferences request of a client it cannot name. The method retries, launchd
  restarts `cfprefsd` every ten seconds, and every root process that reads a preference waits for ever: `apt-get update`
  stopped at its first file, `uicache` with it, and a minute later SpringBoard (a synchronous call to the pasteboard) and
  with it the whole interface. The step now writes `APT::Sandbox::User "root";` into the prefix
  (`etc/apt/apt.conf.d/00scion-sandbox`). With it, no cfprefsd crash in the three boots since.
- **LaunchServices.** `uicache -p` answers "Unable to register" for every path on this port, `/Applications/*.app`
  included, and `uicache -l` lists nothing; lsd's own rebuild at boot does register what is in `/Applications`. So the
  step puts a real copy of `Sileo.app` there (marked `.scion-prefix-app`; `prep.sh` makes it root's and `giveMeRoot`
  set-user-ID at every boot, for roots edited without owners) and drops the LaunchServices databases so that lsd rebuilds
  them. An application installed later by a package lands in `/var/jb/Applications` and is not registered: making
  `uicache` work (probably the entitlements lsd wants of it) is the open item.
- **Sileo's first start** listed Havoc and Chariz besides Procursus (its own defaults); on the next start only Procursus
  and the test repository. A flat repository needs a `Release` file for Sileo (apt does without one).
- The plain bootstrap has no `awk`; the self-test uses `grep` and `cut` instead.

## Things a user trips over (night of 13-14 Nov 2026)

Found from the phone's logs and its root image; the fixes are made, but what is marked "not tested" has not run on a phone.

- **An app built against the iOS 27 SDK without scenes dies at launch.** Puzzles (`com.scion.app.puzzles` 1.3.2-scion1) got
  its icon and died with SIGTRAP the moment it was opened. The kernel's crash record has UIKit's message in `x0`:
  "Application failed to launch: UIScene life cycle is required for apps built with this SDK". iOS 27's UIKit asks this of
  every app whose `LC_BUILD_VERSION` says SDK 27 or later. Puzzles had been linked against Xcode's 27.2 SDK instead of the
  recipe's 16.5. This is iOS's rule, not the port's, and it applies to anything a package brings: an app delegate with no
  `UIApplicationSceneManifest` needs a binary that says an older SDK. The recipe now links with
  `-Wl,-platform_version,ios,14.0,16.5` (scion-installer PR 29); 1.3.2-scion2 is that build. On a phone: not tested yet.
- **`dpkg --audit` was not clean (172 lines).** `tools/mk-jb.sh` unpacks Sileo (and ElleKit) by hand and wrote only their
  status entries, so dpkg had no file list for Sileo. It warns "files list file for package 'org.coolstar.sileo' missing"
  at every install, and an upgrade or removal of Sileo would not know its files. The 164 packages that `usr/libexec/firmware`
  makes (`firmware`, `cy+*`, `gsc.*`) have file lists that name no file and no sums file. `tools/ios27/sileo/dpkg-info.py` now
  writes the list, the sums and the scripts from the package itself, and `prep.sh` gives an empty sums file to a list that
  names no file. Checked by running `mk-jb.sh` on the Mac; on a phone: not tested yet.
- **Sileo's lists are apt's.** Sileo 2.5.1 keeps its repository cache in `/var/jb/var/lib/apt/lists`, the same directory
  and the same file names as apt. So after an `apt-get update`, Sileo shows the packages it reads from apt's `Packages` file
  even when its own refresh never finished. The repository is then called "Untitled Repo" (Sileo takes the name from a
  `Release` file, and apt keeps `InRelease`). The phone showed exactly that: Procursus' `Packages` was apt's (dated by the
  server, 27 Jul) and no `Release` of Sileo's. "Untitled Repo" therefore means that Sileo's own refresh failed, not that the
  repository is broken. `sileolists/` is something else: the reduced index Sileo writes for apt's dependency check.
- **Every package install runs `uicache -a`** (uikittools' trigger on `/var/jb/Applications`). On this port that prints
  "Unable to register" once for each of iOS's own applications (about 200 lines), and it is slow. It does no harm.
- **file: repositories cannot work in Sileo.** Sileo's downloader (Evander) treats any answer that is not HTTP as an error
  (status 522), so a repository has to be http(s).

## The self-test

The port has no shell to ask. `SILEO_SELFTEST=1` with the step (a test root, never a release; `check-root.sh` says so)
adds a third job that runs `tools/ios27/sileo/selftest.sh` once a boot and appends to
`/private/var/scion-test/selftest.log`: owners and modes, the set-ID list, dpkg's database, Sileo registered, a counter
and a kept file with one line per boot, the network as it stands. With `SELFTEST_REPO=DIR` (a flat apt repository and a
`test.conf`) the first boot installs a package from it with `apt-get`, runs it, removes it, installs it again and installs
an app; later boots want both still there. The log is read back from the root in TWRP.

## Status

See the table in `tools/ios27/sileo/README.md`: what has run on a phone and what has only been checked on an image.

## Credits and licences

The Procursus bootstrap and its packages: the Procursus Team and each package's authors, under each package's licence
(the bootstrap carries them in `/var/jb/usr/share/doc`). Sileo: the Sileo Team (CoolStar, Amy While and contributors),
under its own licence (`Licenses.plist` in the app). Neither is in this repository; both are fetched from their own server at build time. Kept data volume,
re-seeding and `var_sync`: Anan Jaser's work on the 16.5 line, reused here unchanged. The files of this step are MIT-0.
