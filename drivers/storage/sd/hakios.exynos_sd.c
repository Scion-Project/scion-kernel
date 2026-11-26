/*
 * The S20's microSD slot (A, 2026-11-20; docs/devices/x1slte/sdcard.md): Synopsys DesignWare MMC 2.70a ("dwmmc2" at
 * 0x132E0000 in HSI1, the block of the embedded UFS and the Wi-Fi PCIe, so always powered while iOS runs).
 *
 * S-Boot leaves the slot initialised: card powered (PWREN 1), clocked (CLKENA 1), 4-bit bus (CTYPE 1), the card in
 * the "tran" state after its own CMD17. The driver keeps that state and uses SDHC/SDXC block addressing.
 * It reads by PIO through the FIFO at +0x200 and uses CMD24 for writes on the first partition.
 * sd_write=0 makes the slot read-only, and sd=0 disables the driver.
 *
 *   /dev/rsdcard   raw, read-only; its size: the end of the last partition in the card's MBR (the card's own size,
 *                  CSD, needs its RCA, which only a re-identification gives -- next step)
 *   kern.redwood.sdcard   a line of state
 */

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

#define SD_PHYS         0x132e0000
#define R_CTRL          0x00
#define R_PWREN         0x04
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
static uint32_t sd_mode = 1;            /* sd=0 disables the driver */
static uint32_t sd_write_enabled = 1;  /* sd_write=0 disables partition writes */
static int sd_ready;
static uint64_t sd_blocks;              /* 512-byte blocks exposed (minor 0: the card up to its last partition) */
static uint64_t sd_part_start, sd_part_blocks;  /* minor 1: the first MBR partition */
static unsigned sd_part_type;
static int sd_cmaj = -1;
#define SD_UNIT 9                       /* /dev/disk9, disk9s1: well clear of the internal storage's names */
static uint32_t sd_reads, sd_errors, sd_last_int;
static char sd_dbg[200];
LCK_GRP_DECLARE(sd_lck_grp, "redwood-sdcard");
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

static int
sd_wait_not_busy(void)
{
	for (unsigned i = 0; i < 1000; i++) {
		if (!(rd(R_STATUS) & STATUS_DATA_BUSY)) {
			return 0;
		}
		IOSleep(1);
	}
	return ETIMEDOUT;
}

/* Send one SDHC block with CMD24. Caller holds sd_lock. */
static int
sd_write_sector(uint64_t lba, const uint64_t *data)
{
	uint32_t status = 0;
	unsigned sent = 0;

	if (lba > UINT32_MAX) {
		return EINVAL;
	}
	int error = sd_wait_not_busy();
	if (error != 0) {
		return error;
	}
	wr(R_RINTSTS, UINT32_MAX);
	wr(R_BLKSIZ, 512);
	wr(R_BYTCNT, 512);
	wr(R_CMDARG, (uint32_t)lba);
	wr(R_CMD, CMD_START | CMD_HOLD | CMD_WAIT_PREV | CMD_WRITE | CMD_DATA | CMD_CRC | CMD_RESP | 24u);

	error = ETIMEDOUT;
	for (unsigned i = 0; i < 10000; i++) {
		status = rd(R_RINTSTS);
		if ((status & INT_ERR) || ((status & INT_CD) && (rd(R_RESP0) & SD_R1_ERRORS))) {
			error = EIO;
			break;
		}
		while (sent < 64 && !(rd(R_STATUS) & STATUS_FIFO_FULL)) {
			*(volatile uint64_t *)(sd + R_FIFO) = data[sent++];
		}
		if (status & INT_TXDR) {
			wr(R_RINTSTS, INT_TXDR);
		}
		if ((status & (INT_CD | INT_DTO)) == (INT_CD | INT_DTO)) {
			error = sent == 64 ? sd_wait_not_busy() : EIO;
			break;
		}
		delay(10);
	}
	sd_last_int = status;
	wr(R_RINTSTS, UINT32_MAX);
	if (error != 0) {
		sd_errors++;
	}
	return error;
}

static int sd_probe(void);

