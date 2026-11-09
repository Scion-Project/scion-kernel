#!/usr/bin/env python3
"""tools/ios27/firehose-ios.py: what it changes, and what it refuses.

Made-up objects and archives in the layout Apple's tools make (a 64-bit arm64 Mach-O with LC_BUILD_VERSION and LC_LINKER_OPTIMIZATION_HINT,
a BSD ar archive with a `__.SYMDEF SORTED` member and long names), nothing of Apple's:

  macOS 15.5 object       -> platform iOS, minos and sdk 18.5; the 12 bytes of those three fields and no others
  the archive             -> same size, same symbol table, same member headers and contents otherwise (offsets still right)
  an iOS object           -> the same bytes again (idempotent); several objects: all of them
  --minos / --sdk         -> each its own
  refused, nothing written: not an archive, a damaged member header, a member that is not Mach-O, 32-bit or big-endian or x86_64
                          objects, an object without LC_BUILD_VERSION (LC_VERSION_MIN_MACOSX), a load command that does not fit,
                          a tvOS object, an archive without an object, a bad version
"""
import os
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "firehose-ios.py")
import importlib.util

spec = importlib.util.spec_from_file_location("firehose_ios", TOOL)
fh = importlib.util.module_from_spec(spec)
spec.loader.exec_module(fh)

failures = []


def check(label, ok, detail=""):
    print("%s %s" % ("ok  " if ok else "FAIL", label))
    if not ok:
        failures.append(label)
        if detail:
            print("     " + str(detail).strip().replace("\n", "\n     ")[:400])


V = fh.parse_version


def macho(platform=1, minos="15.5", sdk="15.5", cputype=0x0100000C, magic=0xFEEDFACF, build=True, lc_extra=b"", tail=b"firehose code\0" * 40):
    """a made-up object: LC_SEGMENT_64 (no sections), LC_BUILD_VERSION, LC_SYMTAB, LC_LINKER_OPTIMIZATION_HINT"""
    seg = struct.pack("<II16sQQQQiiII", 0x19, 72, b"", 0, 0x100, 0x200, len(tail), 7, 7, 0, 0)
    if build:
        bv = struct.pack("<IIIIII", 0x32, 24, platform, V(minos), V(sdk), 0)
    else:
        bv = struct.pack("<IIII", 0x24, 16, V(minos), V(sdk))  # LC_VERSION_MIN_MACOSX
    symtab = struct.pack("<IIIIII", 2, 24, 0x300, 0, 0x300, 0)
    loh = struct.pack("<IIII", 0x2E, 16, 0x300, 0)
    cmds = seg + bv + symtab + loh + lc_extra
    head = struct.pack("<IiiIIIII", magic, cputype, 0, 1, 4 + (1 if lc_extra else 0), len(cmds), 0x2000, 0)
    return head + cmds + b"\0" * (0x200 - 32 - len(cmds)) + tail


def member(name, data, long_name=True):
    """one ar member the way Apple's ar writes it: a long name opens the contents, the size counts it, members start on even offsets"""
    if long_name:
        nm = name.encode() + b"\0" * (-len(name) % 4 + 4)
        body = nm + data
        field = ("#1/%d" % len(nm)).encode()
    else:
        body = data
        field = name.encode()
    out = field.ljust(16) + b"0".ljust(12) + b"0".ljust(6) + b"0".ljust(6) + b"100644".ljust(8) + str(len(body)).encode().ljust(10) + b"`\n"
    return out + body + (b"\n" if len(body) % 2 else b"")


def archive(*objs, symdef=True):
    out = b"!<arch>\n"
    if symdef:
        out += member("__.SYMDEF SORTED", struct.pack("<II", 16, 0) + b"\x01\x02\x03\x04" * 20)
    for name, data in objs:
        out += member(name, data)
    return out


def run(*args, cwd=None):
    return subprocess.run([sys.executable, TOOL, *args], capture_output=True, text=True, cwd=cwd)


def diff_offsets(a, b):
    return [i for i in range(len(a)) if a[i] != b[i]]


# the object's own offsets of the three fields: the LC_BUILD_VERSION follows the 72-byte segment command
BV = 32 + 72

# --- the library function ---
obj = macho()
src = archive(("firehose_buffer.o", obj))
out, done = fh.convert(src, V("18.5"), V("18.5"))
member_start = src.index(obj)
check("one object: the archive keeps its size", len(out) == len(src))
changed = diff_offsets(src, out)
fields = range(member_start + BV + 8, member_start + BV + 20)
check("only the platform, minos and sdk fields changed (the symbol table, the headers, the code are the input's)",
      changed != [] and all(c in fields for c in changed), changed)
platform, minos, sdk = struct.unpack_from("<III", out, member_start + BV + 8)
check("platform iOS, minos 18.5, sdk 18.5", (platform, minos, sdk) == (2, V("18.5"), V("18.5")), (platform, minos, sdk))
check("what was there is reported", done == [("firehose_buffer.o", 1, V("15.5"), V("15.5"))], done)

