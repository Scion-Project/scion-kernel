# Google Pixel 10 Pro (blazer, Tensor G5): the ios-26 base

The second XNU base of this tree, xnu-12377.121.6 for iOS 26 (arm64e), starts here. `docs/two-bases.md` is the design.

**State: nothing here has run.** `make blazer_defconfig && make kernel` takes the fork of XNU, Apple's xnu-12377.121.6 with Scion's
changes (`scion/xnu-12377`, docs/xnu-fork.md); how far the build gets on the build host is in `docs/two-bases.md`. No Scion image boots this phone.

## Where it comes from

[ios-on-pixel-10](https://github.com/TheAndersMadsen/ios-on-pixel-10) by TheAndersMadsen boots iOS 26 on this phone: XNU 12377,
five A725 cores, display, touch, UFS reads and a boot chooser are recorded there as seen on the phone (its `NOTES.md`). Scion has
it privately as `Scion-Project/scion-blazer`. Its author lets Scion use **his own code** under Scion's licence, with credit. That
does not cover what in it comes from others:

- Its XNU 12377 kernel series is not in its git at all: that tree carries `ios-tegu`'s changes, which have no licence. So the
  kernel this device needs is not importable, and has to be written again on the pristine base (`docs/two-bases.md`, gaps).
- Its boot shim mixes GPL-2.0-only (`tgl_ufs_link.*`), GPL-derived tables (`frame_tables.h`, `dcs_tables.h`), tegu-derived
  files with no licence (`start.S`, `loader.c`, `payload.h`, `laguna_usb.*`) and MIT files of its own. None of it is here.

What is here, in `carried/`, is the author's own kernel-side code with an MIT header and nothing of tegu's in it, kept as source
to build on. **None of it is built**: each file needs headers and hooks of that tree's kernel (`blazer_hw_port.h`,
`blazer_cpmq_xnu.h`, `blazer_usbcon_core.inc`, `sys/hakios.blazer_ufs_exact.h`) that are not importable.

| `carried/` | From (scion-blazer f04fd1f) | What it is |
|---|---|---|
| `dualboot/` | `dualboot/readback.*`, `xnu_usb_readback.inc`, `test_readback.c` | a bounded, read-only USB readback of one partition, with its host test |
| `exit_trace/` | `runtime/exit_trace/blazer_exit_trace.c`, `LICENSE` | a bounded record of process exits through XNU's own unwinder |
| `nfc/` | `research/ios26-nfc-r1/receipts/0001-hk-nfc1.patch` (its new files) | NFC controller bring-up on the cli10 bus, off unless `blazer_nfc=N` |
| `haptic/` | `research/ios26-phaseb-r1/pwr/patches/0003-*`, `0832-*` (new headers) | tap-feedback policy; the ROM-effect service interface |
| `sensors/` | `research/ios26-sensors-r1/mag-r1/patches/0930-*` (new files) | MMC5616WA compass, measurements only |
| `power/` | `research/ios26-phaseb-r1/chg/xnu-src/0001-hk-chg1.patch` (new header) | MAX77779 charger registers decoded, read only. Any charging write needs the owner's go |

Each file's first lines name its origin. Taking one into the build means: a `drivers/<class>/conf` entry limited to the
`xnu-12377` base, the hooks it needs written fresh on the pristine base, and a phone run before anything is on by default.
