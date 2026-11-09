# iOS 27 on all eight cores of the Galaxy S9: the LSE rewrite and the soft A55 pins

Written 8 November 2026. The kernel side (the M3 bring-up and the trap emulation) is the Starbite port's and is in every iOS 27 kernel
this repository builds; what this page adds is Scion's: a root-side rewrite that takes the LSE atomics out of the trap path, and an
optional change to how a thread is pinned to a Cortex-A55. The rewrite was run on a phone's root; the soft pins were built and not booted.

## What the M3 lacks

The Exynos 9810 has four Cortex-A55 (ARMv8.2) and four Mongoose M3 (MIDR `0x531f0020`, ARMv8.0). Against the A55 the M3 lacks LSE, LRCPC,
the RDM (`SQRDMLAH`), DotProd and FP16 arithmetic extensions, DPB and PAN. iOS 27's shared cache (arm64e, 901 million instructions
counted by `tools/ios27/lse/lse-rewrite.py census`) has about 928 thousand LSE sites (`LDADD` 868 k, `CAS` 47 k, `SWP` 8 k, `CASP` 2.7 k, the
rest 2.7 k), 163 thousand `LDAPR` (already `LDAR` after `tools/ios27/a64e/a64e-rewrite.py`), about 42 thousand FP16, a thousand DotProd and 185
RDM; no `DC CVAP`.

`starlte/0304` powers the M3s on (PSCI `CPU_ON` `0x100+n`), `0342`-`0344` emulate for EL0 the LSE and LDAPR instructions they lack, and bind
a thread that runs any other instruction an M3 lacks (FP16, DotProd, RDM) to an A55 (`tid % 4`) and retry it. The iOS 27 device tree
already lists `cpu4`-`cpu7`, so `cpus=8` (the default of `tools/ios27/cmdline/starlte-8core.txt`) is the only switch. The emulation is correct
and costs a trap per atomic: on the phone, under load, about 9 million `CAS` and a million `LD<op>` traps in ten seconds, some 25 thousand a
second at idle (`startop=1` prints them).

## The rewrite: `tools/ios27/lse/lse-rewrite.py`

