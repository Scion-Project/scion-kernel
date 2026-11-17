#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""bt-prefs.py ROOT [--undo] : bluetoothd's overrides for the Galaxy S9's BCM4361 on an iOS 27 root (60-audio-bt.sh,
BT_NO_SCAN_CORE=1). bluetoothd reads its defaults overrides from com.apple.MobileBluetooth.debug; two of them say
whether to use Apple's scan core and LP core ("EnableScanCore", "DisableLowPowerCoreScan"). This chip has neither, so
EnableScanCore is NO and DisableLowPowerCoreScan YES, in /Library/Preferences (any user) and in mobile's preferences.
Only these two keys are added to what a file already has; --undo takes them out again (and a file left empty). Which
user bluetoothd reads them as was not checked, hence both places. NOT yet run on a phone (17 November 2026)."""
import os, plistlib, sys

KEYS = {'EnableScanCore': False, 'DisableLowPowerCoreScan': True}
NAME = 'com.apple.MobileBluetooth.debug.plist'


def places(root):
    return [(os.path.join(root, 'Library/Preferences'), 0, 0, 0o644),
            (os.path.join(root, 'private/var/mobile/Library/Preferences'), 501, 501, 0o600)]


def main():
    root, undo = sys.argv[1], '--undo' in sys.argv[2:]
    for d, uid, gid, mode in places(root):
        p = os.path.join(d, NAME)
        try:
            with open(p, 'rb') as f:
                data = plistlib.load(f)
        except (FileNotFoundError, plistlib.InvalidFileException):
            data = None
        if undo:
            if data is None:
                continue
            for k in KEYS:
                data.pop(k, None)
            if data:
                with open(p, 'wb') as f:
                    plistlib.dump(data, f, fmt=plistlib.FMT_BINARY)
            else:
                os.remove(p)
            print(f'bt-prefs: {p}: the overrides taken out')
            continue
        os.makedirs(d, exist_ok=True)
        data = data or {}
        data.update(KEYS)
        with open(p, 'wb') as f:
            plistlib.dump(data, f, fmt=plistlib.FMT_BINARY)
        os.chmod(p, mode)
        if os.geteuid() == 0:
            os.chown(p, uid, gid)
        print(f'bt-prefs: {p}: {sorted(KEYS)}')


if __name__ == '__main__':
    main()
