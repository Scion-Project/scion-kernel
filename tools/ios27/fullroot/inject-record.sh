#!/bin/bash
# SPDX-License-Identifier: MIT-0
# inject-record.sh ROOT RECORD : the record of the libraries the root's feature steps installed for the kernel to insert
# into programs (sweetinject=), written to RECORD (the root build: next to the image, out/root-full.img.inject).
# make-boot-image.sh reads it and puts exactly these libraries, after the kernel's built-in three, on the boot image's
# command line; a library that is not on the root must never be named there (dyld then refuses to start the program, and
# with backboardd or SpringBoard the phone has no interface at all). So an entry whose library is not on the root, is not a
# Mach-O, or has a name the command line cannot carry is left out here, loudly.
# The entries are the files of ROOT/usr/local/share/scion/sweetinject.d/ (fullroot/feature.sh, feature_inject), one line
# each: LIBRARY PROGRAM...  The record holds the same lines, sorted, after comment lines.
set -euo pipefail
R=${1:?usage: inject-record.sh ROOT RECORD}; OUT=${2:?usage: inject-record.sh ROOT RECORD}
D=$R/usr/local/share/scion/sweetinject.d
T=$OUT.tmp.$$
{
	echo "# sweetinject record: the libraries this root has for the kernel to insert, LIBRARY PROGRAM... (inject-record.sh)"
	echo "# made $(date -u +%FT%TZ); make-boot-image.sh --inject-record adds them to the kernel's built-in sweetinject="
} > "$T"
n=0; bad=0
for e in "$D"/*; do
	[[ -f $e ]] || continue
	read -r lib progs < "$e" || true
	why=
	if [[ ! $lib =~ ^/[A-Za-z0-9._/+-]+$ ]]; then why="the library's name '$lib' cannot go on the command line"
	elif [[ -z ${progs:-} || ! $progs =~ ^[A-Za-z0-9._+-]+( [A-Za-z0-9._+-]+)*$ ]]; then why="no program, or a program name the command line cannot carry: '${progs:-}'"
	elif [[ ! -f $R$lib ]]; then why="$lib is not on the root"
	else
		case $(od -An -tx1 -N4 "$R$lib" | tr -d ' \n') in
		cffaedfe | cafebabe) ;;
		*) why="$lib is not a Mach-O" ;;
		esac
	fi
	if [[ -n $why ]]; then
		echo "inject-record: LEFT OUT ${e##*/}: $why" >&2; bad=$((bad + 1)); continue
	fi
	echo "$lib $progs"; n=$((n + 1))
done | LC_ALL=C sort >> "$T"
# the kernel's built-in three are named whether or not the root has them: say so when it does not
for l in s m b; do [[ -f $R/usr/lib/$l.dylib ]] || echo "inject-record: note: /usr/lib/$l.dylib (in the kernel's built-in sweetinject=) is not on this root" >&2; done
mv -f "$T" "$OUT"
echo "inject-record: $OUT: $(grep -vc '^#' "$OUT" || true) librar$( [[ $(grep -vc '^#' "$OUT" || true) == 1 ]] && echo y || echo ies) to insert"
