#!/bin/bash
# SPDX-License-Identifier: MIT-0
# build-lock-test.sh : build-lock.sh, the one-build-at-a-time lock of build-full-root.sh, in a scratch directory (removed at the
# end): a free lock is taken; while its holder lives a second build is refused and the lock stays the holder's; a stranger's
# release leaves it; a lock whose holder is gone is taken over; the holder's release removes it. CI runs it (Linux); macOS too.
set -euo pipefail
H=$(cd "$(dirname "$0")" && pwd)
T=$(mktemp -d "${TMPDIR:-/tmp}/locktest.XXXXXX"); trap 'kill "${holder:-}" 2>/dev/null || true; rm -rf "$T"' EXIT
fail=0
ok() { echo "ok   $1"; }
bad() { echo "FAIL $1"; fail=1; }
L=$T/work/build.lock; mkdir -p "$T/work"
other() { bash -c '. "$1"; build_lock_take "$2" "the work directory"' _ "$H/build-lock.sh" "$L"; }    # another build: its own pid
otherrel() { bash -c '. "$1"; build_lock_release "$2"' _ "$H/build-lock.sh" "$L"; }
. "$H/build-lock.sh"

build_lock_take "$L" "the work directory" && [[ $(cat "$L/pid") == "$$" ]] && ok "a free lock is taken, with this shell's pid" || bad "a free lock is taken"
if other 2> "$T/err"; then bad "a second build is refused while the first lives"; else
	grep -q "pid $$" "$T/err" && ok "a second build is refused while the first lives, the holder named" || bad "the refusal names the holder: $(cat "$T/err")"; fi
[[ $(cat "$L/pid") == "$$" ]] && ok "the refused build left the lock alone" || bad "the refused build left the lock alone"
otherrel; [[ -d $L ]] && ok "another's release leaves the lock" || bad "another's release leaves the lock"
build_lock_release "$L"; [[ ! -e $L ]] && ok "the holder's release removes it" || bad "the holder's release removes it"

# a holder that is alive, then gone
sleep 60 & holder=$!
mkdir "$L"; echo "$holder" > "$L/pid"
if build_lock_take "$L" "the work directory" 2>/dev/null; then bad "a live holder's lock is refused"; else ok "a live holder's lock is refused"; fi
kill "$holder"; wait "$holder" 2>/dev/null || true
if build_lock_take "$L" "the work directory" 2> "$T/err" && [[ $(cat "$L/pid") == "$$" ]]; then
	grep -q "which is gone: taking it over" "$T/err" && ok "a lock whose holder is gone is taken over, and said" || bad "the takeover is said"
else bad "a lock whose holder is gone is taken over"; fi
build_lock_release "$L"
# a lock directory with no pid (a holder between mkdir and its pid, which then died): taken over after the wait
mkdir "$L"; build_lock_take "$L" "the work directory" 2>/dev/null && ok "a lock without a pid is taken over after a moment" || bad "a lock without a pid is taken over"
build_lock_release "$L"
# build-full-root.sh takes both locks before anything else of a build, and its cleanup releases them
B=$H/build-full-root.sh
a=$(grep -n 'build_lock_take "$W/build.lock"' "$B" | cut -d: -f1); b=$(grep -n 'build_lock_take "$OUT.lock"' "$B" | cut -d: -f1); f=$(grep -n 'fw_prepare ||' "$B" | head -1 | cut -d: -f1)
[[ -n $a && -n $b && -n $f && $a -lt $f && $b -lt $f ]] && ok "build-full-root.sh build takes the work directory's and the image's locks before the firmware" || bad "build-full-root.sh takes both locks first"
grep -q '^    cleanup() {.*build_lock_release "$OUT.lock".*build_lock_release "$W/build.lock"' "$B" && ok "its cleanup releases both" || bad "its cleanup releases both"
[[ $fail == 0 ]] && echo "build-lock-test: all passed" || { echo "build-lock-test: FAILED"; exit 1; }
