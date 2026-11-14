#!/usr/bin/env python3
"""Give an ADT's root node a platform-name (default t8030, the SoC of the iPhone the S9 presents as, N104AP).

MobileGestalt answers HardwarePlatform from the device tree's root platform-name; the S9's ADT had none, and PosterBoard's
poster snapshot path aborted in CoreVideo ("Failed to read HardwarePlatform, MG query returned NULL").
  tools/ios27/ui/adt-platform-name.py IN.adt OUT.adt [NAME]"""
import os, sys
sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'herolte'))
import adt  # noqa: E402

src, dst = sys.argv[1], sys.argv[2]
name = (sys.argv[3] if len(sys.argv) > 3 else 't8030').encode() + b'\0'
root, end = adt.parse(open(src, 'rb').read())
for i, (n, flags, val) in enumerate(root.props):
    if n == 'platform-name':
        root.props[i] = (n, flags, name)
        print(f'{dst}: platform-name {val!r} -> {name!r}')
        break
else:
    root.props.append(('platform-name', 0, name))
    print(f'{dst}: platform-name added: {name!r}')
open(dst, 'wb').write(adt.serialize(root))
