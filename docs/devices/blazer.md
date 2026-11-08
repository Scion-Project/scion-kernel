<!-- Made by tools/docs-gen.py from devices/blazer/device.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Pixel 10 Pro (`blazer`)

| | |
|---|---|
| Vendor | Google |
| SoC | `lga` |
| Configuration | `make blazer_defconfig` |
| Maintainer | Scion (README.md) |
| Repository | [`devices/blazer/`](https://github.com/Scion-Project/scion-kernel/tree/main/devices/blazer) |

## What works

| Part | State | |
|---|---|---|
| display | no | ios-on-pixel-10 drives the panel from its boot shim; Scion has the lga shim (socs/lga/shim) on xnu-8796 only |
| cpu | no | pristine xnu-12377 has no Tensor G5 platform support; the lga layer is xnu-8796's |
| touch | no |  |
| buttons | no |  |
| storage | no | carried/dualboot: a read-only USB readback of one partition, not built |
| gpu | no | PowerVR DXT-48-1536 |
| audio | no |  |
| battery | no | carried/power: MAX77779 register decoding, not built; charging writes need the owner's go |
| wifi | no |  |
| bluetooth | no |  |
| cellular | no |  |
| thermal | no |  |
| usb | no |  |
| haptic | no | carried/haptic: tap policy and ROM-effect service headers, not built |
| sensors | no | carried/sensors: MMC5616WA compass; carried/nfc: NFC bring-up; neither built |

The state is what has been seen on this device, written down in `devices/blazer/device.conf`; a part the table does not name has not been looked at.
