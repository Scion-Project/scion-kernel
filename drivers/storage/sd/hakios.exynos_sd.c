/*
 * The Galaxy S9's microSD slot (Exynos 9810: dwmmc2, a Synopsys DesignWare MSHC 2.70a at 0x11500000 in FSYS1), read
 * only, behind boot-arg sdcard= (off by default). docs/devices/starlte-microsd.md is its guide.
 *
 * Jesvi-iOS's S20 driver (bsd/dev/redwood_sdcard.c by jesvi-eulah, APSL-2.0, at b35cfdc; the commit before this one
 * carries it unchanged), moved to the S9 with the facts of Scion's docs/ios27/XNU-MICROSD.md and Samsung's device tree
 * (exynos9810-starlte_eur_open_26.dts: dwmmc2@11500000, fifo-depth 64, card-detect gpa1-5, bus-width 4). Like the S20's,
 * it does not bring the card up itself: it uses the slot only when it finds it as a boot loader leaves an SD card it has
 * initialised (powered, clocked, 4-bit, the 64-bit FIFO; then the card is in "tran" and SDHC/SDXC block addressing
 * reads it with CMD17/CMD18). The card's own bring-up (the S2MPS18 rails through ACPM, CMD0/8/ACMD41/2/3/7/ACMD6) is
 * XNU-MICROSD.md's section 2.3 and a later step.
 *
 *   sdcard=N  the microSD slot looked at (1) or read, read only (2); off by default:
 *   sdcard=0  (the default) nothing
 *   sdcard=1  a look, nothing written: the card-detect pin (GPA1-5 of ALIVE, 0x14050044 bit 5, low = a card), the
 *             three clock gates of the card (CMU top 0x1a242070, FSYS1 0x11402030 / 0x11402034), and, only when both FSYS1
 *             gates read forced on (bits 20 and 21), the controller's VERID, HCON, PWREN, CLKENA, CTYPE, CLKSEL, STATUS
 *             and RESP0: whether the slot was left initialised
 *   sdcard=2  as 1, the three gates forced on if they are not (bit 20 manual, bit 21 on, each read back: the only
 *             writes outside the controller; XNU-MICROSD.md 2.3 step 2), then, if a card is in and the slot is as a
 *             loader leaves it, the card's MBR read 6 s after start and published read only:
 *               /dev/disk9, rdisk9    the card up to the end of its last MBR partition
 *               /dev/disk9s1, rdisk9s1  its first MBR partition
 *               IOKit: an external IOBlockStorageDevice "SD Card" -> IOMedia disk9 -> IOMedia disk9s1, Writable no
 *                      (hakios.sdmedia.cpp), what userfsd (Files' external drives) looks for
 *             Never written: the card (no write command exists in this file), the PMIC, the pins, the CMU dividers.
 *   kern.hakios.sdcard (a line of state), sdcard_reads, sdcard_errors, sdcard_dbg, sdcard_last_int
 */

#include <stdbool.h>
#include <sys/param.h>
#include <sys/systm.h>
#include <sys/conf.h>
#include <sys/errno.h>
#include <sys/uio.h>
#include <sys/disk.h>
#include <sys/sysctl.h>
#include <miscfs/devfs/devfs.h>
#include <kern/locks.h>
#include <kern/clock.h>
#include <kern/thread.h>
#include <IOKit/IOLib.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>

extern vm_offset_t ml_io_map(vm_offset_t phys_addr, vm_size_t size);

#define SD_PHYS         0x11500000      /* dwmmc2 (DT reg 0x11500000 + 0x2000) */
#define CMU_TOP_PAGE    0x1a242000      /* GATE_CLKCMU_FSYS1_MMC_CARD at +0x070 */
#define CMU_FSYS1_PAGE  0x11402000      /* MMC_CARD ACLK +0x030, SDCLKIN +0x034 */
#define ALIVE_GPIO_PA   0x14050000      /* GPA1 at +0x040: CON +0, DAT +4 */
#define GPA1_DAT        0x044
#define CD_PIN          5               /* gpa1-5, the card-detect, low with a card in */
#define GATE_MANUAL_ON  ((1u << 20) | (1u << 21))
#define R_CTRL          0x00
#define R_PWREN         0x04
#define R_CLKDIV        0x08
#define R_CLKENA        0x10
#define R_TMOUT         0x14
#define R_CTYPE         0x18
#define R_BLKSIZ        0x1c
#define R_BYTCNT        0x20
#define R_CMDARG        0x28
#define R_CMD           0x2c
#define R_RESP0         0x30
#define R_RINTSTS       0x44
#define R_STATUS        0x48
#define R_VERID         0x6c
#define R_HCON          0x70
#define R_CLKSEL64      0xa8
#define R_FIFO          0x200

