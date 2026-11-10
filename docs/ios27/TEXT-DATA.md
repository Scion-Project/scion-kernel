# Data inside `__text`: what the instruction rewrite must not touch

The cache pipeline (`tools/ios27/fullroot/pipeline/cache-pipeline.sh`, `docs/ios27/AOT-REWRITE.md`) makes iOS 27's shared
cache runnable on the Exynos M3 (ARMv8.0) by rewriting, ahead of time, every instruction that core lacks. The passes
find their sites by bit pattern: every 32-bit word of the pure-instruction sections that decodes as an LSE atomic, an
FP16 operation, a SHA3 / LRCPC / LOR instruction, ... is replaced by a branch to a thunk (or rewritten in place).

Not every word of those sections is an instruction. Hand-written assembly keeps its tables next to its code: bit
masks, polynomial coefficients of the math library, hash round constants, transform tables of codecs, ident strings.
A table entry that happens to have the bit pattern of a rewritten instruction was rewritten like one:

| table entry | what it is | what the decoder saw |
|---|---|---|
| `0x1fffffff` | a 29-bit mask (and the upper half of a 61-bit one) in an entropy decoder's mask table | `FNMSUB H31, H31, H31, H31` (FP16) |
| `0x78a5636f` | a SHA-256 round constant | `LDUMAXAH W5, W15, [X27]` (LSE) |
| words of `double` coefficient tables | sin/cos, tan, exp, log, tanh tables of the math library | SHA3, LSE, LRCPC, FP16 |

Nothing traps: the table simply holds a branch opcode where a constant was, and the function that reads it computes
garbage. On the phone that was, among others:

- **LZFSE buffer decoding returned wrong bytes** whenever the literal decoder's bit accumulator held exactly 29 or 61
  bits (two of 64 mask entries damaged). Output length right, content wrong. Every reader of compressed asset
  catalogs got damaged data: layered app icons came out as the placeholder (their vector layers no longer parsed) or
  as striped garbage (bitmap layers), deterministically, byte for byte the same at every boot. Restoring the two
  words alone brought back Mail, Clock, News, App Store, Wallet, Settings, Safari and TV on the home screen.
- wrong results from several math-library functions (their coefficient tables), wrong SHA-256 digests from two
  private SHA-256 implementations, a damaged transform table in an audio codec and in a software video codec.

## The guard

`pipeline/cache/textdata-guard.py` compares the stock cache with the rewritten one and reports every changed word
that is **not provably an instruction**. It needs one input besides the two caches: the list of words no A64 decoder
accepts (`undef.txt`, what `oracle.sh` writes when it runs llvm-objdump with every feature on over the cache's distinct
words). Two rules, either one flags the word:

- **C**: an undecodable word within 8 words before *and* within 8 words after it.
- **D**: walking back from it over decodable words, the first other thing met is an undecodable word, not the start
  of its function (`LC_FUNCTION_STARTS`) and not an instruction that ends a block (`B`, `BR`, `RET`, `BRK`, ...). Code
  is entered at a function start or after such an instruction; a table entry sits behind other table entries.

Exact pointer-authentication opcodes are left out unless `--pac` is given (a table word is not exactly `RETAB` by
accident, and code next to vendor-specific instructions the decoder does not know would flood the report).

    textdata-guard.py scan  STOCK/dyld_shared_cache_arm64e NEW/dyld_shared_cache_arm64e --undef undef.txt > candidates.tsv
    textdata-guard.py apply NEW_CACHE_DIR textdata-reviewed.list            # idempotent; --revert damages again (A/B test)
    textdata-guard.py check STOCK/... NEW/... --undef undef.txt --reviewed textdata-reviewed.list   # exit 1 on an unreviewed candidate

On the 24A446 cache `scan` gives 145 candidates out of 33.7 million changed words. A person reads them once: 40
are data (the table above), 105 are code (one- or two-instruction functions between two literal pools or runs of
zero padding, and code after vendor-specific instructions). The reviewed list has a line per candidate,
`VADDR DAMAGED STOCK note` for data and `VADDR code note` for code; it holds addresses and words of the vendor's
cache, so it lives in the builder's private directory (`$IOS27_PRIVATE/textdata-reviewed.list`, next to
`patches.txt`), not in this repository.

`cache-pipeline.sh` runs it as **step 8**: the reviewed data words are put back, then `check` must find nothing new;
a build of another cache version stops there until its candidates are reviewed (`ALLOW_UNCHECKED_TEXTDATA=1` skips,
loudly). The thunks of the restored sites stay in the cache, unreachable. Applied to the cache as built before the
step existed, the 40 restored words are the whole difference (147 bytes in 6 subcache files).

Roots that already carry a cache built without step 8: `steps.d/85-textdata-restore.sh ROOT` (the same `apply`, on
the mounted root, in place).

## What the rules cannot see

A single constant between a `ret` and the next function start, with decodable neighbours on both sides, is not
flagged by C or D. None is known; a constant of that kind that also has the bit pattern of a rewritten instruction
would have to be found by its symptom. The principled fix is a real code/data map (recursive descent from the
function starts, jump tables resolved) inside the rewrite passes themselves, so that they never leave a function's
reachable code; the guard is the check that such a map would have to pass as well.

## The second cause of the empty Settings rows (not the rewrite)

With LZFSE repaired the home screen was right, but thirteen symbol rows of Settings (Wi-Fi, Bluetooth, Battery,
Camera, Control Center, Home Screen, StandBy, Sounds, Focus, Screen Time, Passcode, Emergency SOS, Privacy) were
still empty. The icon agent's own log (os_log capture) names the step: CoreUI "failed to produce foreground layer"
for exactly those thirteen symbols. CoreUI draws a symbol whose glyph is not square oversampled and scales it down
with a Core Image filter, through a default `CIContext`; square glyphs skip the filter, which is why General and
Accessibility were drawn and Wi-Fi and Battery were not. A default `CIContext` is Metal, else OpenGL ES, else
**nil**: only a widget extension or an explicit software request gets the software renderer. The icon agent has
neither a Metal device nor OpenGL ES on this port ("No Metal renderer available", "No OpenGLES renderer available"),
got nil, and the icon was never made (the failure is cached as an empty image).

Two ways out were tried on the phone:

- the agent as a Metal client (client list + the injection that lets the arm64 Metal plug-in load): the tiles are
  drawn, but the glyphs come out empty and one app icon changes for the worse - the Metal driver's problem, open;
- one word of CoreImage in the shared cache (the builder's private word list, like `patches.txt`): "Metal is not
  available" goes to the software context instead of on to OpenCL / OpenGL ES / nil. All thirteen rows are drawn.
  Root step: `steps.d/86-coreimage-sw-context.sh` (private list; this repository carries only the mechanism).

Side effect to know: every process without a Metal device now gets a software Core Image context where it got
none, so work that silently did not happen before is done on the CPU.
