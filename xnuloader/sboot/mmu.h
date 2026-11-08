/* SPDX-License-Identifier: GPL-2.0-only
 * The loader's identity map and caches (mmu.c). */
#ifndef STAR_MMU_H
#define STAR_MMU_H
#include <stdint.h>

void star_mmu_enable(void);
int star_mmu_enabled(void);
void star_cache_inval(const volatile void *addr, uint64_t len);

#endif
