#!/usr/bin/env python3
"""tools/ios27/firehose-ios.py IN.a OUT.a [--minos X.Y[.Z]] [--sdk X.Y[.Z]]

The iOS-target kernel's libfirehose_kernel.a, made from the macOS-target one.

darwin-xnu-build builds libdispatch's libfirehose_kernel (the kernel's logging buffer code) against the macOS SDK. The kernel of the
`ios-27` profile is an iPhoneOS-target XNU, and ld refuses a macOS object in an iOS link ("building for iOS, but linking in object file
built for macOS"). Building the library against the iPhoneOS SDK does not work (libdispatch's xcodebuild target does not find
<Availability.h> there), and `vtool -set-build-version` refuses the object ("not enough space to hold load commands"). The object
itself is the same code for either: only the platform in its LC_BUILD_VERSION differs. So this changes that load command's platform
(macOS -> iOS), minos and sdk, in place, in every Mach-O member of the archive, and nothing else: the archive's size, its symbol
table (__.SYMDEF SORTED, whose offsets stay right because no member moves), the members' headers and every other byte are the input's.

    tools/ios27/firehose-ios.py fakeroot/usr/local/lib/kernel/libfirehose_kernel.a build/firehose-ios/libfirehose_kernel.a

--minos and --sdk (default: the iphoneOS SDK's version, from `xcrun --sdk iphoneos --show-sdk-version`, else 18.5, the SDK of Xcode 16.4)
are what the kernel's own objects carry (-miphoneos-version-min). Run by dxb-ios.patch's hook (XNU_FIREHOSE_IOS) in
tools/ios27/build-xnu-ios.sh. Only the standard library; tools/ios27/firehose-ios-test.py tests it. Fails, and writes nothing, for an
input that is not a 64-bit arm64 Mach-O archive with an LC_BUILD_VERSION (macOS or already iOS) in each of its objects.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile

MH_MAGIC_64 = 0xFEEDFACF
CPU_TYPE_ARM64 = 0x0100000C
LC_BUILD_VERSION = 0x32
PLATFORM_MACOS = 1
PLATFORM_IOS = 2
AR_MAGIC = b"!<arch>\n"
AR_HEADER = 60
DEFAULT_VERSION = "18.5"


class Refuse(Exception):
    pass


def parse_version(text):
    """'18.5' -> 0x00120500 (nibbles of Mach-O's xxxx.yy.zz)"""
    parts = text.split(".")
    if not 1 <= len(parts) <= 3 or not all(p.isdigit() and int(p) < 256 for p in parts) or int(parts[0]) > 0xFFFF:
        raise Refuse("not a version (X.Y or X.Y.Z): %r" % text)
    nums = [int(p) for p in parts] + [0] * (3 - len(parts))
    return (nums[0] << 16) | (nums[1] << 8) | nums[2]


def show_version(v):
    return "%d.%d" % (v >> 16, (v >> 8) & 0xFF) + (".%d" % (v & 0xFF) if v & 0xFF else "")


def sdk_version():
    try:
        out = subprocess.run(["xcrun", "--sdk", "iphoneos", "--show-sdk-version"], capture_output=True, text=True, timeout=60)
        if out.returncode == 0 and out.stdout.strip():
            parse_version(out.stdout.strip())
            return out.stdout.strip()
    except (OSError, subprocess.SubprocessError, Refuse):
        pass
    return DEFAULT_VERSION


def members(data):
    """(name, offset, size) of each member of a BSD/Darwin ar archive; the offset is of the member's contents"""
    if data[: len(AR_MAGIC)] != AR_MAGIC:
        raise Refuse("not an ar archive")
    pos = len(AR_MAGIC)
    while pos < len(data):
        if pos + AR_HEADER > len(data) or data[pos + 58: pos + 60] != b"`\n":
            raise Refuse("the archive's member header at %d is damaged" % pos)
        name = data[pos: pos + 16].decode("latin-1").strip()
        try:
            size = int(data[pos + 48: pos + 58].decode("latin-1").strip())
        except ValueError:
            raise Refuse("the archive's member header at %d has no size" % pos)
        start = pos + AR_HEADER
        if name.startswith("#1/"):  # BSD long name: the name opens the contents and counts in the size
            try:
                n = int(name[3:])
            except ValueError:
                raise Refuse("a bad long name at %d" % pos)
            if n > size or start + size > len(data):
                raise Refuse("the archive's member at %d runs past its end" % pos)
            name = data[start: start + n].split(b"\0")[0].decode("latin-1")
            start += n
            size -= n
        elif start + size > len(data):
            raise Refuse("the archive's member at %d runs past its end" % pos)
        yield name, start, size
        pos = start + size
        pos += pos & 1  # members start on even offsets


def retarget(buf, start, size, name, minos, sdk):
    """change the LC_BUILD_VERSION of the Mach-O object at buf[start:start+size]; (old platform, minos, sdk)"""
    if size < 32 or struct.unpack_from("<I", buf, start)[0] != MH_MAGIC_64:
        raise Refuse("%s: not a 64-bit little-endian Mach-O object" % name)
    cputype, _, _, ncmds, sizeofcmds = struct.unpack_from("<iiIII", buf, start + 4)
    if cputype != CPU_TYPE_ARM64:
        raise Refuse("%s: cputype %#x, not arm64" % (name, cputype))
    if 32 + sizeofcmds > size:
        raise Refuse("%s: the load commands run past the object" % name)
    pos, end = start + 32, start + 32 + sizeofcmds
    found = None
    for _ in range(ncmds):  # every command is read, so that a damaged one after LC_BUILD_VERSION is refused too
        if pos + 8 > end:
            raise Refuse("%s: the load commands are cut short" % name)
        cmd, cmdsize = struct.unpack_from("<II", buf, pos)
        if cmdsize < 8 or cmdsize % 4 or pos + cmdsize > end:
            raise Refuse("%s: a load command of %d bytes does not fit" % (name, cmdsize))
        if cmd == LC_BUILD_VERSION:
            if cmdsize < 24:
                raise Refuse("%s: LC_BUILD_VERSION is %d bytes" % (name, cmdsize))
            if found is not None:
                raise Refuse("%s: two LC_BUILD_VERSION commands" % name)
            found = pos
        pos += cmdsize
    if found is None:
        raise Refuse("%s: no LC_BUILD_VERSION (an object built with -mmacosx-version-min alone has LC_VERSION_MIN_MACOSX, which has no room for it)" % name)
    platform, old_minos, old_sdk = struct.unpack_from("<III", buf, found + 8)
    if platform not in (PLATFORM_MACOS, PLATFORM_IOS):
        raise Refuse("%s: its platform is %d: this makes an iOS object from a macOS one" % (name, platform))
    struct.pack_into("<III", buf, found + 8, PLATFORM_IOS, minos, sdk)
    return platform, old_minos, old_sdk


def convert(data, minos, sdk):
    """the archive's bytes with every object retargeted; the list of what was changed"""
    buf = bytearray(data)
    done = []
    for name, start, size in members(data):
        if name.startswith("__.SYMDEF"):
            continue  # the symbol table: offsets of members, which do not move
        platform, old_minos, old_sdk = retarget(buf, start, size, name, minos, sdk)
        done.append((name, platform, old_minos, old_sdk))
    if not done:
        raise Refuse("the archive has no object")
    return bytes(buf), done


def main(argv=None):
    ap = argparse.ArgumentParser(description="libfirehose_kernel.a for an iOS-target kernel, from the macOS-target one", add_help=True)
    ap.add_argument("input")
    ap.add_argument("output")
    ap.add_argument("--minos", help="deployment target (default: the iphoneos SDK's version, else %s)" % DEFAULT_VERSION)
    ap.add_argument("--sdk", help="SDK version (default: the iphoneos SDK's version, else %s)" % DEFAULT_VERSION)
    args = ap.parse_args(argv)
    try:
        default = None if args.minos and args.sdk else sdk_version()
        minos = parse_version(args.minos or default)
        sdk = parse_version(args.sdk or default)
        with open(args.input, "rb") as f:
            data = f.read()
        out, done = convert(data, minos, sdk)
    except (Refuse, OSError) as e:
        sys.stderr.write("firehose-ios: %s\n" % e)
        return 1
    directory = os.path.dirname(os.path.abspath(args.output))
    os.makedirs(directory, exist_ok=True)
    fd, tmp = tempfile.mkstemp(dir=directory, prefix=".firehose-ios-")
    try:
        with os.fdopen(fd, "wb") as f:
            f.write(out)
        os.chmod(tmp, 0o644)
        os.replace(tmp, args.output)
    except BaseException:
        if os.path.exists(tmp):
            os.unlink(tmp)
        raise
    for name, platform, old_minos, old_sdk in done:
        print("firehose-ios: %s: %s: %s %s (sdk %s) -> iOS %s (sdk %s)" % (
            os.path.basename(args.output), name, "macOS" if platform == PLATFORM_MACOS else "iOS", show_version(old_minos),
            show_version(old_sdk), show_version(minos), show_version(sdk)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
