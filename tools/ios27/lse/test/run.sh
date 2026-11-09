#!/usr/bin/env bash
# Differential test of tools/ios27/lse/lse-rewrite.py on an Apple Silicon Mac (it has both LSE and LL/SC, so the same program
# can run natively and rewritten): gen-test.py writes one function per LSE instruction variant (1520: every operation, B/H/W/X,
# the four orderings, scratch-register clashes, Rs/Rt = ZR, Rn = SP, Rs = Rt); driver.c runs each six times with random registers,
# flags and memory and prints registers, NZCV, SP and the memory it changed, then 8 threads stress the atomics (LDADDAL, CASAL,
# CASPAL, an SWPA spin lock, LDSMAXAL). The rewritten binary must print exactly what the original prints (the lines that
# differ are the cases whose Rs is Rn: they print ASLR'd pointer bits).
#   tools/ios27/lse/test/run.sh [STRESS 0|1, default 1]      output in build/lse-test/
set -euo pipefail
HERE=$(cd "$(dirname "$0")" && pwd)
OUT=$HERE/../../../../build/lse-test
mkdir -p "$OUT"; cd "$OUT"
PY=${PY:-python3}
"$PY" -I "$HERE/gen-test.py" .
clang -O1 -o lsetest -I. "$HERE/driver.c" lsetest.s -lpthread
cp lsetest lsetest-rw
"$PY" -I "$HERE/../lse-rewrite.py" macho lsetest-rw --pool-sym _stub_pool --log rw.log 2>&1 | tail -3
codesign -f -s - lsetest-rw 2>&1 | tail -1
( time ./lsetest "${1:-1}" > out-orig.txt ) 2>&1 | grep real
( time ./lsetest-rw "${1:-1}" > out-rw.txt ) 2>&1 | grep real
wc -l out-orig.txt out-rw.txt; grep STRESS out-orig.txt out-rw.txt || true
diff <(grep -v STRESS out-orig.txt) <(grep -v STRESS out-rw.txt) > diff.txt || true
echo "diff lines: $(grep -c '^<' diff.txt)"; head -10 diff.txt
