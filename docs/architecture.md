<!-- SPDX-License-Identifier: MIT-0 -->
# The kernel's architecture: layers, conventions, and how a file is split

What the kernel's own code (`drivers/`, `socs/`, and the S9 and Starbite files of the fork `Scion-Project/xnu`) is made of,
which part may depend on which, how a driver of several files is laid out in both kernels, and how a file that has grown
too large is split without changing what the kernel does. It is the design a change is held against in review.

Read on 17 November 2026 from `main` at 131697c and the fork's `scion/xnu-12377` at 1532845. Nothing has been moved by this
page. What CI checks from now on is in [What CI enforces](#what-ci-enforces); the files and their proposed splits are in
[architecture-census.md](architecture-census.md). [modular-kernel.md](modular-kernel.md) compares the tree with UniXNU's
layout and is not repeated here.

## Why

Most drivers here grew one bring-up stage at a time in one file. A typical large one holds, in one translation unit,
register definitions, a bus or transport, a parser of what a chip or another processor sends, a state machine of steps
chosen by a boot argument, policy (what may be written, in what order), a device node or sysctl, console commands and
logging. Forty-four files of `drivers/` are over 800 lines; ten are over 3,000 (one of them generated data). The security audit of 17 November found
its bugs at exactly those seams -- parsers that trusted a length because the transport had read it, device nodes open to
everyone because the node was declared next to the hardware code, GPU state shared between a probe and a service -- and
had to write stand-ins for kernel headers before it could run any parser on a computer. A boundary that is a file and a
header makes each of those visible: a parser that cannot include a kernel header can be built and fuzzed on a host, and
a device node declared in the OS-interface file of a driver is where a reviewer looks for its permissions.

What the project does well stays as it is: every fact says where it came from; every step says what it will do before it
does it; new behaviour is behind a boot argument and off by default; catalogues are generated from the source
(`tools/boot-args.py`, `tools/docs-gen.py`, `tools/hakios-config.py`); `drivers/map`, `drivers/*/conf` and
`profiles/ios-27.copy` say where each file goes; provenance and licences are checked; host tests run under sanitizers.

## The layers

Each piece of kernel code belongs to one layer. A layer uses the layers above it in this table, never one below it;
the two exceptions are named.

| # | Layer | What it holds | May include and call | Never | Where it lives |
|---|---|---|---|---|---|
| 0 | **SoC facts** | addresses, interrupt numbers, PMU and CMU offsets, fuse layouts of one SoC; numbers only | `<stdint.h>`, other SoC headers | code, a phone's wiring | `socs/<soc>/include/hakios.<soc>_*.h` |
| 1 | **Codecs** | pure functions over bytes and integers: frame and packet formats (SIPC, H4/HCI, the sensor hub's frames, QMI, UPIU), file-system readers (ext4, the GPT, decmpfs), checks of what another processor or a file says (lengths, crops, CRCs), arithmetic (gamma, UART divisors, VA allocators), whitelists | `<stdbool.h>`, `<stddef.h>`, `<stdint.h>`, `<string.h>`, `<limits.h>`, other codecs | any kernel header, a global, a lock, a log line, the clock | a plain-C header `hakios.<driver>_<what>.h` beside its driver; a host test in `tools/` that CI runs |
| 2 | **Core services** | what every driver may use: the log (`hakios_say`), the step runner, MMIO helpers, the XNU-version compatibility header, `hakios_dev_strict`, physical-memory carve-outs | XNU, layers 0-1 | a driver's knowledge | `drivers/core/`, and today the fork's `bsd/sys/hakios.{log,xnu,physmap}.h` |
| 3 | **SoC blocks** | the registers and sequences of one hardware block of one SoC family -- HSI2C on USI, SPI, a UART, ACPM's mailbox, the PCIe root complex, DECON and DPP, a UFS host's vendor registers, CMU gates -- with no policy and no OS interface: reusable by every phone with that SoC | layers 0-2 | a device node, a sysctl, a boot argument that chooses behaviour, a phone's board wiring | `drivers/soc/<family>/`, or `drivers/<class>/<driver>/hakios.<driver>_<soc>.c` for a block one driver alone uses |
| 4 | **Transports** | moving bytes to a chip or another processor: an I2C or SPI transfer, a UART's FIFO, the loader's block mailbox, a ring in shared memory, SMEM/SMP2P/GLINK/QRTR, MobiCore's notification queue | layers 0-3 | what the bytes mean (that is a codec), what a phone has (that is policy) | its own file in the driver's directory, or `drivers/soc/` when several drivers share it |
| 5 | **Chip drivers** | one chip or one function of one: an amplifier, a codec chip, a sensor, a PMIC's LDOs, a lens motor -- its registers, its sequences, its small API | layers 0-4 | a device node; another chip driver (they meet in policy) | a file of its own: `hakios.max98512.c`, `hakios.ak737x.c` |
| 6 | **Device policy** | which chips this phone has and on which bus, the order of bring-up, the steps a boot argument selects, defaults, what may be written and what never (UFS's fences, the modem's partitions) | layers 0-5 | register access of its own (it asks a block or chip driver) | the driver's main file, `hakios.<driver>.c`, and its step table |
| 7 | **OS interface** | device nodes (`cdevsw`, devfs), sysctls, kernel controls, ttys, IOKit classes and user clients, per XNU version | layers 0-6 | hardware access | `hakios.<driver>_os.c` (or `.cpp` for IOKit), and `hakios.<driver>_xnu<N>.c` where the two XNUs differ |
| 8 | **Diagnostics** | console commands, self-tests, benchmarks, soak runs, summaries | anything | being called by anything but the console's dispatcher and a step that names it | `hakios.<driver>_diag.c` |

The two exceptions: a codec may be included by a host test, which is not kernel code; and diagnostics may reach into
every layer of its own driver, which is why nothing else may call it -- a bug in a self-test cannot then sit on the path
a phone takes every run.

Where today's code is, layer by layer: layer 0 is done (`socs/`, `sys/hakios.exynos_soc.h`); layer 1 exists for five
headers (`hakios.bcm_hci.h`, `hakios.bcm_shub.h`, `hakios.s6e3ha8_gamma.h` with `hakios.s6e3ha8_star2.h`,
`hakios.mali_va.h`), each with a host test; layer 3 is partly done (`drivers/soc/exynos/`: HSI2C, SPI, ACPM, the M3 cluster,
the TEE); everything else is mixed inside one file per driver, and the census says where.

### What may depend on what, as a rule a build can check

- A **codec** includes only the five C headers above and other codecs, and a CI host test names it. *Checked now.*
- A **SoC header** includes only `<stdint.h>` and other SoC headers. *Checked now.*
- **No file includes a `.c` file.** One translation unit is one file; the parts of a driver are files of their own,
  compiled on their own. *Checked now.*
- **No file of `drivers/` or `socs/` is over 800 lines** unless `tools/arch-size.allow` lists it, and an allowance only
  goes down. *Checked now.*
- An **internal header** (`hakios.<driver>_internal.h`) is included only by the files of its own directory. *Next:
  checked once the first driver has one.*
- Only **OS-interface files** include `<miscfs/devfs/devfs.h>`, `<sys/conf.h>`, `<sys/kern_control.h>`, `<sys/tty.h>`,
  `<sys/sysctl.h>` or IOKit headers, and only **diagnostics** files define console commands (`hakios_*_console`). *Next:
  checked per driver as each is split, by naming its files in `tools/arch-layers.conf`; a driver not yet split is not
  held to it.*
- `__has_include(<vm/vm_kern_xnu.h>)` appears only in the compatibility header. *Next: today 34 times in 6 files.*

## What CI enforces

`tools/arch-check.py` runs in `.github/workflows/check.yml`:

- **size**: a source file of `drivers/` or `socs/` over 800 lines fails unless `tools/arch-size.allow` lists it with an
  allowance it is within. The list was taken from today's tree, so CI is green from the start; where an open pull
  request grows a listed file (#319, #320, #329) its growth is in the allowance and the note names the request, so that
  it stays green. The list only shrinks: an entry for a file that is gone or back under the ceiling fails until it is
  deleted. A change that must grow a listed file raises its number in the same pull request and says why; a reviewer
  then decides whether the file is split first.
- **codec**: the files `tools/arch-layers.conf` names as codecs include only the C library's freestanding headers and
  other codecs, and are named by a host test CI runs.
- **soc**: SoC headers include nothing of the kernel.
- **no `.c` included**.
- `--report` lists the files of `userland/src/` and `tools/` over the ceiling: 50 today, not yet enforced, because
  #329 alone adds four new ones (`gnssd.m` and others); they are gated the same way once it has merged.

The two host tests that existed but were not run by CI, `tools/mali-va-test.sh` and `tools/usb-acm-test.sh`, run in it now
(both pass on Linux and macOS under the sanitizers). Two others cannot run from this repository alone and are not added:
`tools/exynos-tmu-test.sh` includes `<sys/hakios.exynos_soc.h>`, which is only in the fork, and `tools/guard-test.sh` copies
`sys/hakios.audio.h` out of a 16.5 kernel tree; [What a split cannot fix](#what-a-split-cannot-fix), item 4, is why.

## Conventions

### Files and names

- A driver of more than one file, or of more than a few hundred lines, has a directory: `drivers/<class>/<driver>/`
  (`drivers/storage/ufs/`, `drivers/bluetooth/bcm4361/`).
- Its files are `hakios.<driver>.c` (the driver: its policy, its steps, its init) and `hakios.<driver>_<part>.<ext>`
  for each other part, `<part>` saying what it holds: `_upiu.h` (a codec), `_loader.c` (a transport), `_exynos.c` (a SoC
  block), `_os.c` (the OS interface), `_diag.c` (diagnostics), `_internal.h` (what the driver's files share). The
  underscore, not a second dot, so that every tool that reads `hakios\.(\w+)\.` keeps reading one name.
- One responsibility per file. The ceiling is **800 lines**; a file of one responsibility that needs more (a register
  table, generated data) is listed in `tools/arch-size.allow` with the reason. Aim at 300 to 600.
- **When a file passes the ceiling**: split it before adding to it, by the method below, in a pull request of its own that
  changes nothing else. If that cannot wait, raise the allowance in the change and open the split next.

### A driver of several files

Today a driver is one or a few files, each mapped to a flat name in the XNU tree (`bsd/dev/hakios.ufs.c`). A driver of
several files keeps that: every file of it is a line in each list, with the same flat target name.

| List | What it does | A driver of three files |
|---|---|---|
| `drivers/map` | which directory each file lives in, and its path in the 16.5 tree (`xnu/apply.sh` copies it) | three lines, `storage/ufs bsd/dev/hakios.ufs_loader.c` and so on |
| `drivers/<class>/conf` | which files the `driver` block compiles (16.5: `tools/hakios-config.py kernel` writes `bsd/conf/files`) | one `file` line per `.c`; headers are not listed |
| `profiles/ios-27.copy` | which files the iOS 27 tree takes | one line per file, headers too |
| the fork's `bsd/conf/files` | which of them the iOS 27 kernel compiles, and for which machine | one line per `.c` |

The last row is the difficulty. The iOS 27 kernel compiles the drivers its fork lists by hand
(32 lines of `bsd/conf/files` from line 108 on, in `scion/xnu-12377`), while the files themselves come from this repository. A new
`.c` therefore needs a commit in each repository, landed in order, and a pin of `fork_commit` in between: a split would
cost two pull requests and a window in which one kernel does not build. The proposal, which the first split that adds a
translation unit to the iOS 27 kernel carries:

1. `profiles/ios-27.copy` takes an optional third field on a `.c` line: the `bsd/conf/files` attribute it is built with
   (`optional scion_generic_arm64`, `optional scion_frankel`). `xnu/fork.sh` appends such lines to the tree's
   `bsd/conf/files` (or `iokit/conf/files` for `iokit/`) after copying, and refuses one the fork already lists.
2. New files use it from then on; the fork's hand-written lines are not touched, so nothing that builds today changes.
3. Later, one pull request in each repository moves the fork's 32 hand-written lines into `profiles/ios-27.copy`, so
   that the drivers' list for both kernels lives here, as `drivers/*/conf` already does for 16.5.

Until then a split that only lifts a codec out (a header, no new translation unit) needs no change to any list but
`drivers/map` and `profiles/ios-27.copy`, and that is why the first splits are of that kind.

### Headers

- **Public** -- what other drivers or XNU call: `sys/hakios.<driver>.h`. Today these are in the fork
  (`bsd/sys/hakios.*.h`, 24 files) while the drivers that implement them are here; see item 4 below.
- **Internal** -- what the files of one driver share: `hakios.<driver>_internal.h` in its directory, included with
  quotes, by its own files only. It declares the functions one part calls in another, the driver's shared state as one
  `struct` (not a scatter of globals), and the lock that guards it.
- **Codec** -- `hakios.<driver>_<what>.h`, plain C, `static` or `static inline` functions, included by the driver and by
  its host test. The head says what format it reads, where each fact comes from, and that it has no kernel dependency.

### The two XNU versions

The drivers that both kernels compile now ask `__has_include(<vm/vm_kern_xnu.h>)` where they differ (34 times in 6 files).
That question is not about the XNU interface: it stands for "this is the iOS 27 kernel", which has no loader hand-back,
no POCO drivers and no recording into loader memory. One header says it once:

```c
/* sys/hakios.base.h: which kernel this is, and what it has. The only file that asks. */
#if __has_include(<vm/vm_kern_xnu.h>)
#define HAKIOS_XNU 12377
#else
#define HAKIOS_XNU 8796
#endif
#define HAKIOS_HAS_LOADER_RETURN (HAKIOS_XNU == 8796)   /* osfmk/arm64/loader_return.c */
```

and a driver asks `#if HAKIOS_HAS_LOADER_RETURN`, a name that says why. Where the difference is more than a few lines
(the audio recording into loader memory, `hakios.audio.c`), it goes into `hakios.<driver>_xnu8796.c`, compiled only by
the 16.5 kernel's `conf`, with the iOS 27 side's stand-ins in `hakios.<driver>_xnu12377.c`. The declarations of XNU
functions no public header has stay in `sys/hakios.xnu.h` (the fork), as [modular-kernel.md](modular-kernel.md) says.

### Steps

A bring-up step is a named function in a table, not a comparison inside a long function:

```c
static const struct hakios_step bt_steps[] = {
	{ "a look: the pins, the clocks, the UART", step_look },
	{ "the expander: BT_EN", step_expander },
	...
};
	hakios_steps_run("bt", bt_steps, nitems(bt_steps), steps);   /* steps: the boot argument */
```

`hakios_steps_run` (core) says each step's number and name before it runs it, stops at the first that returns false, and
says how far it got: the sentence every driver writes by hand today. ABOX already works this way (`abox_thread_run`,
`hakios.exynos_abox.c`); the S9's Bluetooth, sensor hub, camera and modem drivers each compare `step >= N` in one long
thread function. A step's function is in the file of the layer it belongs to; the table is policy, in `hakios.<driver>.c`.
`tools/boot-args.py` keeps finding the argument, which is still read with `PE_parse_boot_argn` in the driver.

### Errors and logging

- A function that can fail returns `bool` (true: done) or an `errno` value, and says why it failed once, where it found
  out; its callers do not repeat it. A function that returns a reason returns a `const char *` (NULL: none), as UFS's
  whitelist does, so that the caller decides whether and how to say it.
- Every line goes through `hakios_say` and begins with the driver's name and a colon (`ufs: native: ...`). A line in a
  loop that can run per packet or per frame is rate-limited at the call (the first N, then every Mth), and says so.
- Nothing parsed from outside the kernel -- a chip, another processor, a file, user space -- is used as a length, an
  index or a count before a codec has checked it against the buffer it is in.

### Locking

Each file that has state says at its head, in one paragraph headed `Locking:`, which lock guards which state, the order
in which two locks are taken, what may sleep with it held, and which functions run on which thread. A lock is declared in
the file that owns the state; an internal header declares it for the driver's other files.

### Host tests by construction

A parser is written as a codec from the start, so its test is a C file in `tools/` that includes the header and feeds it
good input, mutated input and hostile input under the address and undefined-behaviour sanitizers, and a `tools/*-test.sh`
that CI runs. The check refuses a codec no CI test names. The two tests today that cut a function out of a driver by its
first and last lines (`tools/bcm-bt-parsers-test.sh`, and the modem's in #319) are moved to that form when their driver is
split.

## How a split is proven

A split changes where code is and nothing else. A bug found on the way is reported separately, never fixed in the split.
No symbol is renamed, no comment improved, nothing reformatted: the contributor fork that carries patches against these
files must be able to move them by the mapping alone. The pull request of a split carries, in its description, the output
of each step below.

1. **The lines moved.** `tools/split-check.py --map FILE...` compares every line of the files the split touches, before
   and after, as one multiset. A pure move leaves them equal; what a split may add -- blank and comment lines (the head
   of each new file), `#include` lines and include guards, prototypes of functions that exist, `static` dropped where a
   function is now called from another file -- is listed by class, and any other line, removed or added, is printed with
   its file and line and fails the check. `--map` prints the old line ranges and the new file and lines each went to.
2. **The kernels are the same.** Every kernel the files reach is built before and after, through `~/s9/agents/bin/buildlock`:
   16.5 `starlte_defconfig`, `star2lte_defconfig` and `crownlte_defconfig`, the iOS 27 STARLTE and FRANKEL kernels.
   `tools/split-objcmp.py kernel BEFORE AFTER` checks that the external symbols defined are the same set and the C
   strings the same multiset, and lists every local symbol that differs. Some local symbols are named after the line
   they are declared on (`LCK_GRP_DECLARE`, `LCK_MTX_DECLARE` and the other `STARTUP` macros of XNU make
   `___startup_LOCKS_entry_..._<line>`): a split that moves text above them renames those, and nothing else.
3. **The functions are the same.** `tools/split-objcmp.py objects BEFORE.o AFTER.o[,...]` disassembles each function of
   the split's objects, takes out addresses (a branch keeps its target's name) and string labels, and compares them one
   by one. A split that keeps one translation unit (a codec header included where the code was) must give the same code
   for every function. One that makes a new translation unit may differ only where the compiler inlined across the new
   boundary; each such function is listed and read.
4. **The tests pass**: every host test CI runs, the new codec's own, and the repository's checks (`tools/arch-check.py`,
   `tools/drivers-check.py`, `tools/boot-args.py --check`, `tools/docs-gen.py --check`).
5. **What it costs a fork.** The description says which old line ranges went where, so that a patch against the old
   file can be moved with `git apply --directory` or by hand, and that nothing else changed.

When all five hold, the split can be merged without a run on a phone: the phone runs the same instructions.

## The order of work

The full list, with line ranges, open pull requests and the proposed split of each file, is in
[architecture-census.md](architecture-census.md). In short:

1. **This page and its checks** (no code moved).
2. **Codecs lifted out of drivers no open pull request touches**, each with its host test: UFS's UPIU builders and
   whitelist (`hakios.ufshc_upiu.h`, the one check every command passes before it reaches the flash, which no host test
   covered), then the Bluetooth driver's scan, trace and vendor-command filters (replacing the test that cuts them out of
   the driver by their first lines), then the GPT check of UFS's own mode.
3. **The multi-file mechanism** (`profiles/ios-27.copy`'s third field), then the splits into translation units of the
   free drivers: `hakios.ufs.c` (loader transport, devices, own mode, the ufs=3 switch, diagnostics), `hakios.ufshc.c`
   (SoC hooks, slots, shared mode, own mode), `hakios.bcm4361_bt.c` (UART and expander, HCI and patch, the tty and kernel
   control, steps), `hakios.exynos_abox.c`, `hakios.exynos_cpufreq.c`.
4. **The busy files**, each when the pull requests that touch it have merged: the modem (`hakios.shannon.c`, #319), the
   video decoder (`hakios.exynos_mfc.c`, #320 and #329), the front camera (`hakios.exynos_camera.c`, #328), the Mali,
   the sensor hub, the display and the USB gadget (#329), and in the fork the GPU and Mali service (`starlte_gpu.c`,
   `StarbiteMali.cpp`, #29).

## What a split cannot fix

Five problems are in the structure, not in the size of files. Each has a proposal; none is done here.

1. **Three copies of Qualcomm's remote-processor stack.** `hakios.wifi.c`, `hakios.mpss.c` and `hakios.adsp.c` each carry
   their own register access, SMEM, SMP2P, doorbells, RPMh votes, TrustZone's PAS calls, GLINK, QRTR and a link thread,
   marked "(as in hakios.wifi.c)": 928 of `hakios.mpss.c`'s long lines are also in `hakios.adsp.c`, 708 in
   `hakios.wifi.c`. A fix to GLINK's ring arithmetic has to be found and made three times. Proposal: `drivers/soc/qcom/`
   with `hakios.qcom_smem.c`, `_smp2p.c`, `_glink.c` (transport), `_qrtr.c` (with its packet format as a codec),
   `_pas.c`, and one `struct qcom_rproc` per remote processor that the three drivers fill in; their step tables then
   name the shared steps. It touches only POCO drivers, which no open pull request touches; the risk is that only QEMU
   and the POCO can test it.
2. **Two copies of the S9's front-camera driver.** `hakios.exynos_camera_star2.c` (4,482 lines) began as a copy of
   `hakios.exynos_camera.c`: 1,528 of its 2,375 long lines are in the other. The ISPPRE domain, CSIS, D-PHY, WDMA and the
   frame path are the same hardware on both phones; the sensors and their tables differ. Proposal: one camera core
   (`hakios.exynos_csis.c`: domain, PHY, DMA, frames), sensor drivers as chip drivers (`hakios.s5k3h1.c`, `hakios.s5k2l3.c`,
   ...), and each phone's camera as a policy file that names its sensors. The iris camera (`hakios.exynos_iris.c`)
   repeats `isp_up`, `isp_down` and `link_setup` too. Waits for #328.
3. **Drivers copied into the fork and changed there.** The fork has its own `hakios.exynos_cpufreq.c` (656 of 808 long
   lines the same as this repository's), `hakios.exynos_dpu.c` (387 of 451), `hakios.s6e3ha8.c` and `hakios.exynos_tmu.c`,
   each a fork of a file here that has since moved on in both places. A fix lands in one. Proposal: for each, the
   difference made into a hook or an `#if HAKIOS_XNU` in the file here, the fork's copy deleted and the file taken by
   `profiles/ios-27.copy`, as `hakios.s6e3ha8_gamma.h` already is. One driver per pull request pair, the governor first
   once its over-current work has settled.
4. **The drivers' public headers live in the other repository.** The drivers are here; the 24 headers that declare what
   they offer each other (`sys/hakios.exynos_hsi2c.h`, `sys/hakios.audio.h`, `sys/hakios.log.h`, ...) are only in the
   fork. A change to a driver's interface is two pull requests, and a host test cannot include them (that is why
   `tools/exynos-tmu-test.sh` does not build here and `tools/guard-test.sh` needs a 16.5 kernel tree). Proposal:
   `drivers/include/sys/` here, copied to `bsd/sys/` by `drivers/map` and `profiles/ios-27.copy` like the SoC headers;
   the fork keeps only the headers of its own files.
5. **Shared global state without an owner.** Most drivers keep their state in file-scope `static` variables guarded by
   whichever lock was at hand (UFS's `hakios.ufshc.c` has 56 of them, `hakios.ufs.c` 53; the Mali's `hakios.mali.c` shares its job
   state between the bring-up thread, the IOKit service and the governor). That is what the audit found shared
   unsafely in the GPU. Proposal: each driver's state in one `struct`, declared in its internal header with the lock
   that guards each field written beside it; then a function's dependencies are its arguments. This is a logic-level
   change and is done after a driver is split, one driver at a time, with its own review.

## Adding a driver

1. Put it in `drivers/<class>/<driver>/` and decide the layer of each piece before writing it: the registers and
   sequences of a block (layer 3), the bus (4), the chip (5), the parsers (1: a codec header and its host test, from the
   first commit), the order and the steps (6, `hakios.<driver>.c`), the nodes and sysctls (7, `_os.c`), the console and
   tests (8, `_diag.c`).
2. Each file opens with the head `drivers/README.md` shows: what it drives, what the hardware needs that the code cannot
   say, its boot arguments, its sources, what has and has not been run on a phone; and, for a file with state, its
   `Locking:` paragraph.
3. Numbers of the SoC go in `socs/<soc>/include/`; what a phone has goes in its device repository.
4. List every file in `drivers/map`, every `.c` in a `driver` block of `drivers/<class>/conf`, and every file the iOS 27
   kernel takes in `profiles/ios-27.copy`.
5. Behaviour is off by default and behind a boot argument read with `PE_parse_boot_argn`; `tools/boot-args.py --check`
   then wants the catalogue regenerated.
6. Before the pull request: `python3 tools/drivers-check.py`, `python3 tools/arch-check.py`, the generators in the order
   `CONTRIBUTING.md` gives, the host tests, and a build of each kernel the driver is in.
