#!/var/jb/bin/sh
# SPDX-License-Identifier: MIT-0
# scion-apps-sync (tools/ios27/sileo/scion-apps-sync.sh, installed by 60-sileo.sh as /var/jb/usr/libexec/scion-apps-sync):
# the prefix's applications where LaunchServices registers them on this port. Run as root by prep.sh at every boot and by
# the uicache wrapper (the trigger of every package that brings an application), so an app installed from Sileo gets
# its home-screen icon.
#
# Why: on the S9's iOS 27, LaunchServices refuses uicache's registration of any bundle ("Unable to register", for
# /var/jb/Applications and /Applications alike; seen on phone 1, 13 Nov 2026), while lsd's own rebuild registers what is
# in /Applications. So every /var/jb/Applications/X.app gets a real copy /Applications/X.app, marked .scion-prefix-app,
# root's, nothing writable by others, a giveMeRoot set-user-ID again. A copy that differs from the prefix's (diff -r)
# is made again; a marked copy whose application is gone from the prefix is removed. iOS's own
# applications (no mark) are never touched. When anything changed, the LaunchServices databases are dropped and lsd
# is restarted, so it rebuilds them.
P=/var/jb/usr/bin
changed=0
for app in /var/jb/Applications/*.app; do
	[ -d "$app" ] || continue
	name=${app##*/}
	dst=/Applications/$name
	if [ -e "$dst" ] && [ ! -e "$dst/.scion-prefix-app" ]; then
		continue                                        # one of iOS's own: never touched
	fi
	if [ -e "$dst/.scion-prefix-app" ] && $P/diff -rq -x .scion-prefix-app "$app" "$dst" > /dev/null 2>&1; then
		continue                                        # the same as the prefix's
	fi
	$P/rm -rf "$dst.scion-new"
	if $P/cp -a "$app" "$dst.scion-new" && : > "$dst.scion-new/.scion-prefix-app" &&
	    /var/jb/usr/sbin/chown -R 0:0 "$dst.scion-new" && $P/chmod -R go-w "$dst.scion-new"; then
		[ ! -f "$dst.scion-new/giveMeRoot" ] || $P/chmod 4755 "$dst.scion-new/giveMeRoot"
		$P/rm -rf "$dst" && $P/mv "$dst.scion-new" "$dst" && changed=1 && echo "scion-apps-sync: $name copied to /Applications"
	else
		$P/rm -rf "$dst.scion-new"
		echo "scion-apps-sync: $name could not be copied"
	fi
done
for dst in /Applications/*.app; do
	[ -e "$dst/.scion-prefix-app" ] || continue
	[ -d "/var/jb/Applications/${dst##*/}" ] && continue
	$P/rm -rf "$dst" && changed=1 && echo "scion-apps-sync: ${dst##*/} removed from /Applications (its package is gone)"
done
if [ $changed = 1 ]; then
	for c in /private/var/mobile/Containers/Data/InternalDaemon/*/Library/Caches/*com.apple.LaunchServices-*.csstore; do
		[ -f "$c" ] && $P/rm -f "$c"
	done
	$P/killall -9 lsd 2>/dev/null
	echo "scion-apps-sync: LaunchServices databases dropped, lsd restarted (it rebuilds them)"
fi
exit 0
