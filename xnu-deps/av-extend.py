#!/usr/bin/env python3
"""av-extend.py DSL HEADER: AvailabilityVersions' availability.dsl, with the iOS and macOS versions it lacks added.

Apple's public AvailabilityVersions (157.2, the last release on github.com/apple-oss-distributions) lists iOS up to 17.3
and macOS up to 14.3. An SDK's own usr/include/AvailabilityVersions.h defines __IPHONE_<x>_<y> and __MAC_<x>_<y> for
every release it knows; each one the list lacks is added as a "version" line, after the last of its platform's, in
order. Version numbers are the only thing taken from the header. Written to standard output.
"""
import re
import sys


def key(v):
    return tuple(int(x) for x in v.split("."))


def main():
    dsl, header = sys.argv[1], sys.argv[2]
    lines = open(dsl, encoding="utf-8").read().splitlines()
    have = {"ios": set(), "macos": set()}
    last = {}
    for i, line in enumerate(lines):
        m = re.match(r"version\s+(ios|macos)\s+([0-9.]+)\s*$", line)
        if m:
            have[m.group(1)].add(m.group(2))
            last[m.group(1)] = i
    sdk = {"ios": set(), "macos": set()}
    for line in open(header, encoding="utf-8"):
        m = re.match(r"#define\s+__(IPHONE|MAC)_(\d+)_(\d+)(?:_(\d+))?\s+\d+", line)
        if not m:
            continue
        plat = "ios" if m.group(1) == "IPHONE" else "macos"
        v = "%s.%s" % (m.group(2), m.group(3)) + (".%s" % m.group(4) if m.group(4) else "")
        sdk[plat].add(v)
    adds = {}
    for plat in ("ios", "macos"):
        top = max(key(v) for v in have[plat])
        adds[plat] = sorted((v for v in sdk[plat] - have[plat] if key(v) > top), key=key)
    out = []
    for i, line in enumerate(lines):
        out.append(line)
        for plat in ("ios", "macos"):
            if last.get(plat) == i and adds[plat]:
                out.append("# added by Scion's xnu-deps/av-extend.py from the SDK's AvailabilityVersions.h")
                out += ["version     %-19s %s" % (plat, v) for v in adds[plat]]
    sys.stdout.write("\n".join(out) + "\n")


main()