#define INT_CD          (1u << 2)       /* command done */
#define INT_DTO         (1u << 3)       /* data transfer over */
#define INT_TXDR        (1u << 4)
#define INT_RXDR        (1u << 5)       /* receive FIFO over its watermark */
#define INT_HTO         (1u << 10)      /* FIFO full, the card clock paused until it is drained: not an error */
#define INT_ERR         0xbbc2u         /* RE, RCRC, DCRC, RTO, DRTO, FRUN, HLE, SBE, EBE */

#define CMD_START       (1u << 31)
#define CMD_HOLD        (1u << 29)
#define CMD_WAIT_PREV   (1u << 13)
#define CMD_AUTO_STOP   (1u << 12)
#define CMD_WRITE       (1u << 10)
#define CMD_DATA        (1u << 9)
#define CMD_CRC         (1u << 8)
#define CMD_RESP        (1u << 6)

#define STATUS_FIFO_FULL (1u << 3)
#define STATUS_DATA_BUSY (1u << 9)
#define SD_R1_ERRORS    0xfff9a080u

#define SD_MAX_BLOCKS   128             /* 64 KB per command */

static volatile uint8_t *sd;
static uint32_t sd_mode;                /* sdcard=: 0 nothing (the default), 1 a look, 2 the card read only */
static int sd_ready;
static uint64_t sd_blocks;              /* 512-byte blocks exposed (minor 0: the card up to its last partition) */
static uint64_t sd_part_start, sd_part_blocks;  /* minor 1: the first MBR partition */
static unsigned sd_part_type;
static int sd_cmaj = -1;
#define SD_UNIT 9                       /* /dev/disk9, disk9s1: well clear of the internal storage's names */
static uint32_t sd_reads, sd_errors, sd_last_int;
static char sd_dbg[200];
LCK_GRP_DECLARE(sd_lck_grp, "hakios-sdcard");
LCK_MTX_DECLARE(sd_lock, &sd_lck_grp);

static inline uint32_t
rd(unsigned off)
{
	return *(volatile uint32_t *)(sd + off);
}

static inline void
wr(unsigned off, uint32_t v)
{
	*(volatile uint32_t *)(sd + off) = v;
}

