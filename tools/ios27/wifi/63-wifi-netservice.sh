#!/bin/bash
# 63-wifi-netservice.sh ROOT [undo] : the Wi-Fi network service in configd's preferences before the first boot. SPDX-License-Identifier: MIT-0
#
# Why: a root that has never had Wi-Fi has a preferences.plist with no network service at all. On its first Wi-Fi boot the
# network was joined at once but DHCP (configd's IPConfiguration) did not send a frame for about 4 minutes, until iOS had
# created the Wi-Fi service itself (seen on phone 1, 13 Nov 2026: join 18:02:48, DHCP 18:07:00; later boots: DHCP 2 s after
# the join). This step writes the same service iOS writes (en0, "Wi-Fi", AirPort, IPv4 DHCP, IPv6 automatic) and a set
# ordering it, only when preferences.plist has no service for en0. Nothing else is changed; no network name or key.
set -euo pipefail
R=$1
P=$R/private/var/preferences/SystemConfiguration/preferences.plist
[[ -d $(dirname "$P") ]] || { echo "63-wifi-netservice: $R has no SystemConfiguration preferences directory" >&2; exit 1; }
/usr/bin/python3 - "$P" "${2:-add}" <<'PY'
import os, plistlib, sys
path, how = sys.argv[1:]
SVC = '5C10D000-0E00-4A7A-9000-5C1000000E00'
SET = '5C10D000-5E70-4A7A-9000-5C1000005E70'
d = plistlib.load(open(path, 'rb')) if os.path.exists(path) else {'__VERSION__': 20191120}
svcs = d.setdefault('NetworkServices', {})
if how == 'undo':
    svcs.pop(SVC, None)
    if d.get('Sets', {}).pop(SET, None) is not None and d.get('CurrentSet') == '/Sets/' + SET:
        d.pop('CurrentSet')
    if not svcs:
        d.pop('NetworkServices')
    if not d.get('Sets', 1):
        d.pop('Sets')
    print('63-wifi-netservice: ours removed (if it was there)')
else:
    have = [k for k, v in svcs.items() if v.get('Interface', {}).get('DeviceName') == 'en0']
    if have:
        print('63-wifi-netservice: preferences.plist already has a service for en0 (%s): unchanged' % have[0])
        sys.exit(0)
    svcs[SVC] = {'DNS': {}, 'IPv4': {'ConfigMethod': 'DHCP'}, 'IPv6': {'ConfigMethod': 'Automatic'},
                 'Interface': {'DeviceName': 'en0', 'Hardware': 'AirPort', 'Type': 'Ethernet', 'UserDefinedName': 'Wi-Fi'},
                 'Proxies': {'ExceptionsList': ['*.local', '169.254/16'], 'FTPPassive': 1}, 'UserDefinedName': 'Wi-Fi'}
    sets = d.setdefault('Sets', {})
    cur = d.get('CurrentSet', '').replace('/Sets/', '')
    if cur in sets:
        net = sets[cur].setdefault('Network', {})
        net.setdefault('Service', {})[SVC] = {'__LINK__': '/NetworkServices/' + SVC}
        order = net.setdefault('Global', {}).setdefault('IPv4', {}).setdefault('ServiceOrder', [])
        order.insert(0, SVC)
        net.setdefault('Interface', {}).setdefault('en0', {'AirPort': {'JoinMode': 'Automatic'}})
    else:
        sets[SET] = {'Network': {'Global': {'IPv4': {'ServiceOrder': [SVC]}},
                                 'Interface': {'en0': {'AirPort': {'JoinMode': 'Automatic'}}},
                                 'Service': {SVC: {'__LINK__': '/NetworkServices/' + SVC}}},
                     'UserDefinedName': 'Automatic'}
        d['CurrentSet'] = '/Sets/' + SET
    d.setdefault('Model', 'N104AP')
    print('63-wifi-netservice: the Wi-Fi service for en0 added')
data = plistlib.dumps(d, fmt=plistlib.FMT_XML); plistlib.loads(data)
open(path, 'wb').write(data)
PY
