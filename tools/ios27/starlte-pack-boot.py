#!/usr/bin/env python3
"""Inspect or build a Samsung starlte (SM-G960F) BOOT image.

Host-side only: never touches the phone. Modeled on ios-a5xelte
tools/ios27/herolte-pack-boot.py (wardevsa, herolte branch).

S-Boot on this phone takes an Android v0 boot header whose last field is the
size of a Samsung DTBH blob placed after the kernel/ramdisk/second sections
(page size 2048, kernel at 0x10008000). The template is any image that is
known to boot on the phone (the official TWRP starlte image, or stock BOOT):
its header addresses, cmdline, os_version and DTBH are kept; only the kernel
(and ramdisk) are replaced.

  info TEMPLATE
  pack TEMPLATE IMAGE OUT [--append PAYLOAD] [--ramdisk R] [--cmdline S]
  kernel BOOTIMG OUT      (write the kernel slot, e.g. the woa UEFI's bootshim + FD)
"""
import argparse
import hashlib
from pathlib import Path
import struct

ARM64_MAGIC = b"ARMd"
BOOT_PARTITION_SIZE = 14080 * 4096   # PIT: BOOT, 14080 blocks of 4 KiB


def parse(boot):
    if boot[:8] != b"ANDROID!":
        raise SystemExit("not an Android BOOT image")
    (kernel_size, kernel_addr, ramdisk_size, ramdisk_addr, second_size,
     second_addr, tags_addr, page, dt_size, os_version) = struct.unpack_from("<10I", boot, 8)
    if page != 2048:
        raise SystemExit(f"unexpected page size {page}")
    align = lambda size: (size + page - 1) // page * page
    kernel_off = page
    ramdisk_off = kernel_off + align(kernel_size)
    second_off = ramdisk_off + align(ramdisk_size)
    dt_off = second_off + align(second_size)
    if dt_off + dt_size > len(boot):
        raise SystemExit("image is truncated")
    return {
        "kernel_addr": kernel_addr, "ramdisk_addr": ramdisk_addr,
        "second_addr": second_addr, "tags_addr": tags_addr, "page": page,
        "os_version": os_version, "align": align,
        "name": boot[48:64], "cmdline": boot[64:576],
        "extra_cmdline": boot[608:1632],
        "kernel": boot[kernel_off:kernel_off + kernel_size],
        "ramdisk": boot[ramdisk_off:ramdisk_off + ramdisk_size],
        "dt": boot[dt_off:dt_off + dt_size],
    }


def image_header(kernel):
    if len(kernel) < 64 or kernel[56:60] != ARM64_MAGIC:
        return None
    return struct.unpack_from("<3Q", kernel, 8)


def info(path):
    b = parse(Path(path).read_bytes())
    for k in ("kernel_addr", "ramdisk_addr", "second_addr", "tags_addr", "page", "os_version"):
        print(f"{k:13} {b[k]:#010x}")
    print(f"cmdline       {b['cmdline'].split(bytes(1))[0].decode(errors='replace')!r}")
    print(f"kernel        {len(b['kernel'])} bytes")
    hdr = image_header(b["kernel"])
    if hdr:
        print("  arm64 Image text_offset %#x image_size %#x flags %#x" % hdr)
    print(f"ramdisk       {len(b['ramdisk'])} bytes")
    print(f"dt            {len(b['dt'])} bytes, magic {b['dt'][:4]!r}")


def pack(args):
    tpl = parse(Path(args.template).read_bytes())
    kernel = bytearray(Path(args.image).read_bytes())
    ours = image_header(kernel)
    stock = image_header(tpl["kernel"])
    if ours is None:
        raise SystemExit(f"{args.image}: no arm64 Image header")
    if stock is None:
        raise SystemExit("template kernel has no arm64 Image header")
    # S-Boot reserves and places the kernel by these fields: use the ones that boot.
    struct.pack_into("<3Q", kernel, 8, *stock)
    if args.append:
        # xnu_boot looks for the payload at its 4 KiB-aligned _image_end and
        # bounds it by image_size (as herolte-pack-boot.py: file + 64 KiB).
        payload = Path(args.append).read_bytes()
        if payload[:8] != b"SWEETXNU":
            raise SystemExit("--append payload is not a SWEETXNU container")
        kernel += bytes((-len(kernel)) % 4096)
        kernel += payload
        image_size = (len(kernel) + 0x10000 + 0xfff) & ~0xfff
        struct.pack_into("<Q", kernel, 16, max(image_size, stock[1]))
        stock = image_header(kernel)
    ramdisk = Path(args.ramdisk).read_bytes() if args.ramdisk else bytes(4096)

    page, align = tpl["page"], tpl["align"]
    dt = tpl["dt"]
    cmdline = tpl["cmdline"] if args.cmdline is None else args.cmdline.encode().ljust(512, b"\0")
    if len(cmdline) > 512:
        raise SystemExit("cmdline longer than 512 bytes")

    sha = hashlib.sha1()
    for blob in (bytes(kernel), ramdisk, b"", dt):
        sha.update(blob)
        sha.update(struct.pack("<I", len(blob)))

    hdr = bytearray(page)
    hdr[:8] = b"ANDROID!"
    struct.pack_into("<10I", hdr, 8, len(kernel), tpl["kernel_addr"], len(ramdisk),
                     tpl["ramdisk_addr"], 0, tpl["second_addr"], tpl["tags_addr"],
                     page, len(dt), tpl["os_version"])
    hdr[48:64] = tpl["name"]
    hdr[64:576] = cmdline
    hdr[576:596] = sha.digest()
    hdr[608:1632] = tpl["extra_cmdline"]

    out = bytearray(hdr)
    for blob in (bytes(kernel), ramdisk, dt):
        out += blob + bytes(align(len(blob)) - len(blob))
    if len(out) > BOOT_PARTITION_SIZE:
        raise SystemExit(f"{len(out)} bytes does not fit BOOT ({BOOT_PARTITION_SIZE})")
    Path(args.out).write_bytes(out)
    print(f"{args.out}: {len(out)} bytes (kernel {len(kernel)}, ramdisk {len(ramdisk)}, "
          f"dt {len(dt)}); Image header text_offset %#x image_size %#x flags %#x" % stock)
    print(f"sha256 {hashlib.sha256(out).hexdigest()}")


def kernel(args):
    k = parse(Path(args.boot).read_bytes())["kernel"]
    if not image_header(k):
        raise SystemExit(f"{args.boot}: kernel slot is not an arm64 Image")
    Path(args.out).write_bytes(k)
    print(f"{args.out}: {len(k)} bytes")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sub = ap.add_subparsers(dest="cmd", required=True)
    i = sub.add_parser("info")
    i.add_argument("template")
    p = sub.add_parser("pack")
    p.add_argument("template")
    p.add_argument("image")
    p.add_argument("out")
    p.add_argument("--append")
    p.add_argument("--ramdisk")
    p.add_argument("--cmdline")
    k = sub.add_parser("kernel")
    k.add_argument("boot")
    k.add_argument("out")
    args = ap.parse_args()
    {"info": lambda: info(args.template), "pack": lambda: pack(args), "kernel": lambda: kernel(args)}[args.cmd]()


if __name__ == "__main__":
    main()
