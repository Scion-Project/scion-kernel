#pragma once
#include <stdint.h>

/* Read a u32/u64 (cell-size agnostic) property of /chosen. Returns 0 if absent. */
int fdt_chosen_u64(uintptr_t fdt, const char *prop, uint64_t *out);
/* Overwrite an existing u32/u64 property of /chosen in place. Returns 0 if absent. */
int fdt_chosen_set_u64(uintptr_t fdt, const char *prop, uint64_t value);
/* Raw /chosen property (address in the FDT + length). Returns 0 if absent. */
int fdt_chosen_raw(uintptr_t fdt, const char *prop, uintptr_t *val, uint32_t *len);
/* DRAM banks from the top-level memory nodes; returns how many were stored. */
int fdt_memory_banks(uintptr_t fdt, uint64_t *base, uint64_t *size, int max);
/* Copy the FDT to out (cap bytes) with " extra" appended to /chosen/bootargs.
 * Returns the new total size, 0 on failure. */
uint32_t fdt_copy_append_bootargs(uintptr_t fdt, uintptr_t out, uint32_t cap, const char *extra);
