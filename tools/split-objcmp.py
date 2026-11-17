#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""Two builds of the kernel compared, before and after a split (docs/architecture.md, "How a split is proven").

usage: tools/split-objcmp.py kernel BEFORE AFTER
       tools/split-objcmp.py objects BEFORE.o[,BEFORE2.o...] AFTER.o[,AFTER2.o...] [--objdump PATH]

kernel   two linked kernels (Mach-O): their external symbols, by name, must be the same set; every symbol (local ones
         too) is listed where the two differ; the C strings (every __cstring section) must be the same multiset.
objects  the objects the split's files compile to, before (one) and after (one or more): the functions defined, by
         name, and the machine code of each one, disassembled (llvm-objdump) with addresses taken out -- branches keep
         the symbol they go to and the offset inside it -- and string labels (l_.str.N, whose numbers follow the order
         strings appear in) taken out. A function whose code is the same is "same"; one that differs is listed with
         its first differing instructions. The C strings of the objects, as a multiset, as for the kernels.

A split that only moved code leaves the kernels' external symbols and strings the same; the code of each function
the same where the compiler had the same translation unit (a codec header included where the code was); and, where
code moved into a translation unit of its own, the same unless the compiler inlined differently across the new
boundary, which this lists function by function for a reviewer. Exit 1 on any difference. Only 64-bit Mach-O (the
kernels of this project) is read.
"""
import collections
import re
import shutil
import struct
import subprocess
import sys

LC_SEGMENT_64, LC_SYMTAB = 0x19, 0x2
N_STAB, N_TYPE, N_EXT, N_SECT = 0xE0, 0x0E, 0x01, 0x0E


def macho(path):
    """(sections {(segname, sectname): bytes}, symbols [(name, external, defined)])"""
    data = open(path, "rb").read()
    magic, _, _, _, ncmds, _, _, _ = struct.unpack_from("<IiiIIIII", data, 0)
    if magic != 0xFEEDFACF:
        raise SystemExit(f"{path}: not a 64-bit Mach-O (magic {magic:#x})")
    off = 32
    sections, symbols = {}, []
    for _ in range(ncmds):
        cmd, size = struct.unpack_from("<II", data, off)
        if cmd == LC_SEGMENT_64:
            nsects = struct.unpack_from("<I", data, off + 64)[0]
            s = off + 72
            for _ in range(nsects):
                sect, seg = data[s:s + 16].rstrip(b"\0").decode(), data[s + 16:s + 32].rstrip(b"\0").decode()
                _, sz, foff = struct.unpack_from("<QQI", data, s + 32)
                flags = struct.unpack_from("<I", data, s + 64)[0]
                if flags & 0xFF not in (0x1, 0xC, 0x12):  # not zerofill
                    sections[(seg, sect)] = data[foff:foff + sz]
                s += 80
        elif cmd == LC_SYMTAB:
            symoff, nsyms, stroff, _ = struct.unpack_from("<IIII", data, off + 8)
            for i in range(nsyms):
                strx, typ, _, _, _ = struct.unpack_from("<IBBhQ", data, symoff + 16 * i)
                if typ & N_STAB:
                    continue
                end = data.index(b"\0", stroff + strx)
                name = data[stroff + strx:end].decode(errors="replace")
                symbols.append((name, bool(typ & N_EXT), (typ & N_TYPE) == N_SECT))
        off += size
    return sections, symbols


def cstrings(sections):
    c = collections.Counter()
    for (seg, sect), b in sections.items():
        if sect == "__cstring":
            for s in b.split(b"\0"):
                if s:
                    c[s] += 1
    return c


def compare_strings(before, after, label):
    a, b = cstrings(before), cstrings(after)
    gone, new = a - b, b - a
    if not gone and not new:
        print(f"{label}: C strings: the same ({sum(a.values())})")
        return 0
    print(f"{label}: C strings DIFFER: {sum(gone.values())} gone, {sum(new.values())} new")
    for s, k in list(gone.items())[:40]:
        print(f"  - {k} x {s[:150]!r}")
    for s, k in list(new.items())[:40]:
        print(f"  + {k} x {s[:150]!r}")
    return 1


def cmd_kernel(before, after):
    sb, yb = macho(before)
    sa, ya = macho(after)
    bad = 0
    eb = {n for n, ext, d in yb if ext and d}
    ea = {n for n, ext, d in ya if ext and d}
    if eb == ea:
        print(f"kernel: external symbols defined: the same ({len(eb)})")
    else:
        bad = 1
        print(f"kernel: external symbols DIFFER: {len(eb - ea)} gone, {len(ea - eb)} new")
        for n in sorted(eb - ea):
            print(f"  - {n}")
        for n in sorted(ea - eb):
            print(f"  + {n}")
    lb = collections.Counter(n for n, ext, d in yb if d)
    la = collections.Counter(n for n, ext, d in ya if d)
    if lb != la:
        print(f"kernel: all symbols (local ones too): {sum((lb - la).values())} gone, {sum((la - lb).values())} new "
              f"(a local symbol that became external, or the reverse, shows only above)")
        for n in sorted((lb - la))[:60]:
            print(f"  - {n}")
        for n in sorted((la - lb))[:60]:
            print(f"  + {n}")
    else:
        print(f"kernel: all symbols defined, by name: the same ({sum(lb.values())})")
    bad |= compare_strings(sb, sa, "kernel")
    tb = sum(len(v) for (seg, s), v in sb.items() if s == "__text")
    ta = sum(len(v) for (seg, s), v in sa.items() if s == "__text")
    print(f"kernel: __text {tb} bytes before, {ta} after ({ta - tb:+d})")
    return bad


ADDR = re.compile(r"\b(0x)?[0-9a-f]+ (<[^>]+>)")
LSTR = re.compile(r"\b(l_\.str|L_\.str|l___func__|L___func__|lJTI\d+_|ltmp)[\w.]*")
FUNC = re.compile(r"^[0-9a-f]+ <(.+)>:$")


def disassemble(objdump, path):
    """{function: [normalised instructions]}"""
    out = subprocess.run([objdump, "-d", "--no-show-raw-insn", path], capture_output=True, text=True, check=True).stdout
    funcs, cur = {}, None
    for line in out.splitlines():
        m = FUNC.match(line.strip())
        if m:
            cur = m.group(1)
            if cur.startswith(("ltmp", "l_", "L")):
                cur = None
                continue
            funcs[cur] = []
            continue
        if cur is None or ":" not in line:
            continue
        ins = line.split(":", 1)[1].strip()
        ins = ADDR.sub(r"\2", ins)
        ins = LSTR.sub("L", ins)
        ins = re.sub(r";.*$", "", ins).strip()
        if ins:
            funcs[cur].append(ins)
    return funcs


def cmd_objects(before, after, objdump):
    objdump = objdump or shutil.which("llvm-objdump") or shutil.which("objdump")
    if objdump is None:
        try:
            objdump = subprocess.run(["xcrun", "--find", "llvm-objdump"], capture_output=True, text=True,
                                     check=True).stdout.strip()
        except (OSError, subprocess.CalledProcessError):
            raise SystemExit("no llvm-objdump: give --objdump")
    fb, fa = {}, {}
    sb, sa = {}, {}
    for p in before:
        fb.update(disassemble(objdump, p))
        for k, v in macho(p)[0].items():
            sb[(p,) + k] = v
    for p in after:
        fa.update(disassemble(objdump, p))
        for k, v in macho(p)[0].items():
            sa[(p,) + k] = v
    bad = 0
    same = [f for f in fb if f in fa and fb[f] == fa[f]]
    differ = [f for f in fb if f in fa and fb[f] != fa[f]]
    print(f"objects: {len(fb)} functions before, {len(fa)} after; {len(same)} the same, {len(differ)} differ")
    for f in sorted(set(fb) - set(fa)):
        bad = 1
        print(f"  - {f} (gone: inlined everywhere now, or removed)")
    for f in sorted(set(fa) - set(fb)):
        bad = 1
        print(f"  + {f} (new: no longer inlined, or added)")
    for f in sorted(differ):
        bad = 1
        a, b = fb[f], fa[f]
        print(f"  ~ {f}: {len(a)} instructions before, {len(b)} after")
        for d in list(difflib_unified(a, b))[:12]:
            print("      " + d)
    # strings: keyed by section name only, the objects pooled
    bad |= compare_strings({("", "__cstring"): b"\0".join(v for k, v in sb.items() if k[2] == "__cstring")},
                           {("", "__cstring"): b"\0".join(v for k, v in sa.items() if k[2] == "__cstring")}, "objects")
    return bad


def difflib_unified(a, b):
    import difflib
    return (l for l in difflib.unified_diff(a, b, lineterm="", n=1) if not l.startswith(("---", "+++")))


def main():
    args = sys.argv[1:]
    objdump = None
    if "--objdump" in args:
        i = args.index("--objdump")
        objdump = args[i + 1]
        del args[i:i + 2]
    if len(args) == 3 and args[0] == "kernel":
        bad = cmd_kernel(args[1], args[2])
    elif len(args) == 3 and args[0] == "objects":
        bad = cmd_objects(args[1].split(","), args[2].split(","), objdump)
    else:
        print(__doc__)
        return 2
    print("split-objcmp: " + ("the same" if not bad else "DIFFERENT: see above"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