# --- idempotent, several objects ---
again, _ = fh.convert(out, V("18.5"), V("18.5"))
check("an iOS object is left as it is", again == out)
two = archive(("a.o", macho()), ("b_long_name_object.o", macho(minos="14.0", sdk="14.2", tail=b"x" * 33)))
out2, done2 = fh.convert(two, V("18.5"), V("18.4"))
check("every object of an archive", [d[0] for d in done2] == ["a.o", "b_long_name_object.o"] and out2.count(struct.pack("<III", 2, V("18.5"), V("18.4"))) == 2,
      done2)
check("an odd-sized member does not throw off the next", done2[1][2:] == (V("14.0"), V("14.2")), done2)

# --- the command line ---
with tempfile.TemporaryDirectory(prefix="firehose-ios-test-") as d:
    a = os.path.join(d, "in.a")
    o = os.path.join(d, "sub", "out.a")
    open(a, "wb").write(src)
    r = run(a, o, "--minos", "18.5", "--sdk", "18.4")
    check("the command line writes the output (and its directory)", r.returncode == 0 and os.path.isfile(o), r.stderr)
    got = open(o, "rb").read()
    check("--minos and --sdk each their own", struct.unpack_from("<III", got, member_start + BV + 8) == (2, V("18.5"), V("18.4")))
    check("it says what it did", "macOS 15.5" in r.stdout and "iOS 18.5 (sdk 18.4)" in r.stdout, r.stdout)
    check("the input is untouched", open(a, "rb").read() == src)
    check("no temporary file is left", sorted(os.listdir(os.path.dirname(o))) == ["out.a"], os.listdir(os.path.dirname(o)))
    r = run(a, a, "--minos", "18.5", "--sdk", "18.5")
    check("input and output may be the same file", r.returncode == 0 and open(a, "rb").read() == out, r.stderr)

    # --- what it refuses: exit 1, a message, no output file ---
    def refuses(label, data, *extra, message=None):
        i = os.path.join(d, "bad.a")
        o2 = os.path.join(d, "bad-out.a")
        if os.path.exists(o2):
            os.unlink(o2)
        open(i, "wb").write(data)
        r = run(i, o2, "--minos", "18.5", "--sdk", "18.5", *extra)
        check("refuses " + label, r.returncode == 1 and not os.path.exists(o2) and r.stderr.startswith("firehose-ios: ") and
              (message is None or message in r.stderr), (r.returncode, r.stderr))

    refuses("a file that is not an archive", b"not an archive at all\n", message="not an ar archive")
    refuses("an empty file", b"")
    refuses("an archive with no object", archive(), message="no object")
    refuses("an archive of the symbol table only", archive(symdef=True), message="no object")
    refuses("a member that is not Mach-O", archive(("readme.txt", b"hello hello hello hello hello hello hello hello\n")), message="Mach-O")
    refuses("a 32-bit Mach-O", archive(("a.o", macho(magic=0xFEEDFACE))), message="64-bit")
    refuses("a fat file", archive(("a.o", b"\xca\xfe\xba\xbe" + macho()[4:])), message="64-bit")
    refuses("a big-endian Mach-O", archive(("a.o", macho(magic=0xCFFAEDFE))), message="64-bit")
    refuses("an x86_64 object", archive(("a.o", macho(cputype=0x01000007))), message="arm64")
    refuses("an object with LC_VERSION_MIN_MACOSX and no LC_BUILD_VERSION", archive(("a.o", macho(build=False))), message="LC_BUILD_VERSION")
    refuses("a tvOS object", archive(("a.o", macho(platform=3))), message="platform is 3")
    refuses("a macCatalyst object", archive(("a.o", macho(platform=6))), message="platform is 6")
    good = archive(("a.o", macho()))
    refuses("an archive cut short", good[:-300], message="past its end")
    refuses("a damaged member header", good[:8 + 58] + b"XX" + good[8 + 60:], message="damaged")
    refuses("a load command that does not fit", archive(("a.o", macho(lc_extra=struct.pack("<II", 0x1D, 4000)))), message="does not fit")
    refuses("a load command of size zero (no endless loop)", archive(("a.o", macho(lc_extra=struct.pack("<II", 0x1D, 0)))), message="does not fit")
    refuses("a bad version", good, "--sdk", "eighteen", message="not a version")
    r = run(os.path.join(d, "nothing.a"), os.path.join(d, "x.a"), "--minos", "18.5", "--sdk", "18.5")
    check("refuses a missing input file", r.returncode == 1 and "firehose-ios:" in r.stderr, r.stderr)

check("the versions: 18.5 and 18.5.1 and 9", V("18.5") == 0x120500 and V("18.5.1") == 0x120501 and V("9") == 0x090000)
check("the versions as shown", fh.show_version(V("18.5")) == "18.5" and fh.show_version(V("18.5.1")) == "18.5.1")

print("firehose-ios-test: %s" % ("FAILED: " + ", ".join(failures) if failures else "all passed"))
sys.exit(1 if failures else 0)
