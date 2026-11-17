#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""Proof that a split of files moved lines and changed none (docs/architecture.md, "How a split is proven").

usage: tools/split-check.py [--map] [--base REV] FILE...

FILE... are every file the split touches: the old file and the new ones, as paths of this repository. Their lines at
REV (default origin/main; a file that did not exist there counts as empty) are compared, as one multiset, with their
lines in the working tree. A pure move leaves the two multisets equal. What a split may add besides is put in classes
and listed for a reviewer: blank and comment lines (the head of each new file says what it holds), #include lines and
include guards, prototypes of functions that already exist, and `static` dropped from a definition another file of the
split now calls (paired with the line it came from). Anything else -- a line removed, a line of code added or changed --
is NOT a move: it is printed with its file and line number and the exit status is 1.

--map prints, for each new file, which ranges of the old file's lines it holds (old lines A-B -> new file lines C-D):
the mapping someone carrying patches against the old file needs to move them.

Trailing white space is ignored; indentation is not. Nothing here compiles anything: the objects are compared by
tools/split-objcmp.py.
"""
import argparse
import collections
import difflib
import re
import subprocess
import sys

COMMENT = re.compile(r"^\s*(/\*.*|\*.*|\*/|//.*)$")
INCLUDE = re.compile(r"^\s*#\s*include\s*[<\"]")
GUARD = re.compile(r"^#\s*(ifndef|define|endif)\b\s*(/\*\s*)?_?[A-Z0-9_]+_H_?\b.*$")
PROTO = re.compile(r"^(?:static\s+|extern\s+|__private_extern__\s+)?[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\([^;{]*\)\s*(?:__\w+(?:\([^)]*\))?\s*)*;\s*$")
FUNC_HEAD = re.compile(r"^([A-Za-z_]\w*)\s*\(")


def at_rev(rev, path):
    r = subprocess.run(["git", "show", f"{rev}:{path}"], capture_output=True, text=True)
    return r.stdout.split("\n") if r.returncode == 0 else []


def now(path):
    try:
        with open(path) as f:
            return f.read().split("\n")
    except FileNotFoundError:
        return []


def norm(lines):
    return [l.rstrip() for l in lines]


def functions(lines):
    """Names of the functions defined in lines (BSD style: the name at the start of a line, or after the type)."""
    names = set()
    for l in lines:
        m = FUNC_HEAD.match(l)
        if m:
            names.add(m.group(1))
        m = re.match(r"^(?:static\s+)?[A-Za-z_][\w\s\*]*?\b([A-Za-z_]\w*)\s*\([^;]*$", l)
        if m and not l.startswith((" ", "\t", "#")):
            names.add(m.group(1))
    return names


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--base", default="origin/main")
    ap.add_argument("--map", action="store_true")
    ap.add_argument("files", nargs="+")
    a = ap.parse_args()

    old = {p: norm(at_rev(a.base, p)) for p in a.files}
    new = {p: norm(now(p)) for p in a.files}
    old_all = collections.Counter(l for p in a.files for l in old[p])
    new_all = collections.Counter(l for p in a.files for l in new[p])
    removed = old_all - new_all
    added = new_all - old_all
    known = functions(l for p in a.files for l in old[p])

    # `static` dropped: a removed "static T name(...)" or "static T" whose rest is an added line, or the BSD form where
    # the line "static int" became "int" (counted by pairs)
    static_pairs = []
    for line in list(removed):
        if line.startswith("static ") and added.get(line[len("static "):], 0) > 0:
            k = min(removed[line], added[line[len("static "):]])
            static_pairs.append((line, line[len("static "):], k))
            removed[line] -= k
            added[line[len("static "):]] -= k
    removed = +removed
    added = +added

    classes = collections.OrderedDict((c, []) for c in ("blank", "comment", "include", "guard", "prototype", "CODE"))
    # an added line is put down to a new file before an old one: a line both have is the new file's (its head, its
    # includes), not the old file's, which kept its own
    for p in sorted(a.files, key=lambda p: bool(old[p])):
        left = collections.Counter(added)
        for i, l in enumerate(new[p], 1):
            if left.get(l, 0) <= 0:
                continue
            left[l] -= 1
            added[l] -= 1
            if not l.strip():
                c = "blank"
            elif COMMENT.match(l):
                c = "comment"
            elif INCLUDE.match(l):
                c = "include"
            elif GUARD.match(l):
                c = "guard"
            elif PROTO.match(l) and PROTO.match(l).group(1) in known:
                c = "prototype"
            else:
                c = "CODE"
            classes[c].append((p, i, l))
    gone = []
    for p in a.files:
        left = collections.Counter(removed)
        for i, l in enumerate(old[p], 1):
            if left.get(l, 0) > 0:
                left[l] -= 1
                removed[l] -= 1
                gone.append((p, i, l))

    moved = sum((old_all & new_all).values())
    print(f"split-check: {sum(len(v) for v in old.values())} lines at {a.base}, {sum(len(v) for v in new.values())} now, "
          f"{moved} moved unchanged")
    for line, rest, k in static_pairs:
        print(f"  static dropped ({k}): {line!r} -> {rest!r}")
    for c, rows in classes.items():
        if not rows:
            continue
        print(f"  added, {c}: {len(rows)}")
        if c in ("include", "guard", "prototype", "CODE") or (c == "comment" and len(rows) <= 400):
            for p, i, l in rows:
                print(f"    {p}:{i}: {l}")
    if gone:
        print(f"  REMOVED or CHANGED (not a move): {len(gone)}")
        for p, i, l in gone:
            print(f"    {p}:{i} (at {a.base}): {l}")

    if a.map:
        print("map: old lines -> new lines (blocks of 3 lines or more)")
        for p_old in a.files:
            if not old[p_old]:
                continue
            for p in a.files:
                if not new[p]:
                    continue
                sm = difflib.SequenceMatcher(None, old[p_old], new[p], autojunk=False)
                for blk in sm.get_matching_blocks():
                    if blk.size >= 3:
                        print(f"  {p_old}:{blk.a + 1}-{blk.a + blk.size} -> {p}:{blk.b + 1}-{blk.b + blk.size}")

    bad = len(gone) + len(classes["CODE"])
    print("split-check: " + ("a pure move" if not bad else f"{bad} line(s) are not a move: review them, or undo them"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