An ahead-of-time rewrite of the shared cache, so most atomics never trap. A site (an LSE instruction in a pure-instruction section) becomes a
`B` to a 16-byte thunk in the zero padding after its image's `__TEXT` (9.86 MB in all; the hot images are placed first so the crowded first
subcache keeps room):

    STR x30, [sp, #-144]!     BL body     LDR x30, [sp], #144     B site+4

`body` is shared by every site with the same encoding (6835 for this cache). It reads `TPIDR_EL0`, where XNU keeps the CPU number in bits 0-11:
on CPUs 0-3 (the A55s) it executes the original LSE instruction; on CPUs 4-7 (the M3s) an `LDXR`/`STXR` loop with the original's ordering
(acquire: `LDAXR`; release: `STLXR`; both: both plus `DMB ISH`; a `CASP` whose compare fails stores the pair back, so the pair it returns is
an atomic snapshot). A thread migrated between the test and the instruction traps once into the kernel's emulation. Every register but the
instruction's destinations, and NZCV, is preserved; scratch registers are saved below the 128-byte red zone.

Coverage on the iOS 27.0.1 cache after `a64e-rewrite.py` (libcorecrypto is skipped: its FIPS self-check hashes its own code): 390,229 sites in
every image but 16 cold ones (WebCore, WebKit, JavaScriptCore, WebKitLegacy, Recon3D, AppleCV3D, usd, VisualMappingKit, VectorKit, SLOM, Espresso,
VisualLogger, AMSKitInternal, ANECompiler, GeoServices, AGXMetalA13: 538 k sites that keep trapping when they run, because their padding is too
small). All of libsystem_*, libdispatch, libobjc, libc++, CoreFoundation, Foundation, the Swift runtime, QuartzCore, UIKitCore, CoreGraphics, the
SpringBoard and BackBoard family, dyld and Network are covered. The 112 daemon executables (10.6 k sites) are not: they are outside the cache and
would need re-signing; they trap.

    PY=<python3 with numpy> tools/ios27/lse/apply-lse-root.sh /Volumes/<root>          rewrite, then verify (writes a log)
    PY=... tools/ios27/lse/restore-lse-root.sh /Volumes/<root> LOG                      put every original word back

Run it on a root that has already been through `a64e-rewrite.py` (the arm64e translation, a step of the root build), attached so
that the cache is writable. Running it twice is harmless. `lse-rewrite.py verify` re-decodes every site, thunk and body against the log.

**What was verified.** On an Apple M1 (it has both LSE and LL/SC, so a program runs natively and rewritten): `tools/ios27/lse/test/run.sh` runs 1520
single-instruction cases (every operation, B/H/W/X, the four orderings, scratch-register clashes, `Rs`/`Rt` = ZR, `Rn` = SP, `Rs` = `Rt`) six
times each with random registers, flags and memory, and the rewritten binary printed the same registers, NZCV, SP and memory as the original
(the cases whose `Rs` is `Rn` differ only in ASLR'd pointer bits); then eight threads stress `LDADDAL` (16 M), `CASAL` (16 M), `CASPAL` (a 128-bit
counter, 4 M), an `SWPA` spin lock (4 M) and `LDSMAXAL`, exact in both (with the dual body, A55-style native and M3-style LL/SC run on the same
words at once). On the cache: `verify` found 390,229 sites ok and none bad, and `restore` brought the instruction counts back to the census.
On the S9 (8 November, `cpus=8`, a root rewritten this way): the `STARLTE LSE (M3)` line fell from about 9-23 M `CAS` and 1-6 M `LD<op>` per ten
seconds to 0-19 k and 0.2-75 k; the hot trap sites that remained were one daemon's own executable and a single cold cache site; no crash with
a PC in the thunk padding (the crashes seen were the ones of the 4-core runs). **Not verified:** the Exynos memory ordering of LL/SC loops on the
M3 beyond that boot, loops on Normal-NC memory (an LL/SC loop on such a page could spin where LSE took an abort), and the frame rate under load.
If a process sits at 100 % in `STARLTE TOP`, boot `cpus=4` on the same root: the rewrite then runs on the A55s only, which separates a wrong
rewrite from a wrong M3.

## Soft A55 pins: `sweetm3unpin=N` (`starlte/0425`)

A thread that runs FP16, DotProd or RDM on an M3 is bound to an A55 for the rest of its life. With `sweetm3unpin=N` (milliseconds, default 0 =
off = permanent pins, as before) the binding is dropped at the thread's first synchronous exception from EL0 (a system call, a fault, a trap) at
least N ms later, so it can use the big cores again; if it meets such an instruction on an M3 once more it is pinned again, one trap later. The
bookkeeping is a 256-slot table in `scion/xnu-12377-starlte:osfmk/arm64/sweet_a64e.c` (entries carry the thread id, so a recycled `thread_t` is never unbound; pins
that do not fit stay permanent); `0425` is the two hooks in Apple's `sleh.c`; the `STARLTE LSE` line ends `(M all, R released)`. Built and not
booted. Use it only if the line shows pins piling up (`N threads pinned to A55`): 100 is the value it was prepared with.

## Not done

- XNU's asymmetric scheduling (`__AMP__`) is compiled only for Apple SoCs: the STARLTE kernel schedules the eight cores symmetrically, and the
  device tree still says one cluster type. An A55 pin is a hard bind to one A55.
- The daemon executables and the sixteen cold images (above): they need re-signing with their entitlements, or an extra mapping in the shared region
  (a kernel and dyld change).
- Eight cores with the CPU governor (`cpufreq_gov=1`, patch `0421`): not stacked yet.
