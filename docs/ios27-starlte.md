# iOS 27 on the Galaxy S9 (starlte): what it would take

> Written 8 November 2026. Nothing here has run on a phone. The facts below were read from an iOS 27 restore image on the build
> machine; nothing of that image is in this repository, and none may be.

## Verdict

**Not bootable from Scion today, and no test image can be made yet.** Three things stand between the S9 and iOS 27, each on
its own:

1. **The kernel.** Apple has published no XNU source for iOS 27. The newest tag in apple-oss-distributions/xnu is
   `xnu-12377.121.6` (iOS 26 / macOS 26.5), the base of the `ios-26` profile; iOS 27.0.1's own kernel is
   `xnu-13432.2.10` (Darwin 27.0.0). So iOS 27 here means iOS 27's user space on xnu-12377 with Darwin 27's additions put
   back. That base builds today only for Apple's virtual machine (`VMAPPLE`, `docs/two-bases.md`); none of the S9's drivers,
   and none of the Exynos 9810 machine layer, is on it yet (`drivers/*/conf`: only `hakios.version.c` names `xnu-12377`).
2. **The CPUs.** From iOS 17 on, every iPhone user-space binary is arm64e only. The S9's Cortex-A55 (ARMv8.2) and Mongoose
   M3 (ARMv8.0) cores have no pointer authentication: the hint-space forms (`PACIASP`, `AUTIASP`, `XPACLRI` ...) run as
   NOPs, but `RETAA`/`RETAB`, `BRAA`/`BLRAA` and friends, `LDRAA`/`LDRAB`, the register forms of `PAC*`/`AUT*` and `PACGA`
   are UNDEFINED and trap. A sample of iOS 27 code (the arm64e Mach-O files of the restore ramdisk) has them in the hundreds
   of thousands, beside v8.3 `FJCVTZS`/`FCADD` and `LDAPR` (RCpc, which the M3 lacks too), and LSE atomics, which the M3
   lacks. Scion has no answer for that user space yet: any answer changes Apple's code inside the builder's own root image,
   which is a separate decision from anything in this repository.
3. **The builder's inputs.** The root comes from the builder's own IPSW. Its system and system-cryptex disk images are
   AEA-encrypted; decrypting them, and building a root file system from them (which needs root on the Mac), is the builder's
   own step.

## What iOS 27 asks of the kernel (measured)

The iOS 27.0.1 kernelcache for the iPhone 11 (`RELEASE_ARM64_T8030`) was read with `ipsw` and compared with
`xnu-12377.121.6`:

- **System calls.** Both tables end at 557. Darwin 27 fills eight numbers that are `nosys` in xnu-12377: `pipe2` (148),
  `dup3` (149), `fchflagsat` (214), `getumask` (215), `aio_readv` (246), `aio_writev` (247), `fsctlat` (249) and
  `guarded_ftruncate_np` (352). iOS 27's `libsystem_kernel` has a wrapper for each; `pipe2` returns its two descriptors the
  way `pipe` does (in the two return registers, which the wrapper stores). No number 12377 implements was taken away
  (the entries that are absent from the iOS kernel are compiled out on iOS in 12377 too: System V IPC, audit, NFS).
- **Mach interfaces (MIG).** The 17 subsystems the iOS 27 kernel serves all exist in 12377's `.defs`, at the same base
  numbers, and none has a routine number above 12377's range. Changes in a routine's arguments within a range are not
  checked here.
- **Kernel extensions.** The kernelcache links 260; Scion uses none of Apple's. What iOS 27's daemons ask of AMFI,
  AppleImage4, IOHIDFamily, IOSurface and the GPU family, beyond what iOS 26 asked, is only learned by booting.

`patches/xnu-12377/0120-darwin27-syscalls.patch` adds the eight system calls to the xnu-12377 base, as Scion's own code
(APSL, like the files it changes): `pipe2` (O_CLOEXEC, O_CLOFORK, O_NONBLOCK), `dup3` (O_CLOEXEC, O_CLOFORK; the same
descriptor twice is EINVAL), `fchflagsat` and `fsctlat` (relative to a directory descriptor), `getumask`, and
`guarded_ftruncate_np` (`ftruncate` checked against the descriptor's guard, as `guarded_pwrite_np` is). `aio_readv` and
`aio_writev` answer ENOTSUP, so a Darwin 27 library gets an error rather than SIGSYS. UniXNU carries the same eight calls
(its `darwin26-syscalls.patch`); its code is not used here.

## Order of work

1. The Exynos 9810 machine layer on xnu-12377: a board configuration beside `VMAPPLE`, plain arm64 (no PAC keys to manage),
   4 KiB kernel pages under 16 KiB user pages as the 8796 S9 kernel has them.
2. The S9's drivers on xnu-12377, one at a time, each with `bases xnu-8796 xnu-12377` once it builds (UFS, the log, DWC3
   first).
3. A user space the S9's cores can run (point 2 above): a decision for the owner first.
4. The AMFI and image4 stand-ins that `docs/two-bases.md` names as the next stop on QEMU.

The S9's iOS 16.5 kernel is unaffected by all of this: 0120 is in the xnu-12377 series only.
