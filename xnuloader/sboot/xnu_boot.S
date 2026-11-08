/* SPDX-License-Identifier: GPL-2.0-only
 * starlte (SM-G960F) S-Boot Image wrapper that hands off to 4 KiB XNU.
 * Based on ios-a5xelte shim/herolte/xnu_boot.S (wardevsa, herolte branch).
 * No storage accesses. Errors are shown on screen, then the Exynos9810 PMU
 * requests Download mode.
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
    /* Log page 0x91fff000, below the XNU RAM window; XNU marks +0x100. */
    ldr x13, =0x91fff000
    mov x14, x13
    add x15, x13, #4096
inval_log:
    dc ivac, x14
    add x14, x14, #64
    cmp x14, x15
    b.lo inval_log
    dsb sy
    mov x14, x13
zero_log:
    stp xzr, xzr, [x14], #16
    cmp x14, x15
    b.lo zero_log
    ldr x14, =0x31304554524c5453 /* "STRLTE01" */
    str x14, [x13]
    str x20, [x13, #8]
    str x21, [x13, #16]
    str x19, [x13, #24]
    adr x14, _start
    str x14, [x13, #32]
    adr x14, star_boot_stack_top
    mov sp, x14
    /* FP/SIMD on for menu v7 (docs/MENU7-PORT.md 2.2): S-Boot's CPACR_EL1 in x22 (callee-saved, kept
     * through the C code) and in log slot 119; it is put back before any OS is entered. */
    mrs x22, cpacr_el1
    str x22, [x13, #952]
    orr x14, x22, #(3 << 20)
    msr cpacr_el1, x14
    isb
    mov x0, x19
    mov x1, x13
    mov x2, x20
    mov x3, x21
    bl star_prepare_xnu
    ldr x13, =0x91fff000
    cbz x0, fail
    mov x16, x0
    ldr x0, [x13, #136]       /* boot_args (XNU) or FDT (Linux): log[17] */
    /*
     * edk2 PeilessSec reads TPIDRRO_EL0 as the Firmware Handoff transfer-list
     * base even when x1 has no TL signature, and dereferences any nonzero
     * value. S-Boot leaves it stale, so the woa UEFI died in SEC on some boots.
     */
    msr tpidrro_el0, xzr
    /* C cleaned the copied ranges and the log. Every OS here is entered with the MMU off: with
     * the loader's MMU on (mmu.c), first write the whole data cache back (no stores between that
     * and the switch: x0 came from the non-cacheable log page). */
    mrs x14, sctlr_el1
    tbz x14, #0, 1f
    mov x15, x0
    mov x0, #1
    bl star_dcache_all
    mov x0, x15
    mrs x14, sctlr_el1
1:
    bic x14, x14, #1
    bic x14, x14, #4
    bic x14, x14, #0x1000
    msr sctlr_el1, x14
    isb
    ic iallu
    tlbi vmalle1
    dsb sy
    isb
    mov x1, xzr               /* arm64 Linux boot protocol: x1-x3 zero (after star_dcache_all) */
    mov x2, xzr
    mov x3, xzr
    msr cpacr_el1, x22        /* S-Boot's FP/SIMD access: each OS sets up its own */
    isb
    br x16
fail:
    /* Exynos9810 PMU 0x14060000: INFORM2, INFORM3 (download), SWRESET. */
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

/*
 * star_dcache_all(op): the whole data cache by set/way, every level up to the LoC: op 0 =
 * invalidate (DC ISW), 1 = clean + invalidate (DC CISW). Leaf, uses x0-x12 only (the handoff
 * above keeps the entry in x16 and x15 across it).
 */
.global star_dcache_all
star_dcache_all:
    mov x12, x0
    dmb sy
    mrs x0, clidr_el1
    and w3, w0, #0x07000000
    lsr w3, w3, #23             /* LoC * 2 */
    cbz w3, 5f
    mov w10, #0                 /* level * 2 */
1:  add w2, w10, w10, lsr #1    /* level * 3 */
    lsr w1, w0, w2
    and w1, w1, #7
    cmp w1, #2
    b.lt 4f                     /* no data cache at this level */
    msr csselr_el1, x10
    isb
    mrs x1, ccsidr_el1
    and w2, w1, #7
    add w2, w2, #4              /* log2(line bytes) */
    mov w4, #0x3ff
    and w4, w4, w1, lsr #3      /* ways - 1 */
    clz w5, w4                  /* way field position */
    mov w7, #0x7fff
    and w7, w7, w1, lsr #13     /* sets - 1 */
2:  mov w9, w4
3:  lsl w6, w9, w5
    orr w11, w10, w6
    lsl w6, w7, w2
    orr w11, w11, w6
    cbz x12, 6f
    dc cisw, x11
    b 7f
6:  dc isw, x11
7:  subs w9, w9, #1
    b.ge 3b
    subs w7, w7, #1
    b.ge 2b
4:  add w10, w10, #2
    cmp w3, w10
    b.gt 1b
5:  mov x10, #0
    msr csselr_el1, x10
    dsb sy
    isb
    ret
    .ltorg

.section .data, "aw"
.balign 16
star_boot_stack:
    .space 8192
star_boot_stack_top:
