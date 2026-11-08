#!/usr/bin/env python3
"""Send launchd jobs' os_log to a file (ACTIVITY_LOG_STDERR=1 + StandardErrorPath) on an attached
iOS root, by label: /private/var/tmp/<name>.log. --off takes it out again.
   starlte-job-log.py /Volumes/IOSROOT com.apple.backboardd com.apple.SpringBoard [--off]
(StandardErrorPath=/dev/console kept backboardd from starting on iPadOS 18; use files.)"""
import plistlib, sys
root, labels = sys.argv[1], [a for a in sys.argv[2:] if not a.startswith('--')]
off = '--off' in sys.argv
path = root + '/System/Library/xpc/launchd.plist'
d = plistlib.load(open(path, 'rb'))
done = []
for key, job in d['LaunchDaemons'].items():
    if job.get('Label') in labels:
        env = job.setdefault('EnvironmentVariables', {})
        name = job['Label'].split('.')[-1]
        if off:
            env.pop('ACTIVITY_LOG_STDERR', None)
            job.pop('StandardErrorPath', None)
        else:
            env['ACTIVITY_LOG_STDERR'] = '1'
            job['StandardErrorPath'] = f'/private/var/tmp/{name}.log'
        done.append(job['Label'])
with open(path, 'r+b') as f:
    f.truncate(0)
    plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
print(('logging off: ' if off else 'logging on: ') + ' '.join(done), '| missing:', set(labels) - set(done) or '-')
