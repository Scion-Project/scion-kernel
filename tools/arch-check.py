#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""The architecture's rules that a machine can check (docs/architecture.md, "What CI enforces").

usage: tools/arch-check.py [--report] [--root DIR]

  (none)    exit 1 when a rule is broken; say which file and why
  --report  also list every file over the ceiling

The rules:
  size      a source file of drivers/, socs/, userland/src/ or tools/ over CEILING lines fails, unless
            tools/arch-size.allow lists it with an
            allowance it is within. The list only shrinks: an entry for a file that is gone, or that is back under the
            ceiling, fails too (delete the entry), unless the entry names an open pull request that will take the
            file over the ceiling ("#329"); a file that has shrunk well under its allowance is asked to lower it.
  codec     a file tools/arch-layers.conf names a codec (plain C over bytes, no kernel) includes only the C library's
            freestanding headers and other codecs, and is exercised by a host test that CI runs: its name appears in a
            tools/*.sh script that .github/workflows/check.yml runs, or in a C file that such a script names, or a
            codec so exercised includes it.
  soc       a SoC header (socs/*/include/) includes nothing but other SoC headers and <stdint.h>.
  no .c     nothing in drivers/ includes a .c file (one translation unit is one file; parts of a driver are files of
            their own, docs/architecture.md, "A driver of several files").
