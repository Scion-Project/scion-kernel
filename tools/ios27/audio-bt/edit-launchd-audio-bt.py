#!/usr/bin/env python3
"""edit-launchd-audio-bt.py ROOT [OS_LAUNCHD_PLIST] [--undo]: put the audio server and Bluetooth jobs back
into an iOS 27 root's /System/Library/xpc/launchd.plist for the Galaxy S9 (Scion, private iOS 27 port), idempotently:
  com.apple.audiomxd   as Apple's (it hosts CoreAudio's HAL, which loads HakiosAudio.driver and BTAudioHALPlugin.driver)
  com.apple.bluetoothd started through /usr/libexec/hakios-inject with /usr/lib/hakios.bluetoothd.dylib, and
                       BT_HCI_TRANSPORT=H4:POSIX:com.apple.uart.bluetooth,4000000 (the kernel's bt=7 UART control)
  com.apple.BlueTool   started through hakios-inject with /usr/lib/hakios.bluetool.dylib (ReleaseType Vendor: our scripts)
A job missing from launchd.plist is taken from launchd-disabled.plist beside it (where the S9's tune scripts keep the jobs
they take out), else from OS_LAUNCHD_PLIST (the IPSW's own launchd.plist).
Where each job was before the first run, and as what, is kept in /usr/local/share/scion/audio-bt-jobs.plist;
--undo puts every job back as it was (in launchd.plist, in launchd-disabled.plist, or nowhere) and deletes that file."""
import os, plistlib, sys
args = [a for a in sys.argv[1:] if not a.startswith('--')]
UNDO = '--undo' in sys.argv
root = args[0]
LOG = os.environ.get('AUDIO_BT_LOG') == '1'   # test runs: stderr and os_log mirrored into /private/var/local/oslog/
xpc = root + '/System/Library/xpc/'
STATE = root + '/usr/local/share/scion/audio-bt-jobs.plist'
d = plistlib.load(open(xpc + 'launchd.plist', 'rb'))
try:
    kept = plistlib.load(open(xpc + 'launchd-disabled.plist', 'rb'))
except FileNotFoundError:
    kept = None
jobs = d['LaunchDaemons']
INJ = '/usr/libexec/hakios-inject'
want = {
    '/System/Library/LaunchDaemons/com.apple.audiomxd.plist': None,
    '/System/Library/LaunchDaemons/com.apple.bluetoothd.plist': '/usr/lib/hakios.bluetoothd.dylib',
    '/System/Library/LaunchDaemons/com.apple.BlueTool.plist': '/usr/lib/hakios.bluetool.dylib',
}


def save():
    with open(xpc + 'launchd.plist', 'r+b') as f:
        f.truncate(0); plistlib.dump(d, f, fmt=plistlib.FMT_BINARY)
    if kept is not None:
        with open(xpc + 'launchd-disabled.plist', 'r+b') as f:
            f.truncate(0); plistlib.dump(kept, f, fmt=plistlib.FMT_BINARY)


if UNDO:
    if not os.path.exists(STATE):
        print('edit-launchd-audio-bt: no record of the jobs as they were (the step never changed them): unchanged')
        sys.exit(0)
    state = plistlib.load(open(STATE, 'rb'))
    for key, was in state.items():
        jobs.pop(key, None)
        if kept is not None:
            kept.pop(key, None)
        if was['where'] == 'launchd':
            jobs[key] = was['job']
        elif was['where'] == 'disabled' and kept is not None:
            kept[key] = was['job']
        print(f'{key.rsplit("/", 1)[-1]}: back {"in " + was["where"] if was["where"] != "none" else "out"}')
    save()
    os.remove(STATE)
    for dname in (os.path.dirname(STATE), os.path.dirname(os.path.dirname(STATE))):
        try:
            os.rmdir(dname)
        except OSError:
            pass
    sys.exit(0)

osj = plistlib.load(open(args[1], 'rb'))['LaunchDaemons'] if len(args) > 1 else {}
if not os.path.exists(STATE):
    state = {}
    for key in want:
        if key in jobs:
            state[key] = {'where': 'launchd', 'job': jobs[key]}
        elif kept is not None and key in kept:
            state[key] = {'where': 'disabled', 'job': kept[key]}
        else:
            state[key] = {'where': 'none'}
    os.makedirs(os.path.dirname(STATE), exist_ok=True)
    with open(STATE, 'wb') as f:
        plistlib.dump(state, f, fmt=plistlib.FMT_BINARY)
for key, lib in want.items():
    src = 'launchd.plist'
    if key not in jobs:
        if kept is not None and key in kept:
            jobs[key] = kept.pop(key); src = 'launchd-disabled.plist'
        elif key in osj:
            jobs[key] = osj[key]; src = 'the IPSW'
        else:
            sys.exit(f'{key}: in neither launchd-disabled.plist nor the IPSW launchd.plist given')
    job = jobs[key]
    if lib is not None:
        args_ = job['ProgramArguments']
        if args_[0] != INJ:
            job['ProgramArguments'] = [INJ, lib] + args_
    if key.endswith('bluetoothd.plist'):
        job.setdefault('EnvironmentVariables', {})['BT_HCI_TRANSPORT'] = 'H4:POSIX:com.apple.uart.bluetooth,4000000'
    job.pop('Disabled', None)
    if LOG:
        name = job['Label'].split('.')[-1]
        job['StandardErrorPath'] = f'/private/var/local/oslog/{name}.err'
        job['StandardOutPath'] = f'/private/var/local/oslog/{name}.out'
        job.setdefault('EnvironmentVariables', {})['OS_ACTIVITY_DT_MODE'] = 'YES'
    print(f'{job["Label"]}: from {src}: {" ".join(job["ProgramArguments"])}')
save()
