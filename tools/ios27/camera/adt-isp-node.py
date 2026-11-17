#!/usr/bin/env python3
# SPDX-License-Identifier: MIT-0
"""The device-tree node by which iOS 27 believes the S9 has a camera and shows the Camera app: /arm-io/isp.

    adt-isp-node.py IN.adt OUT.adt [--when auto|yes|no] [--line LINE] [--kernel KERNEL] [--record RECORD]

Why. Camera.app's Info.plist has SBAppTags = [still-camera]: SpringBoard shows the app only where MobileGestalt answers
still-camera. iOS 27's libMobileGestalt (27.0.1, read by disassembly, names only) answers still-camera when the device
tree's /arm-io/isp has camera-front or camera-rear, front-facing-camera from camera-front, rear-facing-camera from
camera-rear: the same node iOS 16.5's loader made for cam=5 (xnuloader/loader.c, camera-rear only). The S9's iOS 27 tree
(tools/ios27/mkadt_starlte.py) has no isp node, so MobileGestalt says no camera and SpringBoard hides the app. The node
has neither compatible nor device_type: no driver matches it, it is read and nothing else.

Both properties: the device (userland/src/camera_device.c) offers a back and a front camera; on a run with the front camera
(cam_s9=7) both streams carry the front sensor's pictures, with cam_rear=1 both carry the rear sensor's (the kernel drives
one camera per boot: docs/ios27/CAMERA.md).

When (--when auto, make-boot-image.sh's): the node is added only if the boot image's command line, with the kernel's
built-in arguments it does not name (tools/ios27/s9/cmdline-inject.py's model of starlte_bootargs_defaults), has
cam_s9=7, so the kernel makes /dev/hakios-camera, AND the root build's record names /usr/lib/cam.dylib for cameracaptured
(tools/ios27/fullroot/steps.d/94-camera.sh), so iOS has the capture device. Otherwise OUT is IN unchanged and the Camera
app stays hidden, as before. --when yes adds it regardless, --when no never. MobileGestalt may keep an answer it made on
an earlier boot in its cache on /var: 94-camera.sh removes that cache from the root it builds, so it is made again.
"""
import argparse
import importlib.util
import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE.parent / "herolte"))
import adt  # noqa: E402

LIBRARY = "/usr/lib/cam.dylib"
PROGRAM = "cameracaptured"


def cstr(s):
    return s.encode() + b"\0"


def u32(v):
    return v.to_bytes(4, "little")


def cmdline_inject():
    spec = importlib.util.spec_from_file_location("cmdline_inject", HERE.parent / "s9" / "cmdline-inject.py")
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def cam_s9(line):
    """the value of the last cam_s9= on the line (the kernel's PE_parse_boot_argn takes the first; the line has one)"""
    value = 0
    for tok in line.replace("\t", " ").split(" "):
        if tok.startswith("cam_s9="):
            m = re.fullmatch(r"cam_s9=(0x[0-9a-fA-F]+|\d+)", tok)
            value = int(m.group(1), 0) if m else 0
            break
    return value


def record_has_camera(path):
    if not path or not Path(path).is_file():
        return False
    for raw in Path(path).read_text().splitlines():
        parts = raw.split("#", 1)[0].split()
        if len(parts) >= 2 and parts[0] == LIBRARY and PROGRAM in parts[1:]:
            return True
    return False


def wanted(args):
    """(add the node?, why)"""
    if args.when == "yes":
        return True, "--when yes"
    if args.when == "no":
        return False, "--when no"
    line = args.line or ""
    if args.kernel:
        ci = cmdline_inject()
        defaults = ci.kernel_defaults(Path(args.kernel).read_bytes())
        if defaults is not None:
            line, _ = ci.merged_line(line, defaults)
    n = cam_s9(line)
    has_lib = record_has_camera(args.record)
    if n >= 7 and has_lib:
        return True, f"cam_s9={n} on the line and {LIBRARY} for {PROGRAM} in the record"
    why = []
    if n < 7:
        why.append(f"the line has {'cam_s9=%d' % n if n else 'no cam_s9='} (the stream is cam_s9=7)")
    if not has_lib:
        why.append(f"the record names no {LIBRARY} for {PROGRAM} (root step 94-camera)")
    return False, " and ".join(why)


def add_node(root):
    arm_io = next(c for c in root.children if c.name == "arm-io")
    arm_io.children = [c for c in arm_io.children if c.name != "isp"]
    isp = adt.Node()
    isp.props = [
        ("name", 0, cstr("isp")),
        ("camera-front", 0, u32(1)),
        ("camera-rear", 0, u32(1)),
    ]
    arm_io.children.append(isp)


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("inp")
    ap.add_argument("out")
    ap.add_argument("--when", choices=("auto", "yes", "no"), default="auto")
    ap.add_argument("--line")
    ap.add_argument("--kernel")
    ap.add_argument("--record")
    args = ap.parse_args(argv)
    data = Path(args.inp).read_bytes()
    add, why = wanted(args)
    if not add:
        Path(args.out).write_bytes(data)
        print(f"adt-isp-node: no /arm-io/isp: {why} (the Camera app stays hidden)")
        return 0
    root, _ = adt.parse(data)
    add_node(root)
    Path(args.out).write_bytes(adt.serialize(root))
    print(f"adt-isp-node: /arm-io/isp with camera-front and camera-rear (the Camera app shown): {why}")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
