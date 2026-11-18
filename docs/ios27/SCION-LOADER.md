# The S9's second loader, option 2: scion-boot's loader (18 November 2026; built, NOT yet run on a phone)

S-Boot starts whatever is in BOOT's kernel slot. Today that is `xnuloader/sboot` ("Starbite", built by
[build-loader.sh](../../tools/ios27/s9/build-loader.sh)) with the iOS payload appended, and it stays the
default. [make-boot-image.sh](../../tools/ios27/s9/make-boot-image.sh) can now put a second loader there instead:
`loader/` of [Scion-Project/scion-boot-exynos9810](https://github.com/Scion-Project/scion-boot-exynos9810)
(MIT; design and status in its
[docs/STATUS.md](https://github.com/Scion-Project/scion-boot-exynos9810/blob/agent/scion-loader/loader/docs/STATUS.md)).

What it adds over sboot, all of it tested in QEMU (`make qemu-test` there: 13 scenarios) and none of it on a phone yet:

- the **SHA-256** of every payload is checked before it is started (and, when the loader is built with `PINS=`, the
  digest must be one of the project's own list); a mismatch shows **NOT STARTED: CHECK FAILED** with both digests
  instead of a jump into a damaged image;
- a **menu** on the panel: iOS 27, other entries (iOS 16.5, a known-good build, sboot itself), Reboot to recovery
  (TWRP), Download mode, Power off; the command line shown and editable with Bixby for one start;
- a **boot-loop guard**: three automatic starts that never acknowledge start the known-good entry, then make recovery
  the default;
- a **crash screen** for the loader's own faults, and a **re-entry point** (base + 0x40, `struct scion_reentry`) that
  the warm restart (docs/ios27/WARM-RESTART.md, from the warm-restart work) uses.

What it does not do itself: the OS store, Linux/Android/Windows, USB fastboot, EngineerMode, POST, menu v7. With
`--scion-chain-sboot` sboot (with the payload) is appended as the entry "Starbite", so all of that stays one menu
entry away; the payload is then in BOOT twice (BOOT holds 57.7 MB: `starlte-pack-boot.py` refuses an image that
does not fit).

## Making the image

```sh
# in scion-boot-exynos9810 (once per change there)
make PLATFORM=starlte loader            # build/starlte/scion-loader.bin
# here: the same inputs as the image that boots today, plus
tools/ios27/s9/make-boot-image.sh --kernel ... --donor-adt ... --template ... \
    --loader-kind scion --scion-boot ~/src/scion-boot-exynos9810 [--scion-chain-sboot] \
    [--scion-entry 'iOS 16.5:xnu:payload-165.bin:safe']
```

Without `--loader-kind scion` nothing changes: the same steps and the same image as before.

## The first test on the phone

The plan is scion-boot's
[docs/phone-test-plan.md](https://github.com/Scion-Project/scion-boot-exynos9810/blob/agent/scion-loader/loader/docs/phone-test-plan.md):
the smallest image (the loader and the iOS 27 payload that last booted), written to BOOT with
`flash.sh SERIAL boot IMG` and nothing else; expected a dark screen with a blue bar "Scion loader v1 starlte", the
entries, a 5 s countdown, "Checking SHA-256", then iOS 27 as with sboot. The way back is the usual one: TWRP
(Volume Up + Bixby + Power, or the menu's Reboot to recovery), then `flash.sh SERIAL boot` with the previous BOOT
image (keep it first: `flash.sh SERIAL backup-boot`). Until that test has passed, keep `--loader-kind sboot` for
everything else.
