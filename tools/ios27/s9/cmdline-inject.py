#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""cmdline-inject.py: the boot image's sweetinject= from the root build's record (make-boot-image.sh --inject-record).

    cmdline-inject.py compose --line LINE --record RECORD [--kernel KERNEL]   -> the line to pack, on stdout

The kernel inserts libraries into programs from sweetinject= (the fork's bsd/kern/kern_exec.c, "sweetinject": groups
LIBRARY,PROGRAM,PROGRAM... separated by ";"). Its built-in command line (osfmk/arm/arm_init.c, starlte_default_bootargs)
names three libraries; an argument the loader's line names replaces the built-in one of the same name WHOLE
(starlte_bootargs_defaults), so a line that adds one library must carry the built-in three as well. Every library named
must be on the root: dyld refuses to start a program whose inserted library is missing, and with backboardd or SpringBoard
the phone has no interface at all. Hence:
  - the record (tools/ios27/fullroot/inject-record.sh: "LIBRARY PROGRAM..." lines, "#" comments) lists what the root has;
  - the line gets sweetinject=<the built-in three>;<exactly the recorded ones>, and nothing at all when the record lists
    none (the image is then what it was without a record);
  - refused, with the reason: a line that names sweetinject= or sweetdef= itself, a record line that is not well formed,
    a value longer than the kernel keeps (511 bytes), a line longer than the loader's 607 bytes, and, given the kernel,
    a kernel whose built-in sweetinject= is not the one written here, or a line so long that the kernel would have to
    leave some of its own built-in arguments out (they are appended only while the 1024-byte line has room).
"""
import re
import sys

BUILTIN = ('/usr/lib/s.dylib,backboardd,Preferences;/usr/lib/m.dylib,PosterBoard,SpringBoard;'
           '/usr/lib/b.dylib,backboardd')
LOADER_MAX = 607          # the loader's line: 608 bytes with its NUL (make-boot-image.sh)
KERNEL_LINE = 1024        # BOOT_LINE_LENGTH of the arm64 kernel (pexpert/pexpert/arm64/boot.h)
VALUE_MAX = 511           # sweet_exec_inject[512] in kern_exec.c
LIB_RE = re.compile(r'/[A-Za-z0-9._/+-]+')
PROG_RE = re.compile(r'[A-Za-z0-9._+-]+')


class Refused(Exception):
    pass


def names(line):
    """the argument names of a command line, as the kernel's starlte_bootarg_named sees them"""
    out = []
    for tok in line.replace('\t', ' ').split(' '):
        if tok:
            out.append(tok.split('=', 1)[0])
    return out


def parse_record(text):
    """[(library, [program, ...]), ...] in the record's order; the same library twice is one entry"""
    entries = {}
    for n, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith('#'):
            continue
        parts = line.split()
        lib, progs = parts[0], parts[1:]
        if not LIB_RE.fullmatch(lib):
            raise Refused(f'record line {n}: "{lib}" is not a library path the command line can carry')
        if not progs:
            raise Refused(f'record line {n}: {lib} names no program')
        for p in progs:
            if not PROG_RE.fullmatch(p):
                raise Refused(f'record line {n}: "{p}" is not a program name the command line can carry')
        have = entries.setdefault(lib, [])
        have.extend(p for p in progs if p not in have)
    return list(entries.items())


def builtin_groups():
    return [(g.split(',')[0], g.split(',')[1:]) for g in BUILTIN.split(';')]


def compose_value(entries):
    """the sweetinject= value: the built-in three, then each recorded library with the programs the built-in groups do
    not already give it (a library is never inserted twice into one program)"""
    covered = {}
    for lib, progs in builtin_groups():
        covered.setdefault(lib, set()).update(progs)
    groups = [BUILTIN]
    for lib, progs in entries:
        new = [p for p in progs if p not in covered.get(lib, set())]
        if new:
            groups.append(','.join([lib] + new))
            covered.setdefault(lib, set()).update(new)
    return ';'.join(groups), len(groups) > 1


