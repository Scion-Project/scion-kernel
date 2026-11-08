/* SPDX-License-Identifier: GPL-2.0-only
 * Starbite POST, the power-on self test (post.c, docs/STARBITE-RECOVERY.md section 4): a few
 * bounded checks at every boot, shown like a PC BIOS at the top of the panel, logged to the
 * pmsg ring and the log page (slots 200..215), kept in RAM for the menu (post_last()).
 */
#pragma once
#include <stdint.h>

#define POST_QUICK 0U       /* every boot: about 0.3 s on a healthy phone */
#define POST_FULL  1U       /* the recovery menu's "Run POST (full)": a few seconds */

#define POST_SKIP 0U        /* not run (state of a check) */
#define POST_OK   1U
#define POST_WARN 2U        /* shown in gold: worth a look, never holds the boot */
#define POST_FAIL 3U        /* shown in red: the boot pauses 1.5 s so it can be read */

#define POST_MAX 12U

/* The checks, in order (post_check.id). */
#define POST_RAM     0U
#define POST_TIMER   1U
#define POST_PMIC    2U
#define POST_UFS     3U
#define POST_GPT     4U
#define POST_STORE   5U
#define POST_CATALOG 6U
#define POST_BATTERY 7U
#define POST_TEMP    8U
#define POST_TOUCH   9U
#define POST_MICROSD 10U
#define POST_N       11U

struct post_check {
    char name[9];           /* "RAM", "BATTERY", ... */
    uint8_t state;          /* POST_SKIP .. POST_FAIL */
    uint8_t fatal;          /* this failure stops the boot (RAM, timer): recovery comes up */
    int32_t value;          /* the check's number: battery %, degrees C, entries, MHz x 10 ... */
    uint32_t us;            /* how long it took */
    char detail[24];        /* what the line says ("84 %", "V2 41 ENTRIES") */
};

struct post_report {
    uint32_t magic;         /* POST_MAGIC once a run has finished */
    uint32_t mode;          /* POST_QUICK or POST_FULL */
    uint32_t n, fails, warns, fatal;
    uint64_t total_us;
    struct post_check c[POST_MAX];
};
#define POST_MAGIC 0x54534f50U   /* "POST" */

/* The quick POST of a normal boot: draws at the top of the panel (rows saved first and put back
 * after, so the menu's intro sees S-Boot's splash), pauses 1.5 s after a red line. touch: also
 * bring the touch controller up (the boot menu does that anyway; 0 skips it). *bixby = 1 if
 * Bixby was held during the run. Returns 1 if a fatal check failed (*why says which). */
int post_boot(int touch, int *bixby, const char **why);
/* Any POST, drawn at fbtext rows row0.. as the row base stands (recovery.c). Returns the fatal
 * flag as post_boot. */
int post_run(unsigned mode, unsigned row0, int touch, int *bixby);
/* The last finished run, or 0 before the first. */
const struct post_report *post_last(void);
/* How many of its checks passed. */
unsigned post_ok_count(const struct post_report *r);
