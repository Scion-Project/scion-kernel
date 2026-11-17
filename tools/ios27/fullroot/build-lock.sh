#!/bin/bash
# SPDX-License-Identifier: MIT-0
# build-lock.sh : sourced by build-full-root.sh. One root build at a time per work directory and per image: two builds that
# share either attach the same mount points, stage into the same image and reuse the same cache at once, and both results
# are spoiled (it happened on 17 November 2026: the same command started twice, 4 s apart).
#   build_lock_take DIR WHAT   take the lock DIR (a directory, made atomically with mkdir; the holder's pid inside). A lock
#                              whose holder is gone is taken over, and said so. Returns 1, naming the holder, while it is alive.
#   build_lock_release DIR     remove DIR when this shell holds it; leave another's lock alone.
# build-lock-test.sh runs it (CI).
build_lock_take() {
	local d=$1 what=${2:-$1} pid try
	for try in 1 2; do
		if mkdir "$d" 2>/dev/null; then echo "$$" > "$d/pid"; return 0; fi
		pid=$(cat "$d/pid" 2>/dev/null || true)
		# a holder that made the directory but has not written its pid yet: give it a moment
		[[ -n $pid ]] || { sleep 1; pid=$(cat "$d/pid" 2>/dev/null || true); }
		if [[ -n $pid ]] && kill -0 "$pid" 2>/dev/null; then
			echo "build-lock: another build (pid $pid) is using $what ($d): refused; wait for it, or stop it first" >&2
			return 1
		fi
		[[ $try == 1 ]] || break
		echo "build-lock: $what was locked by pid ${pid:-unknown}, which is gone: taking it over" >&2
		rm -rf "$d"
	done
	echo "build-lock: could not take $d" >&2
	return 1
}
build_lock_release() {
	local d=$1
	[[ -d $d && "$(cat "$d/pid" 2>/dev/null || true)" == "$$" ]] || return 0
	rm -rf "$d"
}
