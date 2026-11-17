<!-- Made by tools/docs-gen.py from devices/*/device.conf and socs/*/soc.conf. Do not edit: change that, then run tools/docs-gen.py. -->


# Devices

Every device with a repository, and how far each part of it works. A device of yours: [Port a device](../porting/index.md).

| Device | SoC | display | touch | buttons | cpu | gpu | storage | audio | battery | haptic | wifi | bluetooth | cellular | network | thermal | usb | sensors |
|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|
| [Pixel 10 Pro](blazer.md) | `lga` | no | no | no | no | no | no | no | no | no | no | no | no |  | no | no | no |
| [QEMU virt (aarch64)](qemu.md) | `none` | yes | yes | yes |  | no | yes | yes |  |  |  |  |  | yes |  |  |  |

An empty cell is a part nobody has looked at on that device yet.

## SoC families

| SoC | What | |
|---|---|---|
| `exynos2100` | Samsung Exynos 2100 (S5E9840) | `socs/exynos2100/` |
| `exynos850` | Samsung Exynos 850 (S5E3830) | `socs/exynos850/` |
| `exynos8895` | Samsung Exynos 8895 (S5E8895) | `socs/exynos8895/` |
| `exynos9810` | Samsung Exynos 9810 (S5E9810) | `socs/exynos9810/` |
| `exynos9820` | Samsung Exynos 9820 (S5E9820) | `socs/exynos9820/` |
| `exynos990` | Samsung Exynos 990 (S5E9830) | `socs/exynos990/` |
| `gs101` | Google Tensor G1 (gs101) | `socs/gs101/` |
| `gs201` | Google Tensor G2 (gs201) | `socs/gs201/` |
| `zuma` | Google Tensor G3 (zuma) | `socs/zuma/` |
| `zumapro` | Google Tensor G4 (zumapro) | `socs/zumapro/` |
