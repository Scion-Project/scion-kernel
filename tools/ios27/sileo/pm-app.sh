# SPDX-License-Identifier: MIT-0
# pm-app.sh : sourced by tools/ios27/fullroot/steps.d/60-sileo.sh and 61-zebra.sh, the steps of the package managers.
# A package manager's app from the prefix the root carries (/jb, put there by 60-sileo.sh) as a real copy in /Applications:
# on this port uicache cannot register any bundle ("Unable to register"; docs/ios27/SILEO.md) while lsd's rebuild at boot
# registers what is in /Applications. The copy is marked .scion-prefix-app, so that prep.sh makes it root's at every
# boot (and a giveMeRoot in it set-user-ID) and scion-apps-sync keeps it the same as the prefix's.

# pm_app_put ROOT NAME.app : the copy made (again); stops if /Applications has a NAME.app that is not such a copy
pm_app_put() {
	local R=$1 n=$2
	[[ -d $R/jb/Applications/$n ]] || { echo "pm-app: no $n in the prefix at $R/jb" >&2; return 1; }
	if [[ -e $R/Applications/$n && ! -e $R/Applications/$n/.scion-prefix-app ]]; then
		echo "pm-app: a $n that is not the prefix's copy is in /Applications" >&2
		return 1
	fi
	rm -rf "$R/Applications/$n"
	cp -a "$R/jb/Applications/$n" "$R/Applications/$n"
	: > "$R/Applications/$n/.scion-prefix-app"
	if [[ $EUID == 0 ]]; then
		chown -R 0:0 "$R/Applications/$n"
		[[ ! -f $R/Applications/$n/giveMeRoot ]] || chmod 4755 "$R/Applications/$n/giveMeRoot"
	fi
	pm_ls_drop "$R"
}

# pm_app_take ROOT NAME.app : a marked copy taken off (an app of iOS's own is never touched)
pm_app_take() {
	if [[ -e $1/Applications/$2/.scion-prefix-app ]]; then
		rm -rf "$1/Applications/$2"
		pm_ls_drop "$1"
	fi
	return 0
}

# pm_ls_drop ROOT : the LaunchServices databases dropped, so that lsd rebuilds them at the next boot
pm_ls_drop() {
	local c
	for c in "$1"/private/var/mobile/Containers/Data/InternalDaemon/*/Library/Caches/*com.apple.LaunchServices-*.csstore; do
		[[ -f $c ]] && rm -f "$c" && echo "pm-app: LaunchServices database dropped (lsd rebuilds it at the next boot): ${c##*/}"
	done
	return 0
}
