#!/bin/bash
# SPDX-License-Identifier: MIT-0
# 45-media-library.sh ROOT [undo] : the media library's directory, /private/var/mobile/Media/iTunes_Control with iTunes/ and Music/,
# owned by mobile (501), as a restore makes it on an iPhone. The IPSW's root does not have it and this port never restores, so it was
# missing on every iOS 27 root (24A446 and 24B156 alike).
#
# Why (iOS 27.2.1 24B156's MusicLibrary, its logic only; docs/ios27/SETTINGS.md): when a database connection cannot be opened,
# -[ML3DatabaseConnection _raiseConnectionClosedException] first runs ML3DatabaseConnectionCheckDatabasePathAndRaiseIfNecessary, which
# checks the media container, then iTunes_Control (ML3DatabaseConnectionCheckPathiTunesControl: it must exist, have attributes, not be
# owned by root and be owned by the caller's user), then iTunes_Control/iTunes, then the database file, and raises an Objective-C
# exception ("<path> does not exist.") for the first that fails. -[ML3MusicLibrary libraryUID] reads a database property through
# -[ML3MusicLibrary databaseConnectionAllowingWrites:withBlock:], which rethrows; nothing above catches it, so the process aborts.
# Settings (Preferences) aborts that way on 24B156 (phone 1, 18 Nov: three boots, the same ten frames each time), and atc aborts that
# way 8 to 17 times a boot on both builds. A writer connection creates iTunes_Control/iTunes and the database itself
# (-[ML3DatabaseConnection _createDatabaseDirectoryIfNonexistent] / _createDatabaseFileIfNonexistent) once iTunes_Control is there.
#
# Default ON: it is a correction (a directory every iPhone has), not new behaviour. ENABLE_MEDIA_LIBRARY_DIRS=0 turns it off.
# "undo" removes the directories only when they hold nothing (a database made on the phone is never deleted here).
# Idempotent. NOT RUN ON A PHONE yet.
set -euo pipefail
R=${1:?usage: 45-media-library.sh ROOT [undo]}
[[ ${ENABLE_MEDIA_LIBRARY_DIRS:-1} == 1 ]] || { echo "45-media-library: off (ENABLE_MEDIA_LIBRARY_DIRS=0)"; exit 0; }
MEDIA=$R/private/var/mobile/Media
M=$MEDIA/iTunes_Control
[[ -d $MEDIA ]] || { echo "45-media-library: $R has no /private/var/mobile/Media" >&2; exit 1; }
if [[ ${2:-} == undo ]]; then
	rmdir "$M/iTunes" "$M/Music" "$M" 2>/dev/null || true
	echo "45-media-library: iTunes_Control $([[ -e $M ]] && echo 'kept (not empty)' || echo removed)"
	exit 0
fi
before=$([[ -d $M ]] && echo there || echo absent)
mkdir -p "$M/iTunes" "$M/Music"
chmod 0755 "$M" "$M/iTunes" "$M/Music"
# MusicLibrary refuses a directory owned by root, and one owned by another user than the reader (mobile, 501)
if [[ $EUID == 0 ]]; then
	chown 501:501 "$M" "$M/iTunes" "$M/Music"
fi
owner=$(stat -f %u "$M" 2>/dev/null || stat -c %u "$M")
[[ $owner == 501 ]] || { echo "45-media-library: $M is owned by $owner, not mobile (501): run the build as root" >&2; exit 1; }
echo "45-media-library: iTunes_Control, iTunes_Control/iTunes, iTunes_Control/Music owned by mobile (was $before)"