/* `count` blocks from block `lba` into `out`: CMD18 with auto-stop, PIO. 0 or an errno. sd_lock held. */
static int
sd_read_blocks(uint64_t lba, uint32_t count, uint8_t *out)
{
	/* HCON 0x08e42541: H_DATA_WIDTH 2, a 64-bit FIFO -- only 64-bit reads pop it (Linux: dw_mci_pull_data64) */
	uint32_t want = count * 64, got = 0, st, idle_us = 0;   /* 64-bit entries */

	if (count == 0 || count > SD_MAX_BLOCKS || lba > 0xffffffffull) {
		return EINVAL;
	}
	/* a transfer left half-read (an earlier timeout): empty the FIFO first (CTRL bit 1, FIFO_RESET) */
	if (!(rd(R_STATUS) & (1u << 2))) {
		wr(R_CTRL, rd(R_CTRL) | 2u);
		for (int i = 0; i < 1000 && (rd(R_CTRL) & 2u); i++) {
			delay(1);
		}
	}
	wr(R_RINTSTS, 0xffffffff);
	wr(R_BLKSIZ, 512);
	wr(R_BYTCNT, count * 512);
	wr(R_CMDARG, (uint32_t)lba);
	wr(R_CMD, CMD_START | CMD_HOLD | CMD_WAIT_PREV | (count > 1 ? CMD_AUTO_STOP : 0) | CMD_DATA | CMD_CRC | CMD_RESP |
	    (count > 1 ? 18u : 17u));
	uint32_t st0 = rd(R_STATUS), loops = 0, stmax = 0;
	for (;;) {
		loops++;
		st = rd(R_RINTSTS);
		{
			uint32_t x = rd(R_STATUS);
			if (((x >> 17) & 0x1fff) > ((stmax >> 17) & 0x1fff)) {
				stmax = x;
			}
		}
		if (st & INT_ERR) {
			break;
		}
		/* drain the FIFO until it says empty (STATUS bit 2, FIFO_EMPTY) */
		uint32_t before = got;
		while (got < want && !(rd(R_STATUS) & (1u << 2))) {
			((uint64_t *)out)[got++] = *(volatile uint64_t *)(sd + R_FIFO);
		}
		if (st & (INT_RXDR | INT_HTO)) {
			wr(R_RINTSTS, st & (INT_RXDR | INT_HTO));
		}
		if ((st & INT_DTO) && got >= want) {
			break;
		}
		/* timed out: a second with nothing new (a counted wait: the clock conversions are not up this early in boot) */
		if (got != before) {
			idle_us = 0;
		} else if (++idle_us > 1000000) {
			st |= 1u << 30;
			break;
		} else {
			delay(1);
		}
	}
	sd_last_int = st;
	snprintf(sd_dbg, sizeof(sd_dbg), "cmd %#x: STATUS before %#x, fullest %#x, end %#x; loops %u, words %u/%u; CTRL %#x TBBCNT %#x",
	    rd(R_CMD), st0, stmax, rd(R_STATUS), loops, got, want, rd(R_CTRL), rd(0x60));
	wr(R_RINTSTS, 0xffffffff);
	if ((st & (INT_ERR | (1u << 30))) || got < want) {
		sd_errors++;
		return EIO;
	}
	sd_reads++;
	return 0;
}

static int sd_probe(void);

static int
sd_copen(dev_t dev, int flags, __unused int devtype, __unused struct proc *p)
{
	/* UVFS opens the partition O_RDWR before it asks DKIOCISWRITABLE (Jesvi-iOS): the open is let through for the
	 * partition, every write then refused */
	if (minor(dev) != 1 && (flags & FWRITE)) {
		return EROFS;
	}
	if (!sd_ready) {
		lck_mtx_lock(&sd_lock);
		int error = sd_ready ? 0 : sd_probe();
		lck_mtx_unlock(&sd_lock);
		return error;
	}
	return 0;
}

static int
sd_cclose(__unused dev_t dev, __unused int flags, __unused int devtype, __unused struct proc *p)
{
	return 0;
}

static int
sd_cread(dev_t dev, struct uio *uio, __unused int ioflag)
{
	uint8_t *bounce;
	int error = 0;
	uint64_t size = (minor(dev) == 1 ? sd_part_blocks : sd_blocks) * 512;
	uint64_t first = minor(dev) == 1 ? sd_part_start : 0;

	if (!sd_ready) {
		return ENXIO;
	}
	if (uio_offset(uio) < 0 || uio_offset(uio) % 512 != 0 || uio_resid(uio) % 512 != 0) {
		return EINVAL;
	}
	bounce = kalloc_data(SD_MAX_BLOCKS * 512, Z_WAITOK);
	if (bounce == NULL) {
		return ENOMEM;
	}
	while (error == 0 && uio_resid(uio) > 0 && (uint64_t)uio_offset(uio) < size) {
		uint64_t at = (uint64_t)uio_offset(uio), want = (uint64_t)uio_resid(uio);

		if (want > SD_MAX_BLOCKS * 512) {
			want = SD_MAX_BLOCKS * 512;
		}
		if (want > size - at) {
			want = size - at;
		}
		lck_mtx_lock(&sd_lock);
		error = sd_read_blocks(first + at / 512, (uint32_t)(want / 512), bounce);
		lck_mtx_unlock(&sd_lock);
		if (error == 0) {
			error = uiomove((caddr_t)bounce, (int)want, uio);
		}
	}
	kfree_data(bounce, SD_MAX_BLOCKS * 512);
	return error;
}

