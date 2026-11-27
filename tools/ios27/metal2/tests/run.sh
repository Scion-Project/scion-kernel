#!/bin/sh
# tools/ios27/metal2/tests/run.sh: the host checks of the Metal frontend's compile rules (CompileQueue.h).
#   1. compile-queue-test.cpp (plain C++17 + threads; macOS or Linux), also under ThreadSanitizer when the compiler has it
#   2. the frontend patch routes every pipeline creation through gCompiler and has no timeout refusal left in it
set -e
D=$(cd "$(dirname "$0")" && pwd); M=$(cd "$D/.." && pwd); P=$(cd "$M/../metal/patches" && pwd)
T=${TMPDIR:-/tmp}/sb-compile-queue-test.$$
CXX=${CXX:-c++}
trap 'rm -f "$T" "$T.tsan" "$T.out"' EXIT
$CXX -std=c++17 -O1 -g -Wall -Wextra -Wno-unused-parameter -pthread -o "$T" "$D/compile-queue-test.cpp"
"$T"
if $CXX -std=c++17 -O1 -g -fsanitize=thread -pthread -o "$T.tsan" "$D/compile-queue-test.cpp" 2>/dev/null; then
	echo "-- again under ThreadSanitizer"
	# Linux kernels with a large mmap randomisation (ubuntu 24.04) stop TSan before main ("unexpected memory mapping"):
	# then run it without address randomisation (setarch -R), and skip only if that is not possible either.
	if ! TSAN_OPTIONS=halt_on_error=1 "$T.tsan" > "$T.out" 2>&1; then
		if grep -q 'unexpected memory mapping' "$T.out" && command -v setarch >/dev/null 2>&1; then
			TSAN_OPTIONS=halt_on_error=1 setarch "$(uname -m)" -R "$T.tsan" > "$T.out" 2>&1 || {
				if grep -q 'unexpected memory mapping' "$T.out"; then echo "-- (ThreadSanitizer cannot start on this kernel: skipped)"; else cat "$T.out"; exit 1; fi; }
		elif grep -q 'unexpected memory mapping' "$T.out"; then
			echo "-- (ThreadSanitizer cannot start on this kernel: skipped)"
		else
			cat "$T.out"; exit 1
		fi
	fi
	tail -1 "$T.out"
else
	echo "-- (no ThreadSanitizer in $CXX: skipped)"
fi
F=${FRONTEND_DIFF:-$P/frontend-async-compile.diff}   # FRONTEND_DIFF=<file>: check another version of the patch
fail=0
# the frontend's synchronous creation paths: no wait with a time limit and no "ask again" refusal (#396 failure 1)
if grep -nE '^\+.*(wait_for\(|still running after|ask again|COMPILE_TIMEOUT_MS")' "$F"; then
	echo "frontend-async-compile.diff: a timed wait or a timeout refusal is back in the frontend (see CompileQueue.h rule 1)"; fail=1
fi
# every place that makes a pipeline does it inside gCompiler.sync (synchronous) or gCompiler.async (completion handler)
n_new=$(grep -cE '^\+.*nmb_(render|compute)_pipeline_new' "$F" || true)
n_old=$(grep -cE '^-.*nmb_(render|compute)_pipeline_new' "$F" || true)
n_sync=$(grep -cE '^\+.*gCompiler\.sync\(' "$F" || true)
if [ "$n_old" -ne 3 ] || [ "$n_new" -ne 3 ] || [ "$n_sync" -ne 3 ]; then
	echo "frontend-async-compile.diff: expected the 3 pipeline makers moved into 3 gCompiler.sync calls (old $n_old new $n_new sync $n_sync)"; fail=1
fi
n_handlers=$(grep -cE '^\+.*gCompiler\.async\(' "$F" || true)
if [ "$n_handlers" -ne 6 ]; then
	echo "frontend-async-compile.diff: expected the 6 completion-handler methods on gCompiler.async (found $n_handlers)"; fail=1
fi
[ $fail = 0 ] && echo "frontend-async-compile.diff: rules hold (3 sync makers, 6 async handlers, no timed wait)"
exit $fail
