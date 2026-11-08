/* SPDX-License-Identifier: GPL-2.0-only
 * The loader's MMU and caches (Starbite 2, phase 1). S-Boot enters with the MMU off, where every
 * data access is Device-nGnRnE: each framebuffer store waits for DRAM, so full-screen drawing and
 * any animation crawl. With an identity map:
 *
 *   0x000000000..0x07fffffff  Device-nGnRnE, never executable      SoC registers
 *   0x080000000..0x08bffffff  Normal write-back, executable         loader, stack, menu assets
 *                                                                   (0x89000000), S-Boot's FDT
 *                                                                   (0x8a000000), screen copies
 *   0x08c000000..0x0ffffffff  Normal non-cacheable (write-combined)  the OS staging area
 *                                                                   0x8c000000, log page
 *                                                                   0x91fff000, every OS load
 *                                                                   area, UFS DMA 0xaf000000,
 *                                                                   framebuffer 0xcc000000
 *   0x880000000..0x8ffffffff  Normal non-cacheable                   XNU's RAM bank (payload copy)
 *   everything else           unmapped (a fault: EngineerMode's vectors skip it)
 *
 * Nothing that DMA or another core writes is cacheable, so no cache maintenance is needed while
 * the loader runs (except EngineerMode's core mailbox, which lives in .data: em.c invalidates it).
 * Before any OS starts, xnu_boot.S cleans and invalidates the data cache by set/way and turns the
 * MMU off again, the state every OS here is entered in.
 */
#include <stdint.h>
#include "mmu.h"

#define BLOCK      0x1ULL                     /* level 1/2 block descriptor */
#define TABLE      0x3ULL
#define ATTR(i)    ((uint64_t)(i) << 2)       /* MAIR index */
#define SH_INNER   (3ULL << 8)
#define AF         (1ULL << 10)
#define XN         ((1ULL << 53) | (1ULL << 54))
#define DEV        (BLOCK | ATTR(0) | AF | XN)
#define WB         (BLOCK | ATTR(1) | SH_INNER | AF)
#define NC         (BLOCK | ATTR(2) | SH_INNER | AF | XN)
#define MAIR       ((0x44ULL << 16) | (0xffULL << 8) | 0x00ULL)   /* 2 normal NC, 1 normal WB, 0 device */
#define TCR        (28ULL                   /* T0SZ: 36-bit addresses, walks start at level 1 */ \
                    | (1ULL << 8) | (1ULL << 10)   /* IRGN0/ORGN0 write-back write-allocate */ \
                    | (3ULL << 12)                  /* SH0 inner shareable */ \
                    | (1ULL << 23)                  /* EPD1: no TTBR1 walks */ \
                    | (1ULL << 32))                 /* IPS 36-bit */
#define WB_END     0x8c000000ULL

static uint64_t l1[64] __attribute__((aligned(4096)));       /* 64 x 1 GiB */
static uint64_t l2_low[512] __attribute__((aligned(4096)));  /* 0x80000000..0xbfffffff, 2 MiB each */

extern void star_dcache_all(unsigned op);  /* xnu_boot.S: 0 invalidate, 1 clean+invalidate (set/way) */

static int mmu_on;

int star_mmu_enabled(void)
{
    return mmu_on;
}

void star_mmu_enable(void)
{
    uint64_t sctlr;
    __asm__ volatile("mrs %0, sctlr_el1" : "=r"(sctlr));
    if (sctlr & 1)
        return;
    for (unsigned i = 0; i < 64; ++i)
        l1[i] = 0;
    l1[0] = DEV | 0x00000000ULL;
    l1[1] = DEV | 0x40000000ULL;
    for (unsigned i = 0; i < 512; ++i) {
        uint64_t pa = 0x80000000ULL + ((uint64_t)i << 21);
        l2_low[i] = (pa < WB_END ? WB : NC) | pa;
    }
    l1[2] = TABLE | (uint64_t)(uintptr_t)l2_low;
    l1[3] = NC | 0xc0000000ULL;
    l1[34] = NC | 0x880000000ULL;
    l1[35] = NC | 0x8c0000000ULL;
    /* the tables were written with the caches off: in DRAM, which is what the walker reads */
    star_dcache_all(0);
    __asm__ volatile(
        "dsb sy\n"
        "ic iallu\n"
        "tlbi vmalle1\n"
        "dsb sy\n"
        "isb\n"
        "msr mair_el1, %0\n"
        "msr tcr_el1, %1\n"
        "msr ttbr0_el1, %2\n"
        "isb\n"
        :: "r"(MAIR), "r"(TCR), "r"((uint64_t)(uintptr_t)l1) : "memory");
    sctlr |= 1ULL | (1ULL << 2) | (1ULL << 12);   /* M, C, I */
    __asm__ volatile("msr sctlr_el1, %0\nisb" :: "r"(sctlr) : "memory");
    mmu_on = 1;
}

/* Invalidate (and clean) the lines of [addr, addr + len): memory another observer wrote while
 * the MMU was on (EngineerMode's core mailbox). A no-op with the MMU off. */
void star_cache_inval(const volatile void *addr, uint64_t len)
{
    if (!mmu_on)
        return;
    uintptr_t a = (uintptr_t)addr & ~(uintptr_t)63, end = (uintptr_t)addr + len;
    for (; a < end; a += 64)
        __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb sy" ::: "memory");
}