def kernel_defaults(kernel_bytes):
    """the kernel's built-in S9 line (starlte_default_bootargs), found by its content; None if the kernel has none"""
    for m in re.finditer(rb'sweetinject=/usr/lib/', kernel_bytes):
        start = kernel_bytes.rfind(b'\0', 0, m.start()) + 1
        end = kernel_bytes.find(b'\0', m.start())
        s = kernel_bytes[start:end]
        if b'ufsroot=' in s and b'rd=' in s:
            return s.decode('ascii', 'replace')
    return None


def merged_line(line, defaults):
    """the line as the kernel makes it (starlte_bootargs_defaults): each built-in argument appended unless the line names
    it, while it fits; returns (line, [the built-in arguments left out for want of room])"""
    have = set(names(line))
    if 'sweetdef' in have:
        return line, []
    out, dropped = line, []
    for tok in defaults.replace('\t', ' ').split(' '):
        if not tok:
            continue
        if tok.split('=', 1)[0] in have:
            continue
        if len(out) + 1 + len(tok) >= KERNEL_LINE:
            dropped.append(tok)
            continue
        out = out + ' ' + tok if out else tok
    return out, dropped


def compose(line, record_text, kernel_bytes=None):
    """-> (the line to pack, a sentence saying what was done)"""
    if '\n' in line:
        raise Refused('the command line has more than one line')
    entries = parse_record(record_text)
    value, added = compose_value(entries)
    if not added:
        return line, 'the record lists no library beyond the kernel\'s built-in three: the line is unchanged'
    have = names(line)
    if 'sweetinject' in have:
        raise Refused('the command line names sweetinject= itself; with a record make-boot-image.sh writes it: take it '
                      'off the line, or pass --inject-record none to keep the line as it is')
    if 'sweetdef' in have:
        raise Refused('the command line has sweetdef= (the kernel adds none of its own arguments): write sweetinject= '
                      'into the line yourself and pass --inject-record none')
    if len(value) > VALUE_MAX:
        raise Refused(f'sweetinject= would be {len(value)} bytes; the kernel keeps {VALUE_MAX}')
    out = (line + ' ' if line else '') + 'sweetinject=' + value
    if len(out.encode()) > LOADER_MAX:
        raise Refused(f'with sweetinject= the command line is {len(out.encode())} bytes; the boot image holds '
                      f'{LOADER_MAX}: shorten --cmdline or leave a feature out of the root')
    if kernel_bytes is not None:
        d = kernel_defaults(kernel_bytes)
        if d is None:
            raise Refused('the kernel given has no built-in S9 command line (starlte_default_bootargs): is it the '
                          'S9 kernel? (--inject-record none to pack without the record)')
        kv = [t.split('=', 1)[1] for t in d.split(' ') if t.startswith('sweetinject=')]
        if kv != [BUILTIN]:
            raise Refused('the kernel\'s built-in sweetinject= is not the one tools/ios27/s9/cmdline-inject.py knows '
                          f'({kv[0] if kv else "none"!r}): bring BUILTIN up to date before packing a record')
        _, dropped = merged_line(out, d)
        if dropped:
            raise Refused('the kernel would leave out its own built-in ' + ' '.join(dropped) +
                          f' for want of room in its {KERNEL_LINE}-byte line: shorten --cmdline')
    libs = ', '.join(f'{lib} ({",".join(p)})' for lib, p in entries)
    return out, f'sweetinject= is the kernel\'s built-in three and {libs}, from the record'


def main(argv):
    import argparse
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    sub = ap.add_subparsers(dest='cmd', required=True)
    c = sub.add_parser('compose')
    c.add_argument('--line', required=True)
    c.add_argument('--record', required=True)
    c.add_argument('--kernel')
    a = ap.parse_args(argv)
    try:
        with open(a.record, encoding='utf-8') as f:
            rec = f.read()
        kb = open(a.kernel, 'rb').read() if a.kernel else None
        out, what = compose(a.line, rec, kb)
    except (Refused, OSError, UnicodeDecodeError) as e:
        print(f'cmdline-inject: {e}', file=sys.stderr)
        return 1
    print(what, file=sys.stderr)
    print(out)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
