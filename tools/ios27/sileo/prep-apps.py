#!/usr/bin/env python3
"""SPDX-License-Identifier: MIT-0
prep-apps.py PREFIX : give a package-manager prefix (a /jb or /private/var/jb of an iOS 27 root) the app-icon fix of
tools/ios27/sileo/scion-apps-sync.sh. Idempotent; used by 60-sileo.sh and by edits of a pulled phone root.
  PREFIX/usr/libexec/scion-apps-sync   the script (copied from next to this file)
  PREFIX/prep.sh                       once a boot: uicache diverted (dpkg-divert, so a uikittools upgrade keeps it) to a
                                       wrapper that runs scion-apps-sync first (as root), then scion-apps-sync itself
"""
import os, shutil, sys

if len(sys.argv) != 2:
    raise SystemExit(__doc__)
pre = sys.argv[1]
here = os.path.dirname(os.path.abspath(__file__))
dst = os.path.join(pre, 'usr/libexec/scion-apps-sync')
os.makedirs(os.path.dirname(dst), exist_ok=True)
shutil.copyfile(os.path.join(here, 'scion-apps-sync.sh'), dst)
os.chmod(dst, 0o755)

block = r'''# scion: the prefix's applications copied where LaunchServices registers them (usr/libexec/scion-apps-sync), at every
# boot and from uicache, which every package bringing an application runs: diverted once to a wrapper
if [ ! -e /var/jb/usr/bin/uicache.scion-real ] && [ -x /var/jb/usr/bin/uicache ]; then
	/var/jb/usr/bin/dpkg-divert --quiet --local --rename --divert /var/jb/usr/bin/uicache.scion-real --add /var/jb/usr/bin/uicache &&
	printf '#!/var/jb/bin/sh\n# scion (prep.sh): the copies in /Applications first (root only), then the real uicache\n[ "$(/var/jb/usr/bin/id -u)" != 0 ] || /var/jb/usr/libexec/scion-apps-sync >> /private/var/log/scion-apps-sync.log 2>&1\nexec /var/jb/usr/bin/uicache.scion-real "$@"\n' > /var/jb/usr/bin/uicache &&
	/var/jb/usr/bin/chmod 0755 /var/jb/usr/bin/uicache
fi
/var/jb/usr/libexec/scion-apps-sync >> /private/var/log/scion-apps-sync.log 2>&1
'''
p = os.path.join(pre, 'prep.sh')
s = open(p).read()
key = 'for app in /var/jb/Applications/*.app; do'
if block not in s:
    i = s.find('# scion s3:')
    if i < 0:
        i = s.find('# scion: copies of the prefix')
    if i < 0:
        i = s.find(key)
    assert i >= 0, 'prep.sh changed: no place for the block'
    s = s[:i] + block + s[i:]
    open(p, 'w').write(s)
    print('prep-apps: %s: scion-apps-sync and the uicache wrapper added' % pre)
else:
    print('prep-apps: %s: already there' % pre)
