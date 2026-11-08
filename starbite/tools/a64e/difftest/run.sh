#!/usr/bin/env bash
# Differential test of the kernel's FHM and SHA3/SHA512 emulation (sweet_a64e.c, patches
# 0351/0352) against the real instructions of this Mac (Apple M1+ has FEAT_FHM, SHA3, SHA512).
# The emulation code is cut out of the kernel source unchanged and run in user space with a
# mocked thread NEON state; the FMADD helper is sweet_a64e_fp_fmadd_s without the DAIF lines
# (EL0 cannot mask interrupts).
#   tools/a64e/difftest/run.sh [path/to/sweet_a64e.c] [FHM iterations per encoding, default 20000]
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
src=${1:-$here/../../../build/xnu-12377/osfmk/arm64/sweet_a64e.c}
iters=${2:-20000}
work=$(mktemp -d "${TMPDIR:-/tmp}/a64e-difftest.XXXXXX")
trap 'rm -rf "$work"' EXIT
cut() { python3 -c 'import sys; s=open(sys.argv[1]).read(); a=s.index(sys.argv[2]); print(s[a:s.index(sys.argv[3], a)])' "$src" "$1" "$2"; }
cut '/* ---- FEAT_FHM' '/* ---- FEAT_SHA3 (EOR3' > "$work/fhm_part.c"
cut '/* ---- FEAT_SHA3 (EOR3' '/* ---- LSE and LRCPC' > "$work/sha_part.c"
cp "$here"/fhm_test.c "$here"/sha_test.c "$here"/fmadd_s.s "$work"/
cd "$work"
clang -O1 -w -march=armv8.5-a+fp16fml -o fhm_test fhm_test.c fmadd_s.s
clang -O1 -w -march=armv8.5-a+sha3 -o sha_test sha_test.c
./fhm_test "$iters"
./sha_test
