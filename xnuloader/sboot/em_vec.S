/* SPDX-License-Identifier: GPL-2.0-only
 * EngineerMode exception vectors and the core bring-up stub.
 *
 * em_vectors: while EngineerMode runs, VBAR_EL1 points here instead of at
 * S-Boot's table, so a trapped system register read or a synchronous
 * external abort from a probed address is recorded in em_fault and skipped
 * (ELR + 4) instead of resetting the phone. SError (asynchronous) is only
 * counted. em.c restores S-Boot's VBAR before it returns.
 *
 * em_core_entry: target of PSCI CPU_ON. Records MPIDR_EL1 in em_core_box
 * (slot from the MPIDR affinity fields), then calls PSCI
 * CPU_OFF; if that returns, records the result and parks in WFE.
 * MMU and caches are off on every core, so plain stores are coherent.
 */
.section .text
.balign 2048
.global em_vectors
em_vectors:
    .irp group, 0, 1, 2, 3
    .balign 0x80
    b em_sync
    .balign 0x80
    b em_other
    .balign 0x80
    b em_other
    .balign 0x80
    b em_serror
    .endr

em_sync:
    stp x0, x1, [sp, #-16]!
    adr x1, em_fault
    mrs x0, esr_el1
    str x0, [x1]
    mrs x0, elr_el1
    str x0, [x1, #8]
    mrs x0, far_el1
    str x0, [x1, #16]
    ldr x0, [x1, #24]
    add x0, x0, #1
    str x0, [x1, #24]
    mrs x0, elr_el1
    add x0, x0, #4
    msr elr_el1, x0
    ldp x0, x1, [sp], #16
    eret

em_serror:
    stp x0, x1, [sp, #-16]!
    adr x1, em_fault
    mrs x0, esr_el1
    str x0, [x1, #32]
    ldr x0, [x1, #40]
    add x0, x0, #1
    str x0, [x1, #40]
    ldp x0, x1, [sp], #16
    eret

em_other:
    eret

.global em_core_entry
em_core_entry:
    /* PSCI 0.1 here passes no context id (build/stub/park.S): slot from
     * MPIDR, Aff2 (cluster) * 4 + Aff1 (core; MT = 1, Aff0 = thread). */
    mrs x2, mpidr_el1
    ubfx x3, x2, #8, #2
    ubfx x4, x2, #16, #1
    add x0, x3, x4, lsl #2
    adr x1, em_core_box
    add x1, x1, x0, lsl #5
    str x2, [x1, #8]
    mrs x2, CurrentEL
    str x2, [x1, #16]
    mov x2, #1
    str x2, [x1]                /* state 1: alive */
    dsb sy
    mov x9, x1
    movz w0, #0x8400, lsl #16
    movk w0, #0x0002            /* PSCI CPU_OFF */
    smc #0
    str x0, [x9, #24]           /* CPU_OFF returned (it should not) */
    mov x2, #2
    str x2, [x9]                /* state 2: parked */
    dsb sy
1:  wfe
    b 1b

.section .data
.balign 8
.global em_fault
em_fault:
    .quad 0, 0, 0, 0, 0, 0      /* esr, elr, far, count, serror esr, serror count */
.global em_core_box
em_core_box:
    .fill 8 * 4, 8, 0           /* per slot: state, mpidr, el, cpu_off ret */