static int
sd_cwrite(__unused dev_t dev, __unused struct uio *uio, __unused int ioflag)
{
	return EROFS;                   /* read only: this driver has no write command */
}

static int
sd_ioctl(dev_t dev, u_long cmd, caddr_t data, __unused int flag, __unused struct proc *p)
{
	switch (cmd) {
	case DKIOCGETBLOCKSIZE:
	case DKIOCGETPHYSICALBLOCKSIZE:
		*(uint32_t *)data = 512;
		return 0;
	case DKIOCGETBLOCKCOUNT:
		*(uint64_t *)data = minor(dev) == 1 ? sd_part_blocks : sd_blocks;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTREAD:
		*(uint64_t *)data = SD_MAX_BLOCKS;
		return 0;
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*(uint64_t *)data = 0;
		return 0;
	case DKIOCSYNCHRONIZECACHE:
		return 0;
	case DKIOCISWRITABLE:
		*(uint32_t *)data = 0;
		return 0;
	default:
		return ENOTTY;
	}
}

static const struct cdevsw sd_cdevsw = {
	.d_open       = sd_copen,
	.d_close      = sd_cclose,
	.d_read       = sd_cread,
	.d_write      = sd_cwrite,
	.d_ioctl      = sd_ioctl,
	.d_stop       = eno_stop,
	.d_reset      = eno_reset,
	.d_ttys       = NULL,
	.d_select     = eno_select,
	.d_mmap       = eno_mmap,
	.d_strategy   = eno_strat,
	.d_reserved_1 = eno_getc,
	.d_reserved_2 = eno_putc,
	.d_type       = D_DISK,
};

static char sd_state[200] = "off (boot-arg sdcard=0)";
SYSCTL_DECL(_kern_hakios);
SYSCTL_STRING(_kern_hakios, OID_AUTO, sdcard, CTLFLAG_RD | CTLFLAG_LOCKED, sd_state, 0, "the microSD slot");
SYSCTL_UINT(_kern_hakios, OID_AUTO, sdcard_reads, CTLFLAG_RD | CTLFLAG_LOCKED, &sd_reads, 0, "reads done");
SYSCTL_UINT(_kern_hakios, OID_AUTO, sdcard_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &sd_errors, 0, "reads failed");
SYSCTL_STRING(_kern_hakios, OID_AUTO, sdcard_dbg, CTLFLAG_RD | CTLFLAG_LOCKED, sd_dbg, 0, "the last transfer, in detail");
SYSCTL_UINT(_kern_hakios, OID_AUTO, sdcard_last_int, CTLFLAG_RD | CTLFLAG_LOCKED, &sd_last_int, 0, "last RINTSTS");

static uint32_t sd_verid;

/*
 * The card's MBR, on the first open and not at boot: at driver init the transfer's data reached the FIFO but the
 * reads of it never counted (TBBCNT 0, 2026-11-20); from a running system the same sequence works. sd_lock held.
 */
static int
sd_probe(void)
{
	uint8_t *mbr = kalloc_data(512, Z_WAITOK);
	int error;

	if (mbr == NULL) {
		return ENOMEM;
	}
	error = sd_read_blocks(0, 1, mbr);
	if (error != 0 || mbr[510] != 0x55 || mbr[511] != 0xaa) {
		snprintf(sd_state, sizeof(sd_state), "no card or no MBR: read %d, RINTSTS %#x", error, sd_last_int);
		kfree_data(mbr, 512);
		return ENXIO;
	}
	sd_blocks = 0;
	sd_part_blocks = 0;
	for (unsigned i = 0; i < 4; i++) {          /* the end of the furthest partition */
		const uint8_t *e = mbr + 446 + 16 * i;
		uint64_t start = (uint64_t)e[8] | (uint64_t)e[9] << 8 | (uint64_t)e[10] << 16 | (uint64_t)e[11] << 24;
		uint64_t len = (uint64_t)e[12] | (uint64_t)e[13] << 8 | (uint64_t)e[14] << 16 | (uint64_t)e[15] << 24;

		if (e[4] != 0 && start + len > sd_blocks) {
			sd_blocks = start + len;
		}
		if (e[4] != 0 && sd_part_blocks == 0 && len != 0) {
			sd_part_start = start;
			sd_part_blocks = len;
			sd_part_type = e[4];
		}
	}
	kfree_data(mbr, 512);
	if (sd_blocks == 0) {
		snprintf(sd_state, sizeof(sd_state), "MBR without partitions");
		return ENXIO;
	}
	sd_ready = 1;
	snprintf(sd_state, sizeof(sd_state), "ready: %llu MB (to the last partition), DW MMC %#x, 4-bit, the loader's setup",
	    sd_blocks / 2048, sd_verid);
	printf("sdcard: %s\n", sd_state);
	return 0;
}

