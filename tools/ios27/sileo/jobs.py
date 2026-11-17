#!/usr/bin/env python3
"""SPDX-License-Identifier: MIT-0
jobs.py ROOT add [--selftest]|remove|list : the launchd jobs of the package manager's prefix on an attached iOS 27 root. iOS's launchd
reads its jobs from one file, /System/Library/xpc/launchd.plist, not from the directories, so they are entries there
(written in place: the file keeps its owner and mode).
  com.scion.jbseed    /usr/local/bin/jb_seed, once a boot: the image's /jb copied to /private/var/jb the first time, then
                      prep.sh (dpkg's "firmware" record, Sileo and the prefix's applications registered with uicache)
  com.scion.varsync   /usr/local/bin/var_sync 5, for the whole run: the volume to the flash every 5 s
  com.scion.sileotest only with --selftest (a test root): tools/ios27/sileo/selftest.sh, once a boot. On a first boot
                      there is no /var/jb/bin/sh yet when launchd loads it; WatchPaths starts it when prep.sh has made
                      /private/var/jb/.prepared
All run as root, none listens on anything, both write to a log in /private/var/log (not to /dev/console: a job with
that as its output kept backboardd from starting on this port). remove takes all three out."""
import plistlib, sys

PREFIX = '/System/Library/LaunchDaemons/'
JOBS = {
    'com.scion.jbseed': {
        'Label': 'com.scion.jbseed',
        'ProgramArguments': ['/usr/local/bin/jb_seed'],
        'RunAtLoad': True,
        'AbandonProcessGroup': True,         # prep.sh leaves its registrations of applications running aside
        'LimitLoadToSessionType': 'System',
        'ProcessType': 'Background',
        'StandardOutPath': '/private/var/log/scion-jbseed.log',
        'StandardErrorPath': '/private/var/log/scion-jbseed.log',
    },
    'com.scion.varsync': {
        'Label': 'com.scion.varsync',
        'ProgramArguments': ['/usr/local/bin/var_sync', '5'],
        'RunAtLoad': True,
        'KeepAlive': True,
        'LimitLoadToSessionType': 'System',
        'ProcessType': 'Background',
        'StandardOutPath': '/private/var/log/scion-varsync.log',
        'StandardErrorPath': '/private/var/log/scion-varsync.log',
    },
}

TEST = {
    'com.scion.sileotest': {
        'Label': 'com.scion.sileotest',
        'ProgramArguments': ['/var/jb/bin/sh', '/usr/local/libexec/scion-sileo-selftest.sh'],
        'RunAtLoad': True,
        'WatchPaths': ['/private/var/jb/.prepared'],
        'LimitLoadToSessionType': 'System',
        'ProcessType': 'Background',
    },
}

def main():
    selftest = '--selftest' in sys.argv
    if selftest:
        sys.argv.remove('--selftest')
    if len(sys.argv) != 3 or sys.argv[2] not in ('add', 'remove', 'list') or (selftest and sys.argv[2] != 'add'):
        raise SystemExit(__doc__)
    if selftest or sys.argv[2] != 'add':
        JOBS.update(TEST)
    path = sys.argv[1] + '/System/Library/xpc/launchd.plist'
    with open(path, 'rb') as f:
        d = plistlib.load(f)
    jobs = d['LaunchDaemons']
    ours = {PREFIX + label + '.plist': job for label, job in JOBS.items()}
    if sys.argv[2] == 'list':
        for key in ours:
            print(key, 'present' if key in jobs else 'absent')
        return
    # a label of ours under some other key would be a second job of the same name: stop
    for key, job in jobs.items():
        if isinstance(job, dict) and job.get('Label') in JOBS and key not in ours:
            raise SystemExit('jobs.py: %s is in the file already as %s' % (job.get('Label'), key))
    before = dict(jobs)
    for key, job in ours.items():
        if sys.argv[2] == 'add':
            jobs[key] = job
        else:
            jobs.pop(key, None)
    if sys.argv[2] == 'add' and not selftest:           # a root that had the test job and is made again without it
        jobs.pop(PREFIX + 'com.scion.sileotest.plist', None)
    if jobs == before:
        print('jobs.py: nothing to change (%s)' % sys.argv[2])
        return
    data = plistlib.dumps(d, fmt=plistlib.FMT_BINARY)
    plistlib.loads(data)
    with open(path, 'r+b') as f:
        f.truncate(0)
        f.write(data)
    print('jobs.py: %s: %s' % (sys.argv[2], ', '.join(JOBS)))

main()
