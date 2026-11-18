# libcorecrypto on the S9: the hottest arm64e traps, and its FIPS seal (18 November 2026)

## What the phone was trapping on

On phone 1 (iOS 27.2.1, 24B156, the `o272` boot image, root v9) the kernel's `STARLTE A64E hot` line (the ten sites that trapped most in
the last ten seconds) read, at idle, a pair of `RETAB` words at about 2,000-15,000 traps each per ten seconds, and during the re-render of
the home screen's icons after a switch to dark mode (`iconservicesagent` at ~150 % CPU for two minutes) **106,512 and 104,880** traps in one
ten-second interval at those two words, with a dozen more sites at 2,000-17,000 (`RETAB` `d65f0fff`, `BLRAAZ x8` `d63f091f`,
`BLRAA x16, x17` `d73f0a11`, `BRAAZ x5` `d61f08bf`). `STARLTE LSE hot` had one more site next to them.

The addresses in those lines are **slid**: the same sites read `0x1da0a3c5c` in a boot whose `CRASH` lines say `slide 0x63a8000` and
`0x1d919fc5c` in one with `slide 0x54a4000`; both are `0x1d3cfbc5c` unslid. Every hot site, unslid, is in
`/usr/lib/system/libcorecrypto.dylib`'s `__TEXT,__text`: the hottest pair is the return of `ccder_blob_decode_range` (`0x1d3cfbbe0`) and of
the local function right after it (`0x1d3cfbc5c`), then `ccder_sizeof` and local functions whose names are not in the cache (DER decoding,
most likely of signatures and certificates). The LSE site is the library's one `SWP` (`0x1d3cfa77c`).

## Why they were left

Not an encoding the rewrite misses, not a section it does not scan: `a64e-rewrite.py` handles all of them. The cache pipeline passed it
`--skip libcorecrypto` (and `lse-rewrite.py`, `a64e-rewrite2.py`, `rewrite-grow.py` skip it by default), because corecrypto's FIPS
power-on self-test checks a MAC of its own code, and a rewritten library fails it. So the whole library, 3,538 words (1,500 `RETA*`, 67
`BRA*`, 1,312 `BLRA*`, 659 `PAC*`/`AUT*` register forms), stayed arm64e and went through the kernel's emulation on every call.

`a64e-rewrite.py census` (new) shows it at once: on the v9 root's cache it prints exactly three images with words left, the three
corecrypto images, and nothing else in 4,759.

## The seal, and why it can simply be recomputed

corecrypto's integrity test (`fipspost_post_integrity`, run in user space by `/usr/libexec/cc_fips_test`; apple/corecrypto
`cc_fips/src/fipspost_get_hmac.c`) is HMAC-SHA256 with a key of 32 zero bytes over the image's first `__TEXT,__text` section, compared with
the 32 bytes of its `__TEXT,__fips_hmacs` section. There is no secret and nothing outside the image: on the 24B156 cache the stored value
of `libcorecrypto.dylib` is exactly that HMAC of its `__text` as it sits in the cache. `tools/ios27/a64e/fips-seal.py` recomputes it after
the rewrite and writes it into `__fips_hmacs`, **only for an image whose seal was true in the reference** (the stock cache, or the cache
before the step) and whose stored value is still the reference one. The test is as strong as before: any change after the reseal fails it
again. The `_noasm` and `_trace` variants are not loaded by anything and their seals are already false in the stock cache; they stay
untranslated and untouched.

So the stand-in for `cc_fips_test` that the project's roots carry (`docs/ios27/SPEED.md`, "Not published") is no longer needed for this:
the real test's integrity check passes on a resealed cache (the stand-in is still on the roots; the real helper has not been run on a phone).

## The tools

- `a64e-rewrite.py cache ... --log FILE` writes `ADDR OLD NEW` for every changed word; `verify --log` checks every word holds its new value,
  `restore --log` puts the originals back (round trip, as `lse-rewrite.py verify/restore`).
- `a64e-rewrite.py census CACHE [--allow SUBSTR ...]`: per image, the words the rewrite would still change, nothing skipped; exit 1 for an image
  not allowed. The pipeline runs it last (step 9) with only the two unloaded variants allowed.
- `fips-seal.py check|reseal|restore` (pure Python; `fips-seal-test.py` exercises it on made-up Mach-O files, in CI).
- `pipeline/cache-pipeline.sh`: steps 1, 2 and 4 now skip only `libcorecrypto_noasm` and `libcorecrypto_trace`; step 5b reseals and checks
  (before the `PRE` reference of the grow step, which skips corecrypto); step 9 is the census.

The LSE pass over corecrypto must run with the text-data skip map (`TEXTDATA_SKIP`, `docs/ios27/TEXT-DATA.md`), as it does in the pipeline:
two words of corecrypto's SHA-256 constant table (`0x78a5636f`) have the bit pattern of `LDUMAXAH` and would otherwise become branches.

## On the v9 root (off the phone)

As one root step on a clone of root v9 (the step script and its undo are private, on the mini): 3,538 words rewritten and verified, the
one `SWP` given a thunk (the two table words left alone), the seal recomputed and checked; 3,560 words of one subcache file and 4 of
another differ from v9, nothing else. `textdata-guard.py scan --pac` against v9 flags six of the changed words by rule D; all six are
ordinary code (epilogues, `BLRAAZ` calls, `PACIZA x16` before a store). The census then shows only the two unloaded variants.
