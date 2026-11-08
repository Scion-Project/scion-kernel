/* SPDX-License-Identifier: GPL-2.0-only
 * Samsung starlte (SM-G960F, Exynos 9810) stage-0 handoff probe.
 * Based on ios-a5xelte shim/herolte/probe.S (wardevsa, herolte branch).
 * Diagnostic only: no storage, EFS, PMIC or MMU changes. It records the
 * S-Boot handoff on the firmware framebuffer, then reboots into Download
 * mode through the Exynos9810 PMU reset-mode registers (sec_reboot.c,
 * sec_ext.h: INFORM2 0x808, INFORM3 0x80c; exynos-reboot.c: SWRESET 0x400).
 *
 * The arm64 Image header fields text_offset/image_size/flags are written by
 * tools/ios27/starlte-pack-boot.py.
 */
.section .text.head, "ax"
.global _start
_start:
    b entry
    .long 0
    .quad 0x80000            /* text_offset: replaced by the packer */
    .quad 0                  /* image_size: replaced by the packer */
    .quad 0                  /* flags: replaced by the packer */
    .quad 0, 0, 0
    .byte 0x41, 0x52, 0x4d, 0x64
    .long 0

entry:
    msr daifset, #15
    mov x19, x0              /* FDT from S-Boot */
    mrs x20, CurrentEL
    mrs x21, sctlr_el1
    adr x22, _start
    adr x14, star_probe_stack_top
    mov sp, x14
    mov x0, x19
    mov x1, x20
    mov x2, x21
    mov x3, x22
    bl star_probe_main       /* returns only to request Download mode */
    /* Exynos9810 PMU 0x14060000: INFORM2 (reset), INFORM3 (download), SWRESET. */
    ldr x0, =0x14060000
    ldr w14, =0x12345678
    str w14, [x0, #0x808]
    ldr w14, =0x12345671
    str w14, [x0, #0x80c]
    dsb sy
    mov w14, #1
    str w14, [x0, #0x400]
    dsb sy
halt:
    wfe
    b halt
    .ltorg

.section .data, "aw"
.balign 16
star_probe_stack:
    .space 8192
star_probe_stack_top:
