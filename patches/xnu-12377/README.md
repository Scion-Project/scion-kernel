# patches/xnu-12377: the ios-26 base

Apple's `xnu-12377.121.6` plus `series`, applied by `xnu/apply-series.sh` when the OS profile's `base` is `xnu-12377`
(`OS_PROFILE=ios-26`, or `make blazer_defconfig`). The series is empty today. Patches here are Scion's own, under the APSL like
`patches/xnu-8796/`. Nothing derived from `ios-tegu` (no licence) and no GPL code goes in: `docs/two-bases.md`.