"""
import argparse
import fnmatch
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
CEILING = 800
SOURCE = (".c", ".h", ".cpp", ".m", ".mm", ".S", ".s")
SIZED = SOURCE + (".py", ".sh", ".rs")
SIZED_TREES = ("drivers", "socs", "userland/src", "tools")
CODEC_INCLUDES = {"stdbool.h", "stddef.h", "stdint.h", "string.h", "limits.h"}
SOC_INCLUDES = {"stdint.h"}
INCLUDE = re.compile(r'^\s*#\s*include\s*([<"])([^>"]+)[>"]')


def git_files(root, *paths):
    out = subprocess.run(["git", "-C", root, "ls-files", "--", *paths], capture_output=True, text=True, check=True).stdout
    return [p for p in out.splitlines() if p]


def count_lines(path):
    with open(path, "rb") as f:
        return f.read().count(b"\n")


def read_allow(path):
    """{path: (allowance, note, line number)} from lines "path allowance [# note]"."""
    allow = {}
    with open(path) as f:
        for n, line in enumerate(f, 1):
            body, _, note = line.partition("#")
            words = body.split()
            if not words:
                continue
            if len(words) != 2 or not words[1].isdigit():
                raise SystemExit(f"{path}:{n}: expected 'path allowance [# note]'")
            allow[words[0]] = (int(words[1]), note.strip(), n)
    return allow


def read_layers(path):
    """[(layer, glob)] from lines "layer glob"."""
    layers = []
    with open(path) as f:
        for n, line in enumerate(f, 1):
            words = line.split("#", 1)[0].split()
            if not words:
                continue
            if len(words) != 2:
                raise SystemExit(f"{path}:{n}: expected 'layer path-or-glob'")
            layers.append((words[0], words[1]))
    return layers


def includes(path):
    with open(path, errors="replace") as f:
        for n, line in enumerate(f, 1):
            m = INCLUDE.match(line)
            if m:
                yield n, m.group(1), m.group(2)


def ci_test_text(root):
    """The text of every tools/ script check.yml runs, and of the C files those scripts name."""
    wf = os.path.join(root, ".github/workflows/check.yml")
    text = []
    if not os.path.exists(wf):
        return ""
    scripts = set(re.findall(r"tools/[\w./-]+\.sh", open(wf).read()))
    for s in sorted(scripts):
        p = os.path.join(root, s)
        if not os.path.exists(p):
            continue
        body = open(p, errors="replace").read()
        text.append(body)
        for c in set(re.findall(r"tools/[\w./-]+\.c\b", body)):
            q = os.path.join(root, c)
            if os.path.exists(q):
                text.append(open(q, errors="replace").read())
    return "\n".join(text)


def check(root, report):
    errors, notes = [], []
    allow_path = os.path.join(root, "tools/arch-size.allow")
    allow = read_allow(allow_path)
    layers = read_layers(os.path.join(root, "tools/arch-layers.conf"))

    # size
    seen, over = set(), []
    for p in git_files(root, *SIZED_TREES):
        if not p.endswith(SIZED):
            continue
        n = count_lines(os.path.join(root, p))
        if n > CEILING:
            over.append((n, p))
        if p in allow:
            seen.add(p)
            cap, note, ln = allow[p]
            if n > cap:
                errors.append(f"{p}: {n} lines, over its allowance of {cap} (tools/arch-size.allow:{ln}). Split it "
                              f"(docs/architecture.md, \"When a file passes the ceiling\"), or raise the allowance in the "
                              f"same pull request and say why in its description")
            elif n <= CEILING and not re.search(r"#\d+", note):
                errors.append(f"{p}: {n} lines, back under the ceiling of {CEILING}: delete its entry "
                              f"(tools/arch-size.allow:{ln})")
            elif cap - n >= 100 and not re.search(r"#\d+", note):
                notes.append(f"{p}: {n} lines, {cap - n} under its allowance: lower it to {n} "
                             f"(tools/arch-size.allow:{ln})")
        elif n > CEILING:
            errors.append(f"{p}: {n} lines, over the ceiling of {CEILING}. Make it two "
                          f"or more files of one responsibility each (docs/architecture.md, \"Conventions\")")
    for p, (cap, note, ln) in allow.items():
        if p not in seen and not re.search(r"#\d+", note):
            errors.append(f"{p}: listed in tools/arch-size.allow:{ln} but not a file that is checked: delete the entry")

    # codec and soc layers
    files = git_files(root, "drivers", "socs")
    codecs = set()
    for layer, glob in layers:
        hits = [p for p in files if fnmatch.fnmatch(p, glob)]
        if not hits:
            errors.append(f"tools/arch-layers.conf: '{layer} {glob}' names no file")
        if layer == "codec":
            codecs.update(hits)
        elif layer != "soc":
            errors.append(f"tools/arch-layers.conf: unknown layer '{layer}' (codec or soc)")
    codec_names = {os.path.basename(p) for p in codecs}
    tests = ci_test_text(root)
    # a codec is tested when a CI test names it, or when a tested codec includes it
    tested = {p for p in codecs if os.path.basename(p) in tests}
    grew = True
    while grew:
        grew = False
        for p in list(tested):
            for _, kind, inc in includes(os.path.join(root, p)):
                for q in codecs:
                    if kind == '"' and os.path.basename(q) == os.path.basename(inc) and q not in tested:
                        tested.add(q)
                        grew = True
    for p in sorted(codecs):
        for n, kind, inc in includes(os.path.join(root, p)):
            if kind == "<" and inc in CODEC_INCLUDES:
                continue
            if kind == '"' and os.path.basename(inc) in codec_names:
                continue
            errors.append(f"{p}:{n}: a codec includes only {', '.join(sorted(CODEC_INCLUDES))} and other codecs, "
                          f"not {kind}{inc}")
        if p not in tested:
            errors.append(f"{p}: a codec with no host test that CI runs (name it in a tools/*-test.sh that "
                          f".github/workflows/check.yml runs, or in a C file that script builds)")
    for p in files:
        if fnmatch.fnmatch(p, "socs/*/include/*.h"):
            for n, kind, inc in includes(os.path.join(root, p)):
                soc_header = any(fnmatch.fnmatch(q, "socs/*/include/" + os.path.basename(inc)) for q in files)
                if (kind == "<" and inc in SOC_INCLUDES) or soc_header:
                    continue
                errors.append(f"{p}:{n}: a SoC header includes only <stdint.h> and other SoC headers, not {inc}")

    # no .c included
    for p in files:
        if p.endswith(SOURCE):
            for n, kind, inc in includes(os.path.join(root, p)):
                if inc.endswith(".c"):
                    errors.append(f"{p}:{n}: includes {inc}: a .c file is a translation unit of its own")

    if report:
        print(f"{len(over)} files over {CEILING} lines, each within its allowance or listed above:")
        for n, p in sorted(over, reverse=True):
            print(f"  {n:6d} {p}")
    return errors, notes


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--report", action="store_true")
    ap.add_argument("--root", default=os.path.dirname(HERE))
    a = ap.parse_args()
    errors, notes = check(a.root, a.report)
    for e in notes:
        print("note: " + e)
    for e in errors:
        print("arch-check: " + e)
    if errors:
        print(f"arch-check: {len(errors)} problem(s); docs/architecture.md says what each rule is for")
        return 1
    print("arch-check: the size allowances, the codec and SoC layers and the include rules hold")
    return 0


if __name__ == "__main__":
    sys.exit(main())