static int
sd_copen(dev_t dev, int flags, __unused int devtype, __unused struct proc *p)
{
	/* UVFS opens the partition O_RDWR before querying DKIOCISWRITABLE. */
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
sd_cwrite(dev_t dev, struct uio *uio, __unused int ioflag)
{
	/* Samsung disables the unconnected write-protect pin for dwmmc2. */
	if (minor(dev) != 1 || !sd_write_enabled) {
		return EROFS;
	}
	if (!sd_ready) {
		return ENXIO;
	}
	off_t offset = uio_offset(uio);
	user_ssize_t count = uio_resid(uio);
	uint64_t size = sd_part_blocks * 512;
	if (offset < 0 || offset % 512 != 0 || count < 0 || count % 512 != 0 ||
	    (uint64_t)offset > size || (uint64_t)count > size - (uint64_t)offset) {
		return EINVAL;
	}
	if (count == 0) {
		return 0;
	}

	uio_t input = uio_duplicate(uio);
	if (input == NULL) {
		return ENOMEM;
	}
	uint64_t data[64];
	int error = 0;
	while (uio_resid(input) > 0) {
		uint64_t lba = sd_part_start + (uint64_t)uio_offset(input) / 512;
		error = uiomove((caddr_t)data, sizeof(data), input);
		if (error != 0) {
			break;
		}
		lck_mtx_lock(&sd_lock);
		error = sd_write_sector(lba, data);
		lck_mtx_unlock(&sd_lock);
		if (error != 0) {
			break;
		}
		uio_update(uio, sizeof(data));
	}
	uio_free(input);
	return error;
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
	case DKIOCGETMAXBLOCKCOUNTWRITE:
		*(uint64_t *)data = SD_MAX_BLOCKS;
		return 0;
	case DKIOCSYNCHRONIZECACHE: {
		lck_mtx_lock(&sd_lock);
		int error = sd_write_enabled ? sd_wait_not_busy() : 0;
		lck_mtx_unlock(&sd_lock);
		return error;
	}
	case DKIOCISWRITABLE:
		*(uint32_t *)data = minor(dev) == 1 && sd_write_enabled;
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

static char sd_state[160] = "off (boot-arg sd=0)";
SYSCTL_DECL(_kern_redwood);
SYSCTL_STRING(_kern_redwood, OID_AUTO, sdcard, CTLFLAG_RD | CTLFLAG_LOCKED, sd_state, 0, "the microSD slot");
SYSCTL_UINT(_kern_redwood, OID_AUTO, sdcard_reads, CTLFLAG_RD | CTLFLAG_LOCKED, &sd_reads, 0, "reads done");
SYSCTL_UINT(_kern_redwood, OID_AUTO, sdcard_errors, CTLFLAG_RD | CTLFLAG_LOCKED, &sd_errors, 0, "reads failed");
SYSCTL_STRING(_kern_redwood, OID_AUTO, sdcard_dbg, CTLFLAG_RD | CTLFLAG_LOCKED, sd_dbg, 0, "the last transfer, in detail");
SYSCTL_UINT(_kern_redwood, OID_AUTO, sdcard_last_int, CTLFLAG_RD | CTLFLAG_LOCKED, &sd_last_int, 0, "last RINTSTS");

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
	snprintf(sd_state, sizeof(sd_state), "ready: %llu MB (to the last partition), DW MMC %#x, 4-bit, S-Boot's setup",
	    sd_blocks / 2048, sd_verid);
	printf("sdcard: %s\n", sd_state);
	return 0;
}

extern int redwood_sdmedia_publish(unsigned unit, int major, uint64_t whole_blocks, uint64_t part_start, uint64_t part_blocks,
    unsigned part_type, int writable);

/*
 * At boot (2026-11-21): the card read once IOKit and devfs are up, then /dev/disk9 + rdisk9 (the card) and disk9s1 +
 * rdisk9s1 (its first partition) and the IOKit disk userfsd matches (iokit/Kernel/RedwoodSDMedia.cpp). Character
 * devices. The file-system service requests a descriptor from Apple's privileged userfs_helper.
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
	error = redwood_sdmedia_publish(SD_UNIT, sd_cmaj, sd_blocks, sd_part_start, sd_part_blocks, sd_part_type, sd_write_enabled);
	snprintf(sd_state, sizeof(sd_state), "ready: disk%d %llu MB, disk%ds1 type %#x at %llu, %llu MB (%s); IOKit %s",
	    SD_UNIT, sd_blocks / 2048, SD_UNIT, sd_part_type, sd_part_start, sd_part_blocks / 2048,
	    sd_write_enabled ? "read-write" : "read-only", error == 0 ? "published" : "FAILED");
	printf("sdcard: %s\n", sd_state);
}

void redwood_sdcard_init(void);
void
redwood_sdcard_init(void)
{
	int cmaj;
	thread_t t;

	(void)PE_parse_boot_argn("sd", &sd_mode, sizeof(sd_mode));
	(void)PE_parse_boot_argn("sd_write", &sd_write_enabled, sizeof(sd_write_enabled));
	if (sd_mode == 0) {
		return;
	}
	sd = (volatile uint8_t *)ml_io_map(SD_PHYS, 0x1000);
	if (sd == NULL) {
		snprintf(sd_state, sizeof(sd_state), "no mapping");
		return;
	}
	sd_verid = rd(R_VERID);
	if ((sd_verid & 0xffff0000) != 0x53420000 || rd(R_PWREN) != 1 || rd(R_CLKENA) != 1) {
		snprintf(sd_state, sizeof(sd_state), "not as S-Boot leaves it: VERID %#x PWREN %u CLKENA %u", sd_verid,
		    rd(R_PWREN), rd(R_CLKENA));
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
	snprintf(sd_state, sizeof(sd_state), "slot found (DW MMC %#x); the card is read on the first open of /dev/rsdcard", sd_verid);
	printf("sdcard: %s\n", sd_state);
}
