/* SPDX-License-Identifier: GPL-2.0-only
 * The microSD card from the loader (sd.c): read only, for the card's OS store (store.c). */
#pragma once
#include <stdint.h>

/* Nothing here touches the card hardware until sd_allow(): store.c allows it when the settings
 * record says so (SET_SD_STORE), EngineerMode's "microSD card" page allows it by hand. */
void sd_allow(void);
int sd_allowed(void);
/* Card up (clocks, pins, rails, controller, SD init): 1 ready, 0 failed (sd_rep says where). */
int sd_up(void);
/* count 512-byte sectors from sector to dst: 0 on success. */
int sd_read(uint64_t sector, uint32_t count, uintptr_t dst);
/* The card's store partition (MBR entry of type 0xDA) in 4 KiB blocks: 1 found. */
int sd_store_area(uint64_t *first, uint64_t *blocks);
/* count 4 KiB blocks from 4 KiB block lba of the card to dst: 0 on success. */
int sd_read4k(uint64_t lba, uint64_t count, uintptr_t dst);

/* What the last sd_up() found, step by step (EngineerMode shows it). */
struct sd_report {
    int state;                  /* 0 not tried, 1 ready, -1 failed */
    const char *why;            /* the failed step */
    int present;                /* card detect GPA1-5 low */
    int rails;                  /* hw_sd_rails: 1 on, 0 refused, -1 no ACPM */
    uint8_t l31, l2;
    uint32_t gate[3], cmu_div, verid, hcon, clksel, clksel_was;
    uint64_t bus_hz, card_hz;
    uint32_t r7, ocr, rca, cid[4];
    int high_capacity;          /* SDHC/SDXC: block addressing */
    uint32_t last_rintsts, last_status;
    int last_error;             /* the last failed read_chunk (-1..-8), 0 none */
    int card_state;             /* CMD13 state after the last recovery (4 tran), -1 no answer */
    uint32_t recoveries;        /* sd_recover() runs */
};
extern struct sd_report sd_rep;
