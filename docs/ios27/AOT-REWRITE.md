# iOS 27 on the Galaxy S9: rewriting every instruction the M3 lacks, ahead of time (9 November 2026)

The Exynos 9810's four Mongoose M3 cores are ARMv8.0; iOS 27's shared cache is built for ARMv8.5+. [EIGHT-CORES.md](EIGHT-CORES.md) describes the
first step (LSE atomics rewritten into stubs in the cache's own padding); this page is the rest: **every** class an M3 lacks, the stubs moved
to space that is big enough, and the one thing that decides whether the phone boots with them (the code signature). It was run on a phone's
root on 9 November. The kernel keeps its trap emulation as a safety net; the point is that nothing reaches it.

## What the M3 still ran

After the LSE pass the cache had 617,681 sites left that an M3 would trap on (counted by decoding every distinct word of the pure-instruction
sections twice, as a Cortex-A53, which is ARMv8.0 + CRC, and with every feature, and classing the words only the second decodes): LSE 538,071 in
sixteen "cold" images with no padding left (WebCore, WebKit, JavaScriptCore, the 3D/vision frameworks, ...); FP16 48 k, FCADD/FCMLA 25 k, BF16 1.7 k,
SHA3/SHA512 1.4 k, FJCVTZS 1.1 k, DotProd 1.0 k, I8MM 0.5 k, RDM 184, LRCPC2 15, LOR 4. SVE/SME words are data or unreachable code on these
cores and are left. The root's own executables (284 files, 74 k sites, mostly LSE) are outside the cache and are not touched here.

## The rewrite

Every site becomes `B thunk`; the thunk is the one of [EIGHT-CORES.md](EIGHT-CORES.md) (`STR x30, [sp, #-144]!`, `BL body`, `LDR x30, [sp], #144`,
`B site+4`), and the body is shared by every site with the same encoding. Bodies per class: LSE as before (A55: the instruction itself, M3: an
exclusive loop); FP16: compute in binary64 and round once to half (exhaustively compared with the instruction on an M1 for every operand pair,
every rounding mode, FZ16, NaN and flag case); FCADD/FCMLA, RDM, DotProd, I8MM, BF16, FHM, SHA3/SHA512, FJCVTZS: NEON sequences, compared with the
instruction on an M1 (which has them all) over millions of random states per word; LDAPR/LDLAR/STLLR and `LDAPUR`/`STLUR #0`: the stronger
`LDAR`/`STLR` in place. 645,782 sites were rewritten: 538,053 LSE, 77,640 FP16, 30,080 other, 9 in place.

**The frame contract.** The thunk's 144 bytes are not free: `[sp]` holds the caller's `x30` and `[sp+16, sp+144)` is the caller's red zone. A body
must lower `sp` itself before it stores anything. The first run on a phone had the FP16 bodies using the thunk's frame ("fits in the 144 bytes"):
the first store overwrote the saved `x30`, the thunk returned to `x30 = 0`, and SpringBoard died at `pc = 0` before it showed a frame. The checker
now decodes every distinct body and fails if any store lands in `[entry sp, entry sp + 144)`.

## Where the stubs go: the VA gap after each small subcache

The cache's text is a series of ~128 MB subcache files, each followed by a small (a few hundred KB) executable subcache, then 2-3 MB of unmapped
address space until the next big one: 29 gaps, about 21 MB used here. The stubs are put there by **growing the small subcache's single `__TEXT`
mapping** into the gap:

- the file becomes `[old text][stub pages][code signature]`; `mappingCount` and both mapping arrays stay where they are, only the size of
  mapping 0 (in `mapping_info` and in `mapping_and_slide_info`) and `codeSignatureOffset` change;
- the stubs are position independent (`B`/`BL` only), so they need no slide information.

**The code signature must cover what is mapped.** In the stock cache every subcache's CodeDirectory has `codeLimit` equal to the end of its last
mapped file range. If it does not, dyld does not load the cache: `launchd` panics ("no dyld cache"). Two earlier attempts failed exactly there
(a second mapping per subcache with relocated header arrays, then a grown mapping with the old signature). Nothing else was needed: the kernel's
`shared_region_map_and_slide` accepts the grown mapping, and the signature is not enforced (`cs_enforcement_disable=1` on the command line). The fix
appends the SHA-256 page hashes of the new pages to the CodeDirectory, raises `nCodeSlots` and `codeLimit`, and moves the blobs that follow it.

## What was checked before the phone

An off-phone checker replays the rules for the whole cache: header page identical to the reference except the three fields; 16 KB alignment,
file ranges inside the file, no overlap of any two mappings of any subcache, inside the shared region; `codeLimit` of every file equals the end
of its last mapped file range, and the grown blob equals the reference blob grown by the new pages (byte for byte); every one of the 645,782
sites is `B thunk` with the thunk and body words the generator makes, both inside one executable mapping within +-128 MB, over zeros in the
reference; no interval overlaps another; unused extension bytes are zero; the frame contract above; and a re-census: no word the A53 cannot
decode is left in an executable section except SVE/SME/undefined words, 816 SHA3 and 550 BF16 words of forms without a body, 18 LSE words
(`libcorecrypto`, which is skipped on purpose: its FIPS self-check hashes its own code, and three data words) and 2 `LDAPUR`. Every stub word
decodes as a Cortex-A53 except the native LSE words of the A55 half of the dual bodies.

## On the phone (9 November, `cpus=8`)

SpringBoard up and stable, no crash at all in the last boot (the owner build's `MessagesBlastDoorService` and `locationd` loops aside: the first is disabled, the second did not recur); `STARLTE LSE (M3)` and the new `STARLTE M3 RESIDUAL (10s)` line (one count per
class of instruction an M3 still ran, from `sleh.c`; it prints only non-zero buckets) read 12-85 `ldop`/`swp` per ten seconds at idle, where they
read 6,000-54,000 `ldop` and 400-2,000 `cas`/`casp` before, `STARLTE A64E` 0 emulated and no thread pinned to an A55. What is left is the root's
own executables (`launchd`, `locationd`, ... outside the cache) and one leaf in `libcorecrypto`. **Not measured:** frame rate under load, the cost
of the FP16 bodies in a hot loop, the 816 + 550 words without a body.

## Not done

The root's 284 executables (74 k sites: CommCenter, libglInterpose, locationd, ...) need a segment appended to each file and a re-signature;
those sites still trap into the kernel's emulation. The generators and the checker are not published yet (they carry census data of Apple
binaries); the method above is all there is to reproduce.