extern int hakios_sdmedia_publish(unsigned unit, int major, uint64_t whole_blocks, uint64_t part_start, uint64_t part_blocks,
    unsigned part_type, int writable);

/*
 * At boot (2026-11-21): the card read once IOKit and devfs are up, then /dev/disk9 + rdisk9 (the card) and disk9s1 +
 * rdisk9s1 (its first partition) and the IOKit disk userfsd matches (iokit/Kernel/hakios.sdmedia.cpp). Character
 * devices, read only.
 */
static void
sd_boot_thread(__unused void *arg, __unused wait_result_t wr)
{
	int error;

	IOSleep(6000);
	lck_mtx_lock(&sd_lock);
	error = sd_ready ? 0 : sd_probe();
	lck_mtx_unlock(&sd_lock);
	if (error != 0 || sd_part_blocks == 0) {
		printf("sdcard: no card to publish (%d): %s\n", error, sd_state);
		return;
	}
	(void)devfs_make_node(makedev(sd_cmaj, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0644, "disk%d", SD_UNIT);
	(void)devfs_make_node(makedev(sd_cmaj, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0644, "rdisk%d", SD_UNIT);
	(void)devfs_make_node(makedev(sd_cmaj, 1), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0644, "disk%ds1", SD_UNIT);
	(void)devfs_make_node(makedev(sd_cmaj, 1), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0644, "rdisk%ds1", SD_UNIT);
	error = hakios_sdmedia_publish(SD_UNIT, sd_cmaj, sd_blocks, sd_part_start, sd_part_blocks, sd_part_type, 0);
	snprintf(sd_state, sizeof(sd_state), "ready: disk%d %llu MB, disk%ds1 type %#x at %llu, %llu MB (read only); IOKit %s",
	    SD_UNIT, sd_blocks / 2048, SD_UNIT, sd_part_type, sd_part_start, sd_part_blocks / 2048,
	    error == 0 ? "published" : "FAILED");
	printf("sdcard: %s\n", sd_state);
}

/* one CMU gate register: read, and with force forced on (manual, on) and read back; the value as it is in the end */
static uint32_t
sd_gate(vm_offset_t page, unsigned off, bool force)
{
	volatile uint32_t *g = (volatile uint32_t *)(page + off);
	uint32_t v = *g;

	if (force && (v & GATE_MANUAL_ON) != GATE_MANUAL_ON) {
		*g = v | GATE_MANUAL_ON;
		v = *g;
	}
	return v;
}

void hakios_exynos_sd_init(void);
void
hakios_exynos_sd_init(void)
{
	vm_offset_t top, fsys1, alive;
	uint32_t g_top, g_aclk, g_sdclk, cd, pwren, clkena, ctype, hcon;
	bool card, gates_on;
	int cmaj;
	thread_t t;

	(void)PE_parse_boot_argn("sdcard", &sd_mode, sizeof(sd_mode));
	if (sd_mode == 0) {
		return;
	}
	top = ml_io_map(CMU_TOP_PAGE, 0x1000);
	fsys1 = ml_io_map(CMU_FSYS1_PAGE, 0x1000);
	alive = ml_io_map(ALIVE_GPIO_PA, 0x1000);
	if (top == 0 || fsys1 == 0 || alive == 0) {
		snprintf(sd_state, sizeof(sd_state), "no mapping");
		printf("sdcard: %s\n", sd_state);
		return;
	}
	cd = *(volatile uint32_t *)(alive + GPA1_DAT);
	card = ((cd >> CD_PIN) & 1u) == 0;
	g_top = sd_gate(top, 0x070, sd_mode >= 2);
	g_aclk = sd_gate(fsys1, 0x030, sd_mode >= 2);
	g_sdclk = sd_gate(fsys1, 0x034, sd_mode >= 2);
	gates_on = (g_aclk & GATE_MANUAL_ON) == GATE_MANUAL_ON && (g_sdclk & GATE_MANUAL_ON) == GATE_MANUAL_ON;
	printf("sdcard: sdcard=%u: card detect GPA1 DAT %#x (%s); gates top %#x, FSYS1 ACLK %#x SDCLKIN %#x%s\n", sd_mode, cd,
	    card ? "a card" : "no card", g_top, g_aclk, g_sdclk, sd_mode >= 2 ? " (forced on where they were not)" : "");
	if (!gates_on) {
		/* a read of an IP whose clock is off can stop the bus: the controller is not touched */
		snprintf(sd_state, sizeof(sd_state), "the controller's clocks are not on (ACLK %#x SDCLKIN %#x): not touched",
		    g_aclk, g_sdclk);
		printf("sdcard: %s\n", sd_state);
		return;
	}
	sd = (volatile uint8_t *)ml_io_map(SD_PHYS, 0x1000);
	if (sd == NULL) {
		snprintf(sd_state, sizeof(sd_state), "no mapping");
		return;
	}
	sd_verid = rd(R_VERID);
	hcon = rd(R_HCON);
	pwren = rd(R_PWREN);
	clkena = rd(R_CLKENA);
	ctype = rd(R_CTYPE);
	printf("sdcard: dwmmc2: VERID %#x HCON %#x (data width %u) PWREN %#x CLKENA %#x CLKDIV %#x CTYPE %#x CLKSEL %#x STATUS %#x "
	    "CMD %#x RESP0 %#x\n", sd_verid, hcon, (hcon >> 7) & 7u, pwren, clkena, rd(R_CLKDIV), ctype, rd(R_CLKSEL64),
	    rd(R_STATUS), rd(R_CMD), rd(R_RESP0));
	/* as a loader leaves an initialised SD card: DW MSHC 2.70a, the 64-bit FIFO this driver reads, powered, clocked, 4-bit */
	if ((sd_verid & 0xffffu) != 0x270au || ((hcon >> 7) & 7u) != 2u || !(pwren & 1u) || !(clkena & 1u) || ctype != 1u) {
		snprintf(sd_state, sizeof(sd_state), "the slot is not as a loader leaves an initialised card (VERID %#x HCON %#x "
		    "PWREN %#x CLKENA %#x CTYPE %#x): not used", sd_verid, hcon, pwren, clkena, ctype);
		printf("sdcard: %s\n", sd_state);
		return;
	}
	if (sd_mode < 2 || !card) {
		snprintf(sd_state, sizeof(sd_state), "slot initialised (DW MMC %#x, 4-bit), %s; %s", sd_verid,
		    card ? "a card in" : "no card", sd_mode < 2 ? "sdcard=1: only looked" : "nothing to read");
		printf("sdcard: %s\n", sd_state);
		return;
	}
	cmaj = cdevsw_add(-1, &sd_cdevsw);
	if (cmaj < 0) {
		snprintf(sd_state, sizeof(sd_state), "no device major");
		return;
	}
	sd_cmaj = cmaj;
	(void)devfs_make_node(makedev(cmaj, 0), DEVFS_CHAR, UID_ROOT, GID_OPERATOR, 0640, "rsdcard");
	if (kernel_thread_start(sd_boot_thread, NULL, &t) == KERN_SUCCESS) {
		thread_deallocate(t);
	}
	snprintf(sd_state, sizeof(sd_state), "slot found (DW MMC %#x); the card's MBR is read 6 s after start", sd_verid);
	printf("sdcard: %s\n", sd_state);
}
