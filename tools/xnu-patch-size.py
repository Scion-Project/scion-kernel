#!/usr/bin/env python3
"""How much of Apple's XNU this tree changes: the lines added plus removed by xnu/hakios.patch and xnu/patches/*.patch.

usage: tools/xnu-patch-size.py [--files N] [--root DIR]

Two counts. "Every file" is what docs/modular-kernel.md has counted since 5 November: every added or removed line of the patches,
whichever file it is in. "Apple's files" leaves out the files the tree adds (a hunk of a new file) and the drivers (drivers/map):
what remains is the change to what Apple wrote. A file one of our own (xnu/files/) that a numbered patch changes is counted in the
first and, as it is not in Apple's tree, in the second as well; fold such a patch into the file.
"""
import collections
import glob
import os
import re
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sections(path):
    """(file, is_new, added, removed) for each file a patch changes; a patch may begin each file with "diff --git" or with "--- a/"."""
    out, cur = [], None
    lines = open(path, encoding="utf-8", errors="replace").read().split("\n")
    for i, line in enumerate(lines):
        if line.startswith("diff --git"):
            if cur:
                out.append(tuple(cur))
            cur = [re.match(r"diff --git a/(\S+) b/", line).group(1), False, 0, 0]
        elif line.startswith("--- a/") and i + 1 < len(lines) and lines[i + 1].startswith("+++ b/") and (cur is None or cur[2] + cur[3] > 0):
            if cur:
                out.append(tuple(cur))
            cur = [line[6:].split("\t")[0], False, 0, 0]
        elif cur is None:
            continue
        elif line.startswith("new file mode"):
            cur[1] = True
        elif line.startswith(("+++", "---")) and not line.startswith(("+++ b/dev", "--- a/dev")) and cur[2] + cur[3] == 0:
            continue
        elif line.startswith("+"):
            cur[2] += 1
        elif line.startswith("-"):
            cur[3] += 1
    if cur:
        out.append(tuple(cur))
    return out


def main():
    root = ROOT
    if "--root" in sys.argv:
        root = os.path.abspath(sys.argv[sys.argv.index("--root") + 1])
    top = 0
    if "--files" in sys.argv:
        top = int(sys.argv[sys.argv.index("--files") + 1])
    drivers = set()
    for line in open(os.path.join(root, "drivers", "map"), encoding="utf-8"):
        words = line.split("#", 1)[0].split()
        if len(words) == 2:
            drivers.add(words[1])
    own = {os.path.relpath(os.path.join(d, f), os.path.join(root, "xnu", "files")) for d, _, fs in os.walk(os.path.join(root, "xnu", "files")) for f in fs}
    patches = [os.path.join(root, "xnu", "hakios.patch")] + sorted(glob.glob(os.path.join(root, "xnu", "patches", "*.patch")))
    every, apple = collections.Counter(), collections.Counter()
    numbered = 0
    for p in patches:
        name = os.path.basename(p)
        numbered += name != "hakios.patch"
        for f, new, added, removed in sections(p):
            every[(name, f)] += added + removed
            if not new and f not in drivers and f not in own:
                apple[(name, f)] += added + removed
    print("xnu/hakios.patch and %d numbered patches" % numbered)
    print("  every file:     %5d lines" % sum(every.values()))
    print("  Apple's files:  %5d lines in %d files" % (sum(apple.values()), len({f for _, f in apple})))
    for (name, f), n in apple.most_common(top):
        print("    %5d  %-40s %s" % (n, name, f))


if __name__ == "__main__":
    main()
