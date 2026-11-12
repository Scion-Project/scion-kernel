// SPDX-License-Identifier: MIT-0
/*
 * The cellular modem of the Galaxy S9 (starlte, SM-G960F): the Samsung "Shannon" CP that is part of the Exynos 9810
 * itself (Samsung's driver calls it ss360ap). Only with boot-arg cp=N on this phone; with cp absent or 0 nothing
 * below runs: no register is read, no memory mapped, no call made to the secure world.
 *
 * WHAT THE CP IS, AS THE AP SEES IT (the phone's device tree and Samsung's kernel, read as data)
 *   its memory     146 MB of the phone's DRAM at 0xF0000000 (the device tree's reserved "modem_if"), outside XNU's
 *                  RAM. The CP sees it at 0x40000000. The image's own table (below) divides it:
 *                    0x0000000  the CP's code and data, 121 MB (BOOT at 0, MAIN at 0x10000, NV at 0x4900000)
 *                    0x7900000  VSS, the voice processor's, 7 MB
 *                    0x8000000  IPC, 5 MB: the memory both sides talk through
 *                    0x8500000  ZMC, 11 MB: downlink packets' buffers (not used here)
 *                    0x9000000  VPA, 2 MB
 *   its image      the phone's RADIO partition, read only, used exactly as it is. It starts with a table of
 *                  contents of 32-byte entries (name[12], offset in the partition, address for the CP, size, CRC-32,
 *                  number): TOC, BOOT, MAIN, VSS, NV, OFFSET. An entry goes to (its address - BOOT's address) in the
 *                  CP's memory. NV has no bytes in RADIO: it is the place of the phone's own non-volatile data
 *                  (Android's boot daemon copies /efs/nv_data.bin there). This driver never reads or writes the EFS
 *                  partition: with cp_nv=1 it takes a file "nv_data.bin" that the boot image's builder put beside
 *                  the kernel (hakios_image_file("fw", ...)), and without it the place is filled with zeros.
 *   the secure     the image is checked and the CP's memory closed by Samsung's EL3 monitor, not by us:
 *   world          SMC 0x82000700 with x1 = the mode (2: open the memory for loading; 0, or'ed with the memory's
 *                  address when the image carries its memory table: check BOOT and MAIN and lock), x2 = BOOT's size,
 *                  x3 = MAIN's size, each between SMC 0x82001011 x1 = 0 and x1 = 1 (the hash engine's clock on and
 *                  off). Its answer is 0 or one of Samsung's error numbers (smc_error below). If it refuses, this
 *                  driver stops: nothing is patched, skipped or retried another way.
 *   the PMU        0x14060000. CP_CTRL_NS (0x30) and CP_CTRL_S (0x34) belong to the secure world and are read and
 *                  written through the same SMC (x1 = 3 read, with x2 = 0 or 1 for the low and high half, x3 = 0 for
 *                  _S and 1 for _NS; x1 = 4 write, x2 = the value). CP_STAT 0x38, CENTRAL_SEQ_CP_CONFIGURATION 0x280
 *                  and _STATUS 0x284 (bits 23:16: 0x80 while the CP is held in reset), RESET_SEQUENCER_STATUS 0x504
 *                  (bits 2:0: 0 before the CP has ever been powered, 5 when it has), the CP's power-down
 *                  configuration 0x1320-0x1338, CLEANY_BUS_CP 0x3920/0x3924.
 *   AP2CP_CFG      0x14820000 bit 0: EL3 prepares the CP's view of the memory at cold boot; writing 1 here lets the
 *                  CP's processor run.
 *   the mailbox    0x14170000. INTGR1 (0x1C) bit n raises interrupt n at the CP; INTSR0 (0x14) bits 31:16 are the
 *                  CP's interrupts to the AP, cleared through INTCR0 (0x0C) and masked by INTMR0 (0x10); 64 shared
 *                  words ISSRn at 0x80 + 4n. This phone's use: word 0 the AP's message, word 1 the CP's, word 2 the
 *                  AP's state (bit 0 wake lock, 4:1 state, 5 "PDA active", 13:6 the board's revision, 15:14 dual
 *                  SIM), word 3 the CP's state (4:1 state, 5 "LTE active"). Interrupt 0 each way says "word 0/1 has
 *                  a message"; the AP's 1 is "wake up". Read by polling here (the interrupts stay masked).
 *   a message      bit 7 valid; with bit 6 a command in bits 5:0 (1 INIT_START, 2 INIT_END, 8 PHONE_START,
 *                  0xD PIF_INIT_DONE, 7 CRASH_RESET, 9 CRASH_EXIT); without it bit 0 "there is data in the rings".
 *   the IPC        its first words are a magic number and an "access" word, then the old two queues' pointers; the
 *   memory         boot loader of the CP talks through the old "raw" queue (to the CP at 12288, 2084864 bytes;
 *                  from it at 2097152, 2097152 bytes; head and tail words at 24/28 and 32/36) with the magic "BOOT"
 *                  (0x424F4F54). The running CP talks through rings of buffers (Samsung's "SBD"): at 2048 a table
 *                  the AP writes (version, channels, per channel and direction the ring's length, buffer size and
 *                  where its arrays of offsets and sizes are), the rings' read and write counters after it, the
 *                  buffers from 64 KB up; magic 0xAA and access 1 once the link is up.
 *   a frame        Samsung's SIPC5: a byte 0xF8 (bit 2: padded), the channel, the length (16 bits, header included),
 *                  then the message. Channels: 215 the boot loader, 235 the phone's control messages ("FMT"),
 *                  245 the CP's file requests to the AP ("RFS").
 *
 * THE STAGES (cp=N)
 *   1  a look, nothing changed: the PMU's and the mailbox's registers, the two secure words (the SMC's read),
 *      the first words of the CP's memory as the boot loader left it, RADIO's table of contents.
 *   2  that, and the image loaded and checked: the CP's power domain started if it never was (CP_CTRL_NS bit 1,
 *      CP_CTRL_S bit 3), the memory opened (mode 2), BOOT, MAIN and VSS copied from RADIO and compared with the
 *      table's CRCs, the NV place filled (zeros, or the builder's file with cp_nv=1), then the secure world's
 *      check (mode 0). Its answer is logged. The CP is not started.
 *   3  that, and the CP started: the mailbox cleared, the AP's state word set, AP2CP_CFG written, the CP's state
 *      waited for, the boot loader's two questions (0x900D, answer 0xA00D; 0x9F00, answer 0xAF00), then the
 *      running CP's INIT_START, the ring table written, PIF_INIT_DONE, and its PHONE_START: "THE CP IS UP".
 *      It is watched for cp_wait seconds (20) and then put back in reset.
 *   4  that, and the link, ONLY with the phone's own NV data loaded (cp_nv=1): the one command this driver ever
 *      sends that changes anything, "phone state: low power mode" (Samsung IPC's power group 0x01, number 0x07,
 *      "execute", the two bytes 01 00: what Android sends for airplane mode), is put in the ring first, then
 *      INIT_END is said (and said again on every PHONE_START until the CP stops repeating it: it looks at the one
 *      message word about every 100 ms, and anything written there sooner replaces INIT_END). The running
 *      CP otherwise goes to its NORMAL state by itself, receiver on, looking for a network (seen on 12 Nov 2026 with
 *      zeros for NV; never again: with zeros or without cp_nv=1 this stage is refused and the run stays at stage
 *      3). The CP is stopped at once if its next "phone state" is not low power, if none comes in 5 s, or if a
 *      registration state other than "none" is ever reported. Everything the CP sends is logged (the channel, the
 *      message's group, number and type, its length; the phone state, the SIM status bytes, the registration state;
 *      a file request's kind, name, offset and length), one harmless question is asked (the modem's software
 *      version) and its answer logged. The CP's file requests are NOT answered and nothing it wants stored is
 *      kept. After cp_wait seconds (25) the CP is put back in reset.
 *
 *   5  that, and, ONLY when the phone's owner has asked for it, the radio: with the CP confirmed in low power mode
 *      and the SIM's status "ready" (0x82 or 0x83; a locked SIM ends the run, no PIN is ever sent), "phone state:
 *      normal" is asked for (the bytes 02 02: what Android sends when airplane mode is switched off). The phone
 *      then registers on its network as it does under Android. Logged: the phone state, every registration state,
 *      the serving network's operator code, the signal words. Two minutes after it has registered (or 90 s after
 *      the request if it has not) low power mode is asked for again, confirmed, and the CP put back in reset.
 *      No call, message, data session, network or band selection is ever asked for.
 *
 * NEVER DONE HERE: no partition is written; EFS is not read; no request that sets an identity, a band, a power or
 * a calibration value, no test or factory mode, no PIN; the radio is on only in stage 5. An answer that could carry
 * an identity is logged as its length only.
 *
 * kern.hakios.cp.state reads where it got to (the stage reached, 100 + a stage that failed, 200 stopped);
 * kern.hakios.cp.go: 1 the state printed, 2 the CP stopped (reset asserted), 3 the version asked again.
 *
 * THE ARGUMENTS
 *   cp=N         the first N of the Galaxy S9's modem's five stages (1 a look, 2 its image loaded and checked, 3 started, 4 the link, 5 the network); 0 or none: left alone
 *   cp_nv=1      the phone's own NV data, a file nv_data.bin the image's builder put beside the kernel, is given to the modem (none: zeros, and no stage 4)
 *   cp_vss=0     the image's VSS part (the voice processor's) is not copied
 *   cp_rev=N     the board's revision told to the modem (26, this phone's)
 *   cp_ds=N      the dual SIM field told to the modem (0)
 *   cp_wait=N    seconds the started modem is watched before it is put back in reset (20; 25 with the link)
 *   cp_delay=N   seconds after the boot before the modem's driver does anything (30), so that its lines are in the log
 *
 * WHERE IT MAY RUN. The CP's memory is fixed by the boot chain at 0xF0000000. On a tree whose loader puts XNU itself
 * there (the 16.5 line's UEFI does: devices/starlte/docs/modem-plan.md, 1.5) this driver must not load anything: it
 * checks that no page of the 146 MB is XNU's and that the kernel is not in it, and does nothing if either fails.
 */
#include <kern/clock.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_time.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/param.h>
#include <sys/hakios.input.h>
#include <sys/hakios.log.h>
#include <sys/hakios.physmap.h>
#include <sys/sysctl.h>
#include <sys/systm.h>
#include <sys/hakios.xnu.h>

/* ---- addresses ---- */

#define CP_MEM_PA               0xF0000000ULL   /* the device tree's "modem_if" */
#define CP_MEM_BYTES            0x09200000ULL
#define CP_IPC_OFFSET           0x08000000u     /* the device tree's; the image's table is checked against it */
#define CP_IPC_BYTES            0x00500000u
#define CP_PMU_PA               0x14060000UL
#define CP_PMU_BYTES            0x4000
#define CP_MBOX_PA              0x14170000UL
#define CP_CFG_PA               0x14820000UL    /* AP2CP_CFG */
#define CP_GICD_PA              0x10100000UL    /* the interrupt controller: one "pending" word read, nothing else */

#define PMU_CP_CTRL_NS          0x0030          /* through the SMC */
#define PMU_CP_CTRL_S           0x0034          /* through the SMC */
#define PMU_CP_STAT             0x0038
#define PMU_CENTRAL_SEQ_CP_CFG  0x0280
#define PMU_CENTRAL_SEQ_CP_STAT 0x0284
#define PMU_RESET_SEQ_STAT      0x0504
#define PMU_CP_PWR_FIRST        0x1320          /* RESET_AHEAD, CLEANY_BUS, LOGIC_RESET, TCXO_GATE, DISABLE_ISO, RESET_ISO, DUMP_PC */
#define PMU_CLEANY_BUS_CP_CFG   0x3920
#define PMU_CLEANY_BUS_CP_STAT  0x3924

#define NS_PWRON                (1u << 1)
#define NS_RESET_SET            (1u << 2)
#define NS_DUMP_PC_NO_PG        (1u << 17)
#define S_START                 (1u << 3)

#define MBOX_MCUCTLR            0x00
#define MBOX_INTGR0             0x08
#define MBOX_INTCR0             0x0c
#define MBOX_INTMR0             0x10
#define MBOX_INTSR0             0x14
#define MBOX_INTMSR0            0x18
#define MBOX_INTGR1             0x1c
#define MBOX_INTCR1             0x20
#define MBOX_INTMR1             0x24
#define MBOX_INTSR1             0x28
#define MBOX_ISSR(n)            (0x80 + 4 * (n))
#define MBOX_WORDS              64

/* this phone's use of the mailbox (the device tree's mif_pdata) */
#define WORD_AP_MSG             0
#define WORD_CP_MSG             1
#define WORD_AP_STATE           2
#define WORD_CP_STATE           3
#define INT_AP_MSG              0
#define INT_AP_WAKEUP           1
#define IRQ_CP_MSG              0
#define AP_STATE_STATUS(v)      (((v) & 0xfu) << 1)
#define AP_STATE_PDA_ACTIVE     (1u << 5)
#define AP_STATE_REV(v)         (((v) & 0xffu) << 6)
#define AP_STATE_DS(v)          (((v) & 0x3u) << 14)
#define CP_STATE_STATUS(w)      (((w) >> 1) & 0xfu)
#define CP_STATE_ACTIVE(w)      (((w) >> 5) & 1u)

#define MSG_VALID               0x80
#define MSG_CMD                 0x40
#define MSG_DATA                0x01
#define CMD_INIT_START          0x01
#define CMD_INIT_END            0x02
#define CMD_CRASH_RESET         0x07
#define CMD_PHONE_START         0x08
#define CMD_CRASH_EXIT          0x09
#define CMD_PIF_INIT_DONE       0x0d

#define SMC_CP                  0x82000700u
#define SMC_CP_READ             3
#define SMC_CP_WRITE            4
#define SMC_CP_REG_S            0
#define SMC_CP_REG_NS           1
#define SMC_SSS_CLK             0x82001011u

/* the IPC memory */
#define IPC_MAGIC               0
#define IPC_ACCESS              4
#define IPC_RAW_TX_HEAD         24
#define IPC_RAW_TX_TAIL         28
#define IPC_RAW_RX_HEAD         32
#define IPC_RAW_RX_TAIL         36
#define IPC_RAW_TX_BUF          12288u
#define IPC_RAW_TX_BYTES        2084864u
#define IPC_RAW_RX_BUF          2097152u
#define IPC_RAW_RX_BYTES        2097152u
#define MAGIC_BOOT              0x424F4F54u
#define MAGIC_IPC               0xAAu

#define SBD_DESC                2048u           /* the table, in the first 64 KB */
#define SBD_BUFFERS             65536u
#define SBD_MAX_CHANNELS        32
#define SBD_RB_CH               12u             /* u32 version, channels, counters' offset; then rb_ch[32] of 16 bytes */
#define SBD_RB_DESC             (SBD_RB_CH + SBD_MAX_CHANNELS * 16u)   /* rb_desc[32][2] of 20 bytes */
#define SBD_GLOBAL_BYTES        (SBD_RB_DESC + SBD_MAX_CHANNELS * 2u * 20u)

#define CH_BOOT                 215
#define CH_FMT                  235
#define CH_RFS                  245

/* the channels of this phone's device tree that get a ring, in its order (the ones for other regions left out) */
static const struct ring_cfg {
	uint16_t ch;
	uint16_t ul_len, ul_size, dl_len, dl_size;
	const char *name;
} ring_cfgs[] = {
	{ 35, 16, 2048, 16, 2048, "cass" },
	{ CH_FMT, 16, 4096, 32, 4096, "ipc0" },
	{ 236, 16, 4096, 32, 4096, "ipc1" },
	{ CH_RFS, 16, 2048, 512, 2048, "rfs0" },
	{ 1, 32, 2048, 64, 2048, "csd" },
	{ 25, 16, 2048, 16, 2048, "router" },
	{ 28, 16, 2048, 128, 2048, "dm0" },
	{ 10, 256, 2048, 256, 2048, "data, high priority" },
	{ 11, 512, 2048, 2048, 8, "data" },     /* its downlink ring holds addresses of ZMC buffers: none given here */
};
#define NRINGS                  (sizeof(ring_cfgs) / sizeof(ring_cfgs[0]))

struct ring {
	uint16_t len, size;
	uint32_t addr_v, size_v;        /* offsets of the arrays in the IPC memory */
	uint32_t buffers;               /* offset of the first buffer */
	uint32_t rp, wp;                /* offsets of the 16-bit counters */
};

struct toc_entry {
	char name[12];
	uint32_t offset, address, size, crc, number;
};

static struct {
	uint32_t stage;                 /* cp=N */
	volatile uint32_t state;
	vm_offset_t pmu, mbox, cfg, gicd, mem;
	uint8_t *ipc;
	struct toc_entry toc[16];
	uint32_t ntoc;
	const struct toc_entry *boot, *main, *vss, *nv;
	bool mem_table;                 /* the image carries its memory table: the check gets the memory's address */
	bool started, up, link;
	bool nv_loaded;                 /* the phone's own NV data is in the CP's memory (not zeros) */
	bool stop;                      /* something seen that this driver stops the CP for */
	bool link_acked;                /* the CP has taken INIT_END: it no longer repeats PHONE_START */
	bool version_answered, bands_asked;
	/* stage 5 */
	int sim_status;                 /* the SIM's last status byte, -1 none yet */
	int reg_state;                  /* the last registration state reported, -1 none yet */
	bool normal_asked, back_asked;  /* "normal" was asked for; then "low power" again */
	uint64_t normal_ms, registered_ms, back_ms, sim_ms;
	uint32_t reg_changes, signals, writes_dropped;
	uint64_t init_end_ms, phone_start_ms;
	uint64_t lpm_sent_ms;           /* when "low power mode" was asked */
	int phone_state;                /* the CP's last "phone state" notification: 1 low power, 2 normal; -1 none yet */
	struct ring ul[NRINGS], dl[NRINGS];
	uint8_t mseq;
	uint32_t frames[NRINGS], rfs_requests, fmt_messages, phone_starts;
	uint32_t rev, ds, wait_s, delay_s, nv_file, vss_on;
	uint32_t more[NRINGS];          /* bytes still to come of a frame longer than one buffer */
	bool multi[NRINGS];             /* the frames now coming on a ring are the rest of a message already seen */
	uint32_t rfs_more;              /* bytes of a file request still to come in later frames */
	uint32_t dropped[NRINGS];       /* such frames dropped */
} cp;

static int cp_go;
static int cp_state_out;

SYSCTL_DECL(_kern_hakios);
SYSCTL_NODE(_kern_hakios, OID_AUTO, cp, CTLFLAG_RW | CTLFLAG_LOCKED, 0, "the Galaxy S9's cellular modem (hakios.shannon.c, cp=N)");
SYSCTL_INT(_kern_hakios_cp, OID_AUTO, state, CTLFLAG_RD | CTLFLAG_KERN | CTLFLAG_LOCKED, &cp_state_out, 0, "the stage reached (1 looked, 2 loaded and checked, 3 the CP up, 4 the link up), 100 + the stage that failed, 200 stopped");
SYSCTL_INT(_kern_hakios_cp, OID_AUTO, go, CTLFLAG_RW | CTLFLAG_KERN | CTLFLAG_LOCKED, &cp_go, 0, "set: 1 the state printed, 2 the CP stopped, 3 the version asked");

/* ---- small things ---- */

static inline uint32_t
rd(vm_offset_t base, uint32_t off)
{
	return *(volatile uint32_t *)(base + off);
}

static inline void
wr(vm_offset_t base, uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(base + off) = v;
}

static inline uint32_t
ipc_rd32(uint32_t off)
{
	return *(volatile uint32_t *)(void *)(cp.ipc + off);
}

static inline void
ipc_wr32(uint32_t off, uint32_t v)
{
	*(volatile uint32_t *)(void *)(cp.ipc + off) = v;
}

static inline uint16_t
ipc_rd16(uint32_t off)
{
	return *(volatile uint16_t *)(void *)(cp.ipc + off);
}

static inline void
ipc_wr16(uint32_t off, uint16_t v)
{
	*(volatile uint16_t *)(void *)(cp.ipc + off) = v;
}

static void
set_state(uint32_t s)
{
	cp.state = s;
	cp_state_out = (int)s;
}

static uint64_t
now_ms(void)
{
	uint64_t ns;

	absolutetime_to_nanoseconds(mach_absolute_time(), &ns);
	return ns / NSEC_PER_MSEC;
}

static uint64_t
smc(uint64_t fid, uint64_t a1, uint64_t a2, uint64_t a3)
{
	register uint64_t x0 __asm__("x0") = fid;
	register uint64_t x1 __asm__("x1") = a1;
	register uint64_t x2 __asm__("x2") = a2;
	register uint64_t x3 __asm__("x3") = a3;

	__asm__ volatile ("smc #0" : "+r" (x0), "+r" (x1), "+r" (x2), "+r" (x3) : :
	    "x4", "x5", "x6", "x7", "x8", "x9", "x10", "x11", "x12", "x13", "x14", "x15", "x16", "x17", "memory");
	return x0;
}

static const char *
smc_error(uint32_t e)
{
	static const char *const names[] = {
		"no error", "not aligned to 64 KB", "memory too big", "flag out of range", "wrong TZASC region number",
		"wrong boot loader size", "memory out of range", "not aligned to 16 bytes", "memory in secure DRAM",
		"ASP enable failed", "ASP disable failed", "CP not working", "CP already working", "CP already in dump mode",
		"not a valid magic", "CP should be disabled", "CP memory already enabled", "window already set",
		"failed to set the window", "invalid CP base", "corrupted CP memory information",
	};

	return e < sizeof(names) / sizeof(names[0]) ? names[e] : "not one of the listed errors";
}

/* one of the two secure words: false if the secure world would not say */
static bool
secure_read(uint32_t which, uint32_t *value)
{
	uint32_t lo = (uint32_t)smc(SMC_CP, SMC_CP_READ, 0, which);
	uint32_t hi;

	if (lo & 0xffff) {
		say("cp: the secure world would not read CP_CTRL_%s (low half): %#x\n", which == SMC_CP_REG_S ? "S" : "NS", lo);
		return false;
	}
	hi = (uint32_t)smc(SMC_CP, SMC_CP_READ, 1, which);
	if (hi & 0xffff) {
		say("cp: the secure world would not read CP_CTRL_%s (high half): %#x\n", which == SMC_CP_REG_S ? "S" : "NS", hi);
		return false;
	}
	*value = (hi & 0xffff0000u) | (lo >> 16);
	return true;
}

/* bits set or cleared in one of the two secure words, as Samsung's PMU sequences do it: read, change, write, read */
static bool
secure_update(uint32_t which, uint32_t clear, uint32_t set, const char *why)
{
	uint32_t before, after = 0;
	uint32_t answer;

	if (!secure_read(which, &before)) {
		return false;
	}
	answer = (uint32_t)smc(SMC_CP, SMC_CP_WRITE, (before & ~clear) | set, which);
	(void)secure_read(which, &after);
	say("cp: CP_CTRL_%s %#x -> %#x (%s), the secure world answered %#x\n", which == SMC_CP_REG_S ? "S" : "NS", before,
	    after, why, answer);
	return answer == 0;
}

static bool
pmu_wait(uint32_t off, uint32_t mask, uint32_t want, uint32_t ms, const char *name)
{
	uint64_t until = now_ms() + ms;
	uint32_t v;

	do {
		v = rd(cp.pmu, off);
		if ((v & mask) == want) {
			return true;
		}
		delay(100);
	} while (now_ms() < until);
	say("cp: %s stayed %#x (wanted %#x under %#x) for %u ms\n", name, v, want, mask, ms);
	return false;
}

/* the CP's own interrupt to the AP's interrupt controller, as Samsung's sequences wait for it: only looked at */
static bool
cp_irq_pending(void)
{
	return cp.gicd != 0 && (rd(cp.gicd, 0x238) & (1u << 28)) != 0;       /* GICD_ISPENDR14 (0x1238), bit 28 */
}

static void
mbox_raise(uint32_t n)
{
	wr(cp.mbox, MBOX_INTGR1, 1u << n);
}

static void
send_msg(uint16_t value)
{
	/* one word carries every message: the last one must have been taken (its interrupt no longer pending at the CP)
	 * before the next is written over it. Seen: a "data" message written 1 ms after INIT_END replaced it, and the CP
	 * never knew the link was up. 50 ms at most. */
	for (uint32_t i = 0; i < 200 && (rd(cp.mbox, MBOX_INTSR1) & (1u << INT_AP_MSG)) != 0; i++) {
		delay(250);
	}
	wr(cp.mbox, MBOX_ISSR(WORD_AP_MSG), value);
	mbox_raise(INT_AP_MSG);
}

static uint32_t
crc32_of(const uint8_t *p, uint32_t n)
{
	static const uint32_t nibble[16] = {
		0x00000000, 0x1db71064, 0x3b6e20c8, 0x26d930ac, 0x76dc4190, 0x6b6b51f4, 0x4db26158, 0x5005713c,
		0xedb88320, 0xf00f9344, 0xd6d6a3e8, 0xcb61b38c, 0x9b64c2b0, 0x86d3d2d4, 0xa00ae278, 0xbdbdf21c,
	};
	uint32_t c = 0xffffffffu;

	while (n-- != 0) {
		c ^= *p++;
		c = nibble[c & 15] ^ (c >> 4);
		c = nibble[c & 15] ^ (c >> 4);
	}
	return ~c;
}

/* the CP's memory must be nobody's: not XNU's pages, not where the kernel itself was loaded */
extern boolean_t pmap_valid_page(ppnum_t pn);

static bool
memory_is_free(void)
{
	uint64_t here[2] = { kvtophys((vm_offset_t)&cp), kvtophys((vm_offset_t)&memory_is_free) };

	for (uint32_t i = 0; i < 2; i++) {
		if (here[i] >= CP_MEM_PA && here[i] < CP_MEM_PA + CP_MEM_BYTES) {
			say("cp: this kernel itself is loaded in the CP's memory (%#llx): the modem cannot be used on this boot path\n",
			    here[i]);
			return false;
		}
	}
	for (uint64_t pa = CP_MEM_PA; pa < CP_MEM_PA + CP_MEM_BYTES; pa += PAGE_SIZE) {
		if (pmap_valid_page((ppnum_t)(pa >> PAGE_SHIFT))) {
			say("cp: %#llx of the CP's memory is one of XNU's pages: the modem cannot be used with this memory layout\n", pa);
			return false;
		}
	}
	return true;
}

/* ---- stage 1: the look ---- */

static void
look_pmu(const char *when)
{
	uint32_t ns = 0, s = 0;
	bool have_ns = secure_read(SMC_CP_REG_NS, &ns), have_s = secure_read(SMC_CP_REG_S, &s);

	say("cp: PMU %s: CP_STAT %#x, CENTRAL_SEQ_CP config %#x status %#x, RESET_SEQUENCER_STATUS %#x, CLEANY_BUS_CP "
	    "config %#x status %#x\n", when, rd(cp.pmu, PMU_CP_STAT), rd(cp.pmu, PMU_CENTRAL_SEQ_CP_CFG),
	    rd(cp.pmu, PMU_CENTRAL_SEQ_CP_STAT), rd(cp.pmu, PMU_RESET_SEQ_STAT), rd(cp.pmu, PMU_CLEANY_BUS_CP_CFG),
	    rd(cp.pmu, PMU_CLEANY_BUS_CP_STAT));
	say("cp: PMU %s: the CP's power-down words 0x1320-0x1338: %#x %#x %#x %#x %#x %#x %#x\n", when,
	    rd(cp.pmu, 0x1320), rd(cp.pmu, 0x1324), rd(cp.pmu, 0x1328), rd(cp.pmu, 0x132c), rd(cp.pmu, 0x1330),
	    rd(cp.pmu, 0x1334), rd(cp.pmu, 0x1338));
	if (have_ns && have_s) {
		say("cp: PMU %s: the secure words: CP_CTRL_NS %#x, CP_CTRL_S %#x; the CP's interrupt %s\n", when, ns, s,
		    cp_irq_pending() ? "pending" : "not pending");
	}
	/* AP2CP_CFG is NOT read here: it is in the CP's own power domain, and reading it before that domain was ever
	 * started, while the CP is held in reset, or while the running CP has put itself to sleep is a bus error (seen
	 * three times: an SError panic). It is touched once, at the start, as Samsung's driver does. */
}

static void
look_mbox(const char *when)
{
	say("cp: mailbox %s: MCUCTLR %#x, to the AP: mask %#x status %#x, to the CP: mask %#x status %#x\n", when,
	    rd(cp.mbox, MBOX_MCUCTLR), rd(cp.mbox, MBOX_INTMR0), rd(cp.mbox, MBOX_INTSR0), rd(cp.mbox, MBOX_INTMR1),
	    rd(cp.mbox, MBOX_INTSR1));
	say("cp: mailbox %s: words 0-9: %#x %#x %#x %#x %#x %#x %#x %#x %#x %#x (CP's state %u, active %u)\n", when,
	    rd(cp.mbox, MBOX_ISSR(0)), rd(cp.mbox, MBOX_ISSR(1)), rd(cp.mbox, MBOX_ISSR(2)), rd(cp.mbox, MBOX_ISSR(3)),
	    rd(cp.mbox, MBOX_ISSR(4)), rd(cp.mbox, MBOX_ISSR(5)), rd(cp.mbox, MBOX_ISSR(6)), rd(cp.mbox, MBOX_ISSR(7)),
	    rd(cp.mbox, MBOX_ISSR(8)), rd(cp.mbox, MBOX_ISSR(9)), CP_STATE_STATUS(rd(cp.mbox, MBOX_ISSR(WORD_CP_STATE))),
	    CP_STATE_ACTIVE(rd(cp.mbox, MBOX_ISSR(WORD_CP_STATE))));
}

/* how much of a stretch of the CP's memory is not zero: a count, never the bytes */
static uint32_t
not_zero(uint32_t off, uint32_t bytes)
{
	const volatile uint32_t *p = (const volatile uint32_t *)(void *)((uint8_t *)cp.mem + off);
	uint32_t n = 0;

	for (uint32_t i = 0; i < bytes / 4; i++) {
		n += p[i] != 0;
	}
	return n * 4;
}

static void
look_mem(void)
{
	const volatile uint32_t *m = (const volatile uint32_t *)cp.mem;

	say("cp: the CP's memory as found: first words %#x %#x %#x %#x; at 0xA0 %#x, at 0x2A0 %#x (\"MEM\" would be an image "
	    "left there by the boot loader)\n", m[0], m[1], m[2], m[3], m[0xa0 / 4], m[0x2a0 / 4]);
	say("cp: the CP's memory as found: bytes not zero in the first 64 KB %u, in MAIN's first MB %u, in the NV place %u "
	    "of 1048576, in the IPC memory's first 64 KB %u; IPC magic %#x access %#x\n", not_zero(0, 0x10000),
	    not_zero(0x10000, 0x100000), not_zero(0x4900000, 0x100000), not_zero(CP_IPC_OFFSET, 0x10000),
	    ipc_rd32(IPC_MAGIC), ipc_rd32(IPC_ACCESS));
}

static const struct toc_entry *
toc_find(const char *name)
{
	for (uint32_t i = 0; i < cp.ntoc; i++) {
		if (strncmp(cp.toc[i].name, name, sizeof(cp.toc[i].name)) == 0) {
			return &cp.toc[i];
		}
	}
	return NULL;
}

static bool
read_toc(void)
{
	static uint8_t head[4096];
	uint64_t size = 0;
	int error = ENXIO;

	/* the storage may come up after this thread: two minutes for it */
	for (uint32_t waited = 0; waited < 240; waited++) {
		error = hakios_ufs_read_partition("RADIO", 0, NULL, 0, &size);
		if (error == 0) {
			break;
		}
		IOSleep(500);
	}
	if (error != 0 || size < sizeof(head)) {
		say("cp: no RADIO partition to read (error %d, %llu bytes)\n", error, size);
		return false;
	}
	error = hakios_ufs_read_partition("RADIO", 0, head, sizeof(head), NULL);
	if (error != 0) {
		say("cp: RADIO's first block could not be read (error %d)\n", error);
		return false;
	}
	cp.ntoc = 0;
	for (uint32_t i = 0; i < 16; i++) {
		struct toc_entry *e = &cp.toc[cp.ntoc];

		memcpy(e, head + 32 * i, sizeof(*e));
		if (e->name[0] == 0) {
			break;
		}
		e->name[sizeof(e->name) - 1] = 0;
		say("cp: RADIO (%llu bytes) entry %u: %s at %#x in the partition, for the CP at %#x, %u bytes, CRC %#x\n", size,
		    i, e->name, e->offset, e->address, e->size, e->crc);
		cp.ntoc++;
	}
	cp.boot = toc_find("BOOT");
	cp.main = toc_find("MAIN");
	cp.vss = toc_find("VSS");
	cp.nv = toc_find("NV");
	if (cp.ntoc == 0 || strcmp(cp.toc[0].name, "TOC") != 0 || cp.boot == NULL || cp.main == NULL ||
	    cp.boot->address != 0x40000000u || cp.main->address <= cp.boot->address ||
	    (uint64_t)(cp.main->address - cp.boot->address) + cp.main->size > CP_IPC_OFFSET ||
	    (uint64_t)cp.boot->offset + cp.boot->size > size || (uint64_t)cp.main->offset + cp.main->size > size ||
	    cp.boot->size > cp.main->address - cp.boot->address) {
		say("cp: RADIO's table of contents is not the one this driver knows: nothing is loaded\n");
		return false;
	}
	/* the image's own memory table, 0xA0 into BOOT: "MEMn", the CP's size, the rest's size, a count, then
	 * (tag, offset, size) entries. Samsung's kernel gives the secure world the memory's address when it is there. */
	{
		uint32_t at = cp.boot->offset + 0xa0;
		uint32_t tag = 0, count = 0;

		if (cp.boot->offset < sizeof(head) - 0xa0 - 16 - 12 * 5) {
			memcpy(&tag, head + at, 4);
			memcpy(&count, head + at + 12, 4);
		}
		cp.mem_table = (tag & 0xffffff00u) == 0x4d454d00u && count >= 1 && count <= 5;
		if (cp.mem_table) {
			for (uint32_t i = 0; i < count; i++) {
				uint32_t t, a, s;

				memcpy(&t, head + at + 16 + 12 * i, 4);
				memcpy(&a, head + at + 20 + 12 * i, 4);
				memcpy(&s, head + at + 24 + 12 * i, 4);
				say("cp: the image's memory table: %c%c%c at %#x, %#x bytes\n", (char)(t >> 16), (char)(t >> 8), (char)t,
				    a, s);
				if ((t & 0xffffff) == 0x495043 /* "IPC" */ && (a != CP_IPC_OFFSET || s != CP_IPC_BYTES)) {
					say("cp: the image puts the IPC memory somewhere this driver does not expect: nothing is loaded\n");
					return false;
				}
			}
		} else {
			say("cp: the image has no memory table: the device tree's layout is used\n");
		}
	}
	return true;
}

/* ---- stage 2: loaded and checked ---- */

static bool
cp_power_first(void)
{
	uint32_t seq = rd(cp.pmu, PMU_RESET_SEQ_STAT) & 7;

	if (seq != 0) {
		say("cp: the CP's power domain was started before us (RESET_SEQUENCER_STATUS %u): left as it is\n", seq);
		return true;
	}
	say("cp: the CP's power domain has never been started in this boot: starting it (Samsung's \"cp_init\")\n");
	if (!secure_update(SMC_CP_REG_NS, 0, NS_PWRON, "power on") || !secure_update(SMC_CP_REG_S, 0, S_START, "start")) {
		return false;
	}
	delay(1024);
	if (!pmu_wait(PMU_RESET_SEQ_STAT, 7, 5, 500, "RESET_SEQUENCER_STATUS")) {
		return false;
	}
	for (uint32_t i = 0; i < 500 && !cp_irq_pending(); i++) {
		IOSleep(1);
	}
	say("cp: the CP's power domain is up (RESET_SEQUENCER_STATUS %u), its interrupt %s\n",
	    rd(cp.pmu, PMU_RESET_SEQ_STAT) & 7, cp_irq_pending() ? "pending" : "not pending after 500 ms (gone on anyway)");
	return true;
}

static bool
copy_entry(const struct toc_entry *e)
{
	uint32_t at = e->address - cp.boot->address;
	uint8_t *dst = (uint8_t *)cp.mem + at;
	uint64_t t0 = now_ms();
	uint32_t crc;

	for (uint32_t done = 0; done < e->size;) {
		uint32_t n = MIN(e->size - done, 1u << 20);
		int error = hakios_ufs_read_partition("RADIO", (uint64_t)e->offset + done, dst + done, n, NULL);

		if (error != 0) {
			say("cp: %s: RADIO could not be read at %#x (error %d)\n", e->name, e->offset + done, error);
			return false;
		}
		done += n;
	}
	crc = crc32_of(dst, e->size);
	say("cp: %s: %u bytes of RADIO at %#x copied to the CP's memory at %#x in %llu ms; read back, their CRC is %#x, "
	    "the table's %#x: %s\n", e->name, e->size, e->offset, at, now_ms() - t0, crc, e->crc,
	    crc == e->crc ? "THE SAME" : "DIFFERENT");
	return crc == e->crc;
}

static bool
fill_nv(void)
{
	uint32_t at, size;
	uint8_t *dst;
	const uint8_t *file = NULL;
	uint64_t file_size = 0;

	if (cp.nv == NULL) {
		say("cp: the image names no NV place\n");
		return true;
	}
	at = cp.nv->address - cp.boot->address;
	size = cp.nv->size;
	if (cp.nv->address < cp.main->address + cp.main->size || (uint64_t)at + size > CP_IPC_OFFSET || size > (4u << 20)) {
		say("cp: the NV place (%#x, %u bytes) is not where this driver expects one: left alone\n", at, size);
		return false;
	}
	dst = (uint8_t *)cp.mem + at;
	if (cp.nv_file) {
		file = hakios_image_file("fw", "nv_data.bin", &file_size, NULL);
		if (file == NULL || file_size != size) {
			say("cp: cp_nv=1, but the boot image carries no nv_data.bin of %u bytes (found %llu): not going on\n", size,
			    file_size);
			return false;
		}
		memcpy(dst, file, size);
		cp.nv_loaded = true;
		say("cp: NV: the builder's nv_data.bin (%u bytes, never from the EFS partition by this kernel) copied to the "
		    "CP's memory at %#x\n", size, at);
	} else {
		bzero(dst, size);
		say("cp: NV: the place at %#x (%u bytes) filled with zeros: the CP gets no non-volatile data in this run\n", at,
		    size);
	}
	return true;
}

static bool
load_and_check(void)
{
	uint32_t answer;
	uint64_t mode;

	if (!cp_power_first()) {
		return false;
	}
	smc(SMC_SSS_CLK, 0, 0, 0);
	answer = (uint32_t)smc(SMC_CP, 2, 0, 0);
	smc(SMC_SSS_CLK, 1, 0, 0);
	say("cp: the secure world asked to open the CP's memory for loading (mode 2): it answered %#x (%s)\n", answer,
	    smc_error(answer));
	/* (Samsung's daemon goes on whatever this answers: "CP not working" is what a CP that never ran gives) */

	if (!copy_entry(cp.boot) || !copy_entry(cp.main)) {
		say("cp: the image in the CP's memory is not the image in RADIO: the secure world is not asked, nothing started\n");
		return false;
	}
	if (cp.vss != NULL && cp.vss_on) {
		if (cp.vss->address - cp.boot->address + (uint64_t)cp.vss->size > CP_IPC_OFFSET ||
		    cp.vss->address < cp.main->address + cp.main->size) {
			say("cp: VSS is not where this driver expects it: not copied\n");
		} else if (!copy_entry(cp.vss)) {
			return false;
		}
	}
	if (!fill_nv()) {
		return false;
	}

	mode = cp.mem_table ? CP_MEM_PA : 0;
	smc(SMC_SSS_CLK, 0, 0, 0);
	answer = (uint32_t)smc(SMC_CP, mode, cp.boot->size, cp.main->size);
	smc(SMC_SSS_CLK, 1, 0, 0);
	say("cp: THE SECURE WORLD'S CHECK of the image (mode 0, memory %#llx, BOOT %u bytes, MAIN %u bytes): it answered "
	    "%#x (%s)\n", mode, cp.boot->size, cp.main->size, answer, smc_error(answer));
	if (answer != 0) {
		say("cp: the secure world did not accept the image: stopping here, the CP is not started\n");
		return false;
	}
	return true;
}

/* ---- the CP back in reset (Samsung's "reset_assert") ---- */

static void
cp_stop(const char *why)
{
	if (!cp.started) {
		return;
	}
	say("cp: stopping the CP (%s)\n", why);
	mbox_raise(INT_AP_WAKEUP);
	IOSleep(5);
	(void)secure_update(SMC_CP_REG_NS, 0, NS_DUMP_PC_NO_PG, "keep its program counter readable");
	wr(cp.pmu, 0x1320, rd(cp.pmu, 0x1320) & ~3u);
	wr(cp.pmu, 0x1324, rd(cp.pmu, 0x1324) & ~1u);
	wr(cp.pmu, 0x1328, rd(cp.pmu, 0x1328) & ~3u);
	wr(cp.pmu, 0x132c, rd(cp.pmu, 0x132c) & ~1u);
	wr(cp.pmu, 0x1330, rd(cp.pmu, 0x1330) | 1u);
	wr(cp.pmu, 0x1334, rd(cp.pmu, 0x1334) & ~1u);
	wr(cp.pmu, 0x1338, rd(cp.pmu, 0x1338) & ~1u);
	(void)secure_update(SMC_CP_REG_NS, 0, NS_RESET_SET, "reset");
	wr(cp.pmu, PMU_CENTRAL_SEQ_CP_CFG, rd(cp.pmu, PMU_CENTRAL_SEQ_CP_CFG) & ~(1u << 16));
	delay(1024);
	if (pmu_wait(PMU_CENTRAL_SEQ_CP_STAT, 0xffu << 16, 0x80u << 16, 200, "CENTRAL_SEQ_CP_STATUS")) {
		say("cp: THE CP IS IN RESET (CENTRAL_SEQ_CP_STATUS %#x)\n", rd(cp.pmu, PMU_CENTRAL_SEQ_CP_STAT));
	}
	/* the mailbox's interrupts to the AP masked and cleared, nothing pending for whoever boots next */
	wr(cp.mbox, MBOX_INTMR0, 0xffff0000u);
	wr(cp.mbox, MBOX_INTCR0, 0xffff0000u);
	cp.started = cp.up = cp.link = cp.link_acked = false;
	look_pmu("after the stop");
	look_mbox("after the stop");
}

/* ---- stage 3: started ---- */

/* a boot loader frame on the old raw queue */
static bool
boot_send(uint32_t word)
{
	uint8_t frame[8] = { 0xfc, CH_BOOT, 8, 0, (uint8_t)word, (uint8_t)(word >> 8), (uint8_t)(word >> 16), (uint8_t)(word >> 24) };
	uint32_t head = ipc_rd32(IPC_RAW_TX_HEAD), tail = ipc_rd32(IPC_RAW_TX_TAIL);

	if (head >= IPC_RAW_TX_BYTES || tail >= IPC_RAW_TX_BYTES) {
		say("cp: the boot queue's pointers are out of range (%u, %u)\n", head, tail);
		return false;
	}
	for (uint32_t i = 0; i < sizeof(frame); i++) {
		cp.ipc[IPC_RAW_TX_BUF + (head + i) % IPC_RAW_TX_BYTES] = frame[i];
	}
	__builtin_arm_dsb(0xf);
	ipc_wr32(IPC_RAW_TX_HEAD, (head + sizeof(frame)) % IPC_RAW_TX_BYTES);
	return true;
}

static bool
boot_receive(uint32_t *word, uint32_t ms)
{
	uint64_t until = now_ms() + ms;

	do {
		uint32_t head = ipc_rd32(IPC_RAW_RX_HEAD), tail = ipc_rd32(IPC_RAW_RX_TAIL);
		uint32_t have;

		if (head < IPC_RAW_RX_BYTES && tail < IPC_RAW_RX_BYTES &&
		    (have = (head + IPC_RAW_RX_BYTES - tail) % IPC_RAW_RX_BYTES) >= 8) {
			uint8_t frame[8];

			for (uint32_t i = 0; i < 8; i++) {
				frame[i] = cp.ipc[IPC_RAW_RX_BUF + (tail + i) % IPC_RAW_RX_BYTES];
			}
			/* everything that was there is taken: the boot loader says one thing at a time */
			ipc_wr32(IPC_RAW_RX_TAIL, head);
			memcpy(word, frame + 4, 4);
			if ((frame[0] & 0xf8) != 0xf8 || frame[1] != CH_BOOT) {
				say("cp: the boot queue held %u bytes that are no boot frame: %02x %02x %02x %02x %02x %02x %02x %02x\n",
				    have, frame[0], frame[1], frame[2], frame[3], frame[4], frame[5], frame[6], frame[7]);
				return false;
			}
			return true;
		}
		IOSleep(1);
	} while (now_ms() < until);
	return false;
}

static bool
boot_ask(uint32_t question, uint32_t expected)
{
	uint32_t answer = 0;
	uint64_t t0 = now_ms();

	if (!boot_send(question)) {
		return false;
	}
	if (!boot_receive(&answer, 5000)) {
		say("cp: the boot loader was asked %#x and said nothing in 5 s (queue to it: head %u tail %u; from it: head %u "
		    "tail %u; mailbox status %#x, word 1 %#x, CP's state word %#x)\n", question, ipc_rd32(IPC_RAW_TX_HEAD),
		    ipc_rd32(IPC_RAW_TX_TAIL), ipc_rd32(IPC_RAW_RX_HEAD), ipc_rd32(IPC_RAW_RX_TAIL), rd(cp.mbox, MBOX_INTSR0),
		    rd(cp.mbox, MBOX_ISSR(WORD_CP_MSG)), rd(cp.mbox, MBOX_ISSR(WORD_CP_STATE)));
		return false;
	}
	say("cp: the boot loader was asked %#x and answered %#x after %llu ms (%s)\n", question, answer, now_ms() - t0,
	    answer == expected ? "as Android's daemon expects" : "NOT the expected answer");
	return answer == expected;
}

/* the table of rings the running CP reads when it is told PIF_INIT_DONE */
static void
write_rings(void)
{
	uint32_t desc = SBD_DESC + SBD_GLOBAL_BYTES;    /* the next free byte of the table's 62 KB */
	uint32_t buffers = SBD_BUFFERS;
	uint32_t counters;

	bzero(cp.ipc + SBD_DESC, SBD_BUFFERS - SBD_DESC);
	counters = desc;
	desc += 2 * 2 * 2 * NRINGS;                      /* u16 ul_rp[], ul_wp[], dl_rp[], dl_wp[] */
	ipc_wr32(SBD_DESC + 0, 1);                      /* version */
	ipc_wr32(SBD_DESC + 4, NRINGS);
	ipc_wr32(SBD_DESC + 8, counters);
	for (uint32_t i = 0; i < NRINGS; i++) {
		const struct ring_cfg *c = &ring_cfgs[i];

		for (uint32_t dir = 0; dir < 2; dir++) {
			struct ring *r = dir == 0 ? &cp.ul[i] : &cp.dl[i];
			uint32_t d = SBD_DESC + SBD_RB_DESC + (i * 2 + dir) * 20;

			r->len = dir == 0 ? c->ul_len : c->dl_len;
			r->size = dir == 0 ? c->ul_size : c->dl_size;
			r->buffers = buffers;
			buffers += (uint32_t)r->len * r->size;
			r->rp = counters + 2 * (NRINGS * (dir * 2 + 0) + i);
			r->wp = counters + 2 * (NRINGS * (dir * 2 + 1) + i);
			r->addr_v = desc;
			desc += 4u * r->len;
			r->size_v = desc;
			desc += 4u * r->len;
			for (uint32_t k = 0; k < r->len; k++) {
				ipc_wr32(r->addr_v + 4 * k, r->buffers + k * r->size);
			}
			/* the ring's descriptor: channel, 0, direction, signalling by interrupt, the message to send for it,
			 * length, number, buffer size, payload offset */
			ipc_wr16(d + 0, c->ch);
			ipc_wr16(d + 2, 0);
			ipc_wr16(d + 4, (uint16_t)dir);
			ipc_wr16(d + 6, 1);
			ipc_wr32(d + 8, MSG_VALID | MSG_DATA);
			ipc_wr16(d + 12, r->len);
			ipc_wr16(d + 14, (uint16_t)i);
			ipc_wr16(d + 16, r->size);
			ipc_wr16(d + 18, 0);
			/* the channel's entry: where its two descriptors and their offset arrays are */
			ipc_wr32(SBD_DESC + SBD_RB_CH + i * 16 + dir * 8 + 0, d);
			ipc_wr32(SBD_DESC + SBD_RB_CH + i * 16 + dir * 8 + 4, r->addr_v);
		}
	}
	__builtin_arm_dsb(0xf);
	say("cp: the ring table written: %u channels, the table ends at %u of 65536, the buffers at %u of %u\n",
	    (uint32_t)NRINGS, desc, buffers, CP_IPC_BYTES);
}

/* ---- stage 4: the link ---- */

static bool fmt_send(uint8_t group, uint8_t index, uint8_t type, const uint8_t *data, uint16_t n);

static bool
ring_send(uint32_t i, const uint8_t *frame, uint32_t n, bool tell)
{
	struct ring *r = &cp.ul[i];
	uint16_t wp = ipc_rd16(r->wp), rp = ipc_rd16(r->rp);

	if (n > r->size || wp >= r->len || rp >= r->len || (uint16_t)((wp + 1) % r->len) == rp) {
		say("cp: no room to send on channel %u (write %u, read %u of %u)\n", ring_cfgs[i].ch, wp, rp, r->len);
		return false;
	}
	memcpy(cp.ipc + r->buffers + (uint32_t)wp * r->size, frame, n);
	ipc_wr32(r->size_v + 4u * wp, n);
	__builtin_arm_dsb(0xf);
	ipc_wr16(r->wp, (uint16_t)((wp + 1) % r->len));
	__builtin_arm_dsb(0xf);
	if (tell) {
		send_msg(MSG_VALID | MSG_DATA);
	}
	return true;
}

/* one control message to the CP: the group, the number in it, the type (2: "get") */
static bool
fmt_send(uint8_t group, uint8_t index, uint8_t type, const uint8_t *data, uint16_t n)
{
	uint8_t frame[64];
	uint16_t len = (uint16_t)(7 + n);

	if (!cp.link || n > sizeof(frame) - 11) {       /* (cp.link is set before INIT_END is sent) */
		return false;
	}
	frame[0] = 0xf8;
	frame[1] = CH_FMT;
	frame[2] = (uint8_t)(4 + len);
	frame[3] = 0;
	frame[4] = (uint8_t)len;
	frame[5] = (uint8_t)(len >> 8);
	frame[6] = ++cp.mseq;
	frame[7] = 0;
	frame[8] = group;
	frame[9] = index;
	frame[10] = type;
	if (n != 0) {
		memcpy(frame + 11, data, n);
	}
	say("cp: sent to the CP on channel 235: group %#x number %#x type %#x, sequence %u, %u bytes of data\n", group, index,
	    type, cp.mseq, n);
	/* while the CP has not taken INIT_END the frame only waits in the ring: the "data" message would replace
	 * INIT_END in the one message word (the watch loop tells the CP once it has) */
	return ring_send(1, frame, 4u + len, cp.link_acked);
}

/* the phone state asked for: 1 low power (bytes 01 00), 2 normal (02 02): Samsung IPC's power group 0x01, number
 * 0x07, "execute": what Android's RIL sends when airplane mode is switched on and off */
static void
ask_phone_state(int state)
{
	static const uint8_t low[2] = { 0x01, 0x00 }, normal[2] = { 0x02, 0x02 };

	(void)fmt_send(0x01, 0x07, 0x01, state == 2 ? normal : low, 2);
}

/* the band selection asked for (network group 0x08, number 7, "get", no data): what bands the modem is set to use.
 * It sets nothing and carries no identity; its answer is logged as its length and first bytes. */
static void
ask_bands(void)
{
	(void)fmt_send(0x08, 0x07, 0x02, NULL, 0);
}

/* the modem's software version asked for: Samsung IPC's "MISC" group (0x0A), number 1, "get", one byte 0xFF.
 * It sets nothing. (Number 3 of the same group is the serial number: never asked.) */
static void
ask_version(void)
{
	static const uint8_t all = 0xff;

	(void)fmt_send(0x0a, 0x01, 0x02, &all, 1);
}

static void
print_text(const char *what, const uint8_t *p, uint32_t n)
{
	char text[65];
	uint32_t k = 0;

	for (uint32_t i = 0; i < n && k < sizeof(text) - 1 && p[i] != 0; i++) {
		text[k++] = (p[i] >= 0x20 && p[i] < 0x7f) ? (char)p[i] : '.';
	}
	text[k] = 0;
	say("cp:   %s \"%s\"\n", what, text);
}

static void
fmt_received(const uint8_t *m, uint32_t n)
{
	uint16_t len;
	uint8_t group, index, type;

	if (n < 7) {
		say("cp: from the CP on channel 235: %u bytes, too short for a message\n", n);
		return;
	}
	memcpy(&len, m, 2);
	group = m[4];
	index = m[5];
	type = m[6];
	cp.fmt_messages++;
	say("cp: from the CP on channel 235: group %#x number %#x type %#x (%s), its sequence %u answering ours %u, %u "
	    "bytes of data (its length word %u)\n", group, index, type, type == 1 ? "indication" : type == 2 ? "response" :
	    type == 3 ? "notification" : "?", m[2], m[3], n - 7, len);
	if (group == 0x0a && index == 0x01 && type == 2 && n >= 7 + 64) {
		/* the version answer: text fields of 32 bytes: software, hardware, calibration date (no identity in them) */
		say("cp: THE MODEM ANSWERED THE VERSION QUESTION (%u bytes):\n", n - 7);
		print_text("software version", m + 7 + 1, 32);
		if (n >= 7 + 1 + 64) {
			print_text("hardware version", m + 7 + 33, 32);
		}
		if (n >= 7 + 1 + 96) {
			print_text("calibration date", m + 7 + 65, 32);
		}
		cp.version_answered = true;
	} else if (group == 0x08 && index == 0x07 && type == 2) {
		say("cp: THE MODEM ANSWERED THE BAND SELECTION QUESTION (%u bytes): %02x %02x %02x %02x %02x %02x %02x %02x\n",
		    n - 7, n > 7 ? m[7] : 0, n > 8 ? m[8] : 0, n > 9 ? m[9] : 0, n > 10 ? m[10] : 0, n > 11 ? m[11] : 0,
		    n > 12 ? m[12] : 0, n > 13 ? m[13] : 0, n > 14 ? m[14] : 0);
	} else if (group == 0x80 && n >= 7 + 5) {
		/* the general response: which request it answers and a 16-bit result */
		say("cp:   a general response to group %#x number %#x type %#x: result %#x\n", m[7], m[8], m[9],
		    (uint32_t)(m[10] | m[11] << 8));
	} else if (group == 0x01 && index == 0x07 && n >= 7 + 1) {
		/* the phone state: 1 low power (the radio off), 2 normal */
		cp.phone_state = m[7];
		say("cp:   PHONE STATE %u (%s)\n", m[7], m[7] == 1 ? "low power mode: the radio is off" : m[7] == 2 ?
		    "NORMAL: the radio may be on" : "not a value this driver knows");
		if (m[7] != 1 && (!cp.normal_asked || cp.back_asked)) {
			say("cp:   the CP is not in low power mode: it is stopped now\n");
			cp.stop = true;
		}
	} else if (group == 0x01 && n <= 7 + 8) {
		/* power group: short state words, no identity */
		say("cp:   power group data: %02x %02x %02x %02x (of %u)\n", n > 7 ? m[7] : 0, n > 8 ? m[8] : 0, n > 9 ? m[9] : 0,
		    n > 10 ? m[10] : 0, n - 7);
	} else if (group == 0x05 && (index == 0x01 || index == 0x07) && n <= 7 + 2) {
		/* the SIM's status (number 1: 0x00 initialising, 0x80 no card, 0x82 ready, 0x83 phone book ready, 1-8 a lock)
		 * and the card's type (number 7): no identity in either */
		say("cp:   SIM %s: %02x %02x (of %u)\n", index == 1 ? "status" : "card type", n > 7 ? m[7] : 0, n > 8 ? m[8] : 0,
		    n - 7);
		if (index == 1 && n > 7) {
			cp.sim_status = m[7];
			cp.sim_ms = now_ms();
		}
	} else if (group == 0x08 && index == 0x05 && n >= 7 + 3) {
		/* the registration state: technology, domain, state (1 none, 2 home, 3 searching, 4 emergency only, 5 unknown,
		 * 6 roaming). The cell's numbers after them are not logged. */
		say("cp:   REGISTRATION: technology %u, domain %u, state %u (%s)\n", m[7], m[8], m[9], m[9] == 1 ? "none" :
		    m[9] == 2 ? "HOME" : m[9] == 3 ? "SEARCHING" : m[9] == 4 ? "EMERGENCY ONLY" : m[9] == 6 ? "ROAMING" : "?");
		if (m[9] != (uint8_t)cp.reg_state) {
			cp.reg_changes++;
		}
		cp.reg_state = m[9];
		if ((m[9] == 2 || m[9] == 6) && cp.registered_ms == 0 && cp.normal_asked) {
			cp.registered_ms = now_ms();
			say("cp:   REGISTERED %llu ms after normal mode was asked for\n", cp.registered_ms - cp.normal_ms);
		}
		if (m[9] != 1 && !cp.normal_asked) {
			say("cp:   a registration state other than \"none\" without normal mode asked for: the CP is stopped now\n");
			cp.stop = true;
		}
	} else if (group == 0x08 && index == 0x03 && n >= 7 + 4) {
		/* the serving network: its operator code is the run of 5 or 6 digits in it (the country's and the network's
		 * numbers, the same for every subscriber of that network); nothing else of the message is logged */
		char code[8];
		uint32_t k = 0;

		for (uint32_t i = 7; i < n && k == 0; i++) {
			uint32_t d = 0;

			while (i + d < n && d < 6 && m[i + d] >= '0' && m[i + d] <= '9') {
				d++;
			}
			if (d >= 5) {
				memcpy(code, m + i, d);
				k = d;
			}
		}
		code[k] = 0;
		say("cp:   SERVING NETWORK: operator code %s (of %u bytes)\n", k != 0 ? code : "not found in it", n - 7);
	} else if (group == 0x07 && (index == 0x01 || index == 0x06) && n >= 7 + 4) {
		/* signal: the display group's icon (number 1) and strength (number 6) words: levels, no identity */
		cp.signals++;
		if (cp.signals <= 6 || cp.signals % 10 == 0) {
			say("cp:   SIGNAL (%s, the %uth): %02x %02x %02x %02x %02x %02x\n", index == 1 ? "icons" : "strength",
			    cp.signals, m[7], m[8], m[9], m[10], n > 11 ? m[11] : 0, n > 12 ? m[12] : 0);
		}
	}
	/* everything else: the header above and the length only (a message may carry an identity) */
}

/* a file request of the CP: logged, never answered, nothing of it kept */
static void
rfs_received(const uint8_t *m, uint32_t n)
{
	uint32_t len = 0;
	uint8_t cmd, id;

	/* a file request is a stream: its first six bytes say how long it is, and a long one (an NV write with its
	 * data) goes on through the next frames with nothing to mark them. Those frames are data: counted, dropped,
	 * never looked into (seen twice: taken for requests, their first bytes were printed) */
	if (cp.rfs_more != 0) {
		cp.rfs_more -= MIN(cp.rfs_more, n);
		cp.dropped[3]++;
		return;
	}
	cp.rfs_requests++;
	if (n < 6) {
		say("cp: from the CP on channel 245: %u bytes, too short for a file request\n", n);
		return;
	}
	memcpy(&len, m, 4);
	cmd = m[4];
	id = m[5];
	if (len > n) {
		cp.rfs_more = len - n;
	}
	if ((cmd == 1 || cmd == 2) && n >= 14) {
		uint32_t offset, length;

		memcpy(&offset, m + 6, 4);
		memcpy(&length, m + 10, 4);
		say("cp: FILE REQUEST %u from the CP (not answered): %s of the NV data, offset %#x, %u bytes (request %u, id %u)\n",
		    cp.rfs_requests, cmd == 1 ? "a READ" : "a WRITE (its data dropped, never kept)", offset, length, len, id);
		cp.writes_dropped += cmd == 2;
		return;
	}
	if (len > n) {
		/* longer than this buffer and not an NV read or write: its header only, no look for a name in data */
		say("cp: FILE REQUEST %u from the CP (not answered): command %#x, id %u, %u bytes in all (data dropped)\n",
		    cp.rfs_requests, cmd, id, len);
		return;
	}
	say("cp: FILE REQUEST %u from the CP (not answered): command %#x, id %u, %u bytes in all\n", cp.rfs_requests, cmd, id,
	    len);
	/* a path, if the request starts with one after a few words: printable text of 4 characters or more with a '/' or '.' */
	for (uint32_t i = 6; i + 4 < n && i < 64; i++) {
		uint32_t k = i;
		bool path = false;

		while (k < n && k - i < 63 && m[k] >= 0x20 && m[k] < 0x7f) {
			path = path || m[k] == '/' || m[k] == '.';
			k++;
		}
		if (k - i >= 4 && path && k < n && m[k] == 0) {
			print_text("it names", m + i, k - i + 1);
			break;
		}
	}
}

static void
rings_receive(void)
{
	for (uint32_t i = 0; i < NRINGS; i++) {
		struct ring *r = &cp.dl[i];
		uint16_t wp, rp;

		if (r->size < 64) {
			continue;       /* the data ring of addresses: nothing can arrive, no buffers were given */
		}
		wp = ipc_rd16(r->wp);
		rp = ipc_rd16(r->rp);
		while (rp != wp && wp < r->len && rp < r->len) {
			static uint8_t frame[4096];
			uint32_t n = ipc_rd32(r->size_v + 4u * rp) & 0xffff;
			uint32_t hdr;

			if (n > r->size) {
				n = r->size;
			}
			if (cp.more[i] != 0) {
				/* the rest of a frame longer than one buffer (a file request with data): counted, not copied,
				 * not looked into */
				cp.more[i] -= MIN(cp.more[i], n);
				rp = (uint16_t)((rp + 1) % r->len);
				ipc_wr16(r->rp, rp);
				continue;
			}
			memcpy(frame, cp.ipc + r->buffers + (uint32_t)rp * r->size, n);
			rp = (uint16_t)((rp + 1) % r->len);
			ipc_wr16(r->rp, rp);
			cp.frames[i]++;
			hdr = n >= 1 && (frame[0] & 2) ? ((frame[0] & 3) == 3 ? 5 : 6) : 4;
			if (n >= 6 && (frame[0] & 0xf8) == 0xf8) {
				uint32_t total = hdr == 6 ? (uint32_t)(frame[2] | frame[3] << 8 | frame[4] << 16) | (uint32_t)frame[5] << 24 :
				    (uint32_t)(frame[2] | frame[3] << 8);

				if (total > n) {
					cp.more[i] = total - n;
				}
			}
			if (n >= 5 && (frame[0] & 0xf8) == 0xf8 && hdr == 5) {
				/* a message cut into several frames ("multi frame": a fifth header byte, bit 7 "more to come"):
				 * the CP's file requests with data come so. Only the first frame is looked into; the others are
				 * data, counted and dropped (seen: taken for requests, their first bytes were printed) */
				bool rest = cp.multi[i];

				cp.multi[i] = (frame[4] & 0x80) != 0;
				if (rest) {
					cp.frames[i]--;
					cp.dropped[i]++;
					bzero(frame, n);
					continue;
				}
			}
			if (n < hdr || (frame[0] & 0xf8) != 0xf8) {
				say("cp: from the CP on ring %u (%s): %u bytes that are no frame\n", i, ring_cfgs[i].name, n);
			} else if (frame[1] == CH_FMT || frame[1] == 236) {
				fmt_received(frame + hdr, n - hdr);
			} else if (frame[1] == CH_RFS) {
				rfs_received(frame + hdr, n - hdr);
			} else {
				say("cp: from the CP on channel %u (%s): a frame of %u bytes (not looked into)\n", frame[1],
				    ring_cfgs[i].name, n);
			}
			bzero(frame, n);
		}
	}
}

/* ---- the CP's messages ---- */

/* false when the CP said it crashed */
static bool
handle_msg(uint16_t v)
{
	if (!(v & MSG_VALID)) {
		say("cp: the CP raised its message interrupt with %#x in its word (not a valid message)\n", v);
		return true;
	}
	if (v & MSG_CMD) {
		switch (v & 0x3f) {
		case CMD_INIT_START:
			say("cp: the CP says INIT_START (it runs its main program)\n");
			write_rings();
			send_msg(MSG_VALID | MSG_CMD | CMD_PIF_INIT_DONE);
			say("cp: told the CP PIF_INIT_DONE\n");
			break;
		case CMD_PHONE_START:
			cp.phone_starts++;
			if (!cp.up) {
				/* the link's words as Samsung's kernel sets them when the CP reports itself started */
				ipc_wr32(IPC_ACCESS, 0);
				ipc_wr32(IPC_MAGIC, 0);
				ipc_wr32(IPC_RAW_TX_HEAD, 0);
				ipc_wr32(IPC_RAW_TX_TAIL, 0);
				ipc_wr32(IPC_RAW_RX_HEAD, 0);
				ipc_wr32(IPC_RAW_RX_TAIL, 0);
				ipc_wr32(8, 0);
				ipc_wr32(12, 0);
				ipc_wr32(16, 0);
				ipc_wr32(20, 0);
				ipc_wr32(IPC_MAGIC, MAGIC_IPC);
				ipc_wr32(IPC_ACCESS, 1);
				cp.up = true;
				set_state(3);
				say("cp: the CP says PHONE_START: THE CP IS UP (its state word %#x: state %u, active %u)\n",
				    rd(cp.mbox, MBOX_ISSR(WORD_CP_STATE)), CP_STATE_STATUS(rd(cp.mbox, MBOX_ISSR(WORD_CP_STATE))),
				    CP_STATE_ACTIVE(rd(cp.mbox, MBOX_ISSR(WORD_CP_STATE))));
			} else if (cp.phone_starts < 5 || cp.phone_starts % 20 == 0) {
				say("cp: the CP says PHONE_START again (%u times now)\n", cp.phone_starts);
			}
			cp.phone_start_ms = now_ms();
			if (cp.link && !cp.link_acked) {
				/* it has not taken INIT_END yet (it looks at the message word about every 100 ms): told again, as
				 * Samsung's kernel does on every repeated PHONE_START */
				send_msg(MSG_VALID | MSG_CMD | CMD_INIT_END);
			}
			if (!cp.link && cp.stage >= 4 && !cp.nv_loaded) {
				if (cp.phone_starts == 1) {
					say("cp: stage 4 REFUSED: the CP has no NV data of its own (zeros): INIT_END is not sent, its protocol "
					    "stack is not started, the run stays at stage 3 (a CP with blank NV goes to its normal state by "
					    "itself with a default identity and no calibration: never near a network)\n");
				}
			} else if (!cp.link && cp.stage >= 4) {
				static const uint8_t lpm[2] = { 0x01, 0x00 };

				cp.link = true;
				set_state(4);
				/* first, so that it is the first thing the CP finds when its stack starts: the radio off. It waits in
				 * the ring; then INIT_END */
				(void)fmt_send(0x01, 0x07, 0x01, lpm, sizeof(lpm));
				send_msg(MSG_VALID | MSG_CMD | CMD_INIT_END);
				cp.init_end_ms = cp.lpm_sent_ms = now_ms();
				say("cp: \"LOW POWER MODE\" (the radio off) put in the ring for the CP, the first thing it will find; "
				    "then told the CP INIT_END\n");
			}
			break;
		case CMD_CRASH_RESET:
		case CMD_CRASH_EXIT:
			say("cp: THE CP SAYS IT CRASHED (%s)\n", (v & 0x3f) == CMD_CRASH_RESET ? "CRASH_RESET" : "CRASH_EXIT");
			/* its reason, if it left text at the place Samsung's kernel reads one (the IPC memory's 2 KB at 2048 -
			 * the old layout - or 63488): printable text only */
			print_text("text at the IPC memory's 0x800:", cp.ipc + 0x800 + 4, 64);
			print_text("text at the IPC memory's 0xF800:", cp.ipc + 0xf800 + 4, 64);
			return false;
		default:
			say("cp: the CP sent command %#x (not one this driver knows)\n", v & 0x3f);
			break;
		}
	}
	if (cp.up && (v & MSG_DATA || !(v & MSG_CMD))) {
		rings_receive();
	}
	return true;
}

/* the mailbox looked at once: false when the CP crashed */
static bool
poll_once(void)
{
	static uint32_t last_state = 0xffffffffu;
	uint32_t pending = rd(cp.mbox, MBOX_INTSR0) & 0xffff0000u;
	uint32_t state = rd(cp.mbox, MBOX_ISSR(WORD_CP_STATE));
	bool alive = true;

	if (pending != 0) {
		uint16_t msg = (uint16_t)rd(cp.mbox, MBOX_ISSR(WORD_CP_MSG));

		wr(cp.mbox, MBOX_INTCR0, pending);
		if (pending & (1u << (16 + IRQ_CP_MSG))) {
			alive = handle_msg(msg);
		}
		if (pending & ~(1u << (16 + IRQ_CP_MSG))) {
			say("cp: the CP raised interrupts %#x (2 state, 3 active, 5-7 clock requests, 8 wake lock, 9 radio mode); "
			    "its state word %#x, words 5-7 %#x %#x %#x\n", pending >> 16, state, rd(cp.mbox, MBOX_ISSR(5)),
			    rd(cp.mbox, MBOX_ISSR(6)), rd(cp.mbox, MBOX_ISSR(7)));
		}
	}
	if (state != last_state) {
		say("cp: the CP's state word is now %#x (state %u, active %u)\n", state, CP_STATE_STATUS(state),
		    CP_STATE_ACTIVE(state));
		last_state = state;
	}
	if (cp.up) {
		rings_receive();
	}
	return alive;
}

static bool
start(void)
{
	uint64_t until;
	uint32_t state = 0;

	/* the mailbox as Samsung's kernel has it before the CP runs: every word 0, then the AP's state */
	wr(cp.mbox, MBOX_INTMR0, 0xffff0000u);          /* its interrupts to the AP: looked at here, never delivered */
	wr(cp.mbox, MBOX_INTCR0, 0xffff0000u);
	wr(cp.mbox, MBOX_INTCR1, 0xffffu);
	for (uint32_t i = 0; i < MBOX_WORDS; i++) {
		wr(cp.mbox, MBOX_ISSR(i), 0);
	}
	wr(cp.mbox, MBOX_ISSR(WORD_AP_STATE), AP_STATE_REV(cp.rev) | AP_STATE_DS(cp.ds) | AP_STATE_PDA_ACTIVE);
	say("cp: the mailbox cleared; the AP's state word %#x (revision %u, dual SIM field %u, PDA active)\n",
	    rd(cp.mbox, MBOX_ISSR(WORD_AP_STATE)), cp.rev, cp.ds);

	say("cp: the IPC memory before the start: magic %#x access %#x\n", ipc_rd32(IPC_MAGIC), ipc_rd32(IPC_ACCESS));

	say("cp: AP2CP_CFG was %#x: writing 1, the CP's processor may run\n", rd(cp.cfg, 0));
	cp.started = true;
	wr(cp.cfg, 0, 1);
	say("cp: AP2CP_CFG is %#x\n", rd(cp.cfg, 0));

	until = now_ms() + 3000;
	do {
		state = rd(cp.mbox, MBOX_ISSR(WORD_CP_STATE));
		if (CP_STATE_STATUS(state) != 0) {
			break;
		}
		IOSleep(5);
	} while (now_ms() < until);
	if (CP_STATE_STATUS(state) == 0) {
		say("cp: the CP did not report a state in 3 s (its word %#x)\n", state);
		look_pmu("with the CP silent");
		look_mbox("with the CP silent");
		return false;
	}
	say("cp: THE CP'S BOOT LOADER RUNS: its state word %#x (state %u) %llu ms after the start\n", state,
	    CP_STATE_STATUS(state), now_ms() - (until - 3000));

	/* Android's order: only now, with the boot loader running, the old queues' pointers are zeroed and the magic
	 * "BOOT" written (the boot loader clears the memory's first words itself when it starts: a magic written before
	 * the start was gone, and the first question was never taken) */
	say("cp: the IPC memory with the boot loader running: magic %#x access %#x, queue words %#x %#x %#x %#x\n",
	    ipc_rd32(IPC_MAGIC), ipc_rd32(IPC_ACCESS), ipc_rd32(IPC_RAW_TX_HEAD), ipc_rd32(IPC_RAW_TX_TAIL),
	    ipc_rd32(IPC_RAW_RX_HEAD), ipc_rd32(IPC_RAW_RX_TAIL));
	for (uint32_t off = 8; off <= IPC_RAW_RX_TAIL; off += 4) {
		ipc_wr32(off, 0);
	}
	ipc_wr32(IPC_MAGIC, MAGIC_BOOT);
	__builtin_arm_dsb(0xf);
	if (ipc_rd32(IPC_MAGIC) != MAGIC_BOOT) {
		say("cp: the IPC memory's magic reads back %#x, not \"BOOT\"\n", ipc_rd32(IPC_MAGIC));
		return false;
	}
	/* and the AP's "boot confirm": its own state field 1 in its state word. The boot loader takes no question
	 * before it (seen: without it the first question lay in the queue untouched for 5 s) */
	wr(cp.mbox, MBOX_ISSR(WORD_AP_STATE), rd(cp.mbox, MBOX_ISSR(WORD_AP_STATE)) | AP_STATE_STATUS(1));
	say("cp: the magic \"BOOT\" written, the AP's state word now %#x (its state field 1: boot confirmed)\n",
	    rd(cp.mbox, MBOX_ISSR(WORD_AP_STATE)));
	if (!boot_ask(0x0000900d, 0x0000a00d) || !boot_ask(0x00009f00, 0x0000af00)) {
		return false;
	}
	say("cp: the boot loader's handshake is complete; waiting for the CP's main program\n");

	until = now_ms() + 15000;
	while (!cp.up && now_ms() < until) {
		if (!poll_once()) {
			return false;
		}
		IOSleep(1);
	}
	if (!cp.up) {
		say("cp: the CP's main program did not report itself in 15 s\n");
		look_pmu("with no PHONE_START");
		look_mbox("with no PHONE_START");
		return false;
	}
	return true;
}

static void
print_state(void)
{
	say("cp: state %u; frames from the CP by ring:", cp.state);
	for (uint32_t i = 0; i < NRINGS; i++) {
		say(" %s %u", ring_cfgs[i].name, cp.frames[i]);
	}
	say("; control messages %u, file requests %u (none answered; %u frames of their data dropped unread), PHONE_START "
	    "%u times, last phone state %d\n", cp.fmt_messages, cp.rfs_requests, cp.dropped[3], cp.phone_starts,
	    cp.phone_state);
	if (cp.mbox != 0) {
		look_mbox("now");
		look_pmu("now");
	}
}

static void
cp_thread(__unused void *arg, __unused wait_result_t result)
{
	uint64_t until, asked = 0, kick = 0;
	const char *why = NULL, *why5 = NULL;
	bool alive = true;

	thread_set_thread_name(current_thread(), "hakios.shannon");
	/* not at once: the boot's own flood of lines would push these out of the log before the USB console has it */
	IOSleep(cp.delay_s * 1000);
	say("cp: stage %u asked for (1 a look, 2 load and check, 3 start, 4 the link, 5 the network)%s\n", cp.stage,
	    cp.nv_file ? ", with the builder's nv_data.bin" : ", no non-volatile data (zeros)");

	if (!memory_is_free()) {
		set_state(101);
		return;
	}
	cp.pmu = ml_io_map(CP_PMU_PA, CP_PMU_BYTES);
	cp.mbox = ml_io_map(CP_MBOX_PA, 0x1000);
	cp.cfg = ml_io_map(CP_CFG_PA, 0x1000);
	cp.gicd = ml_io_map(CP_GICD_PA + 0x1000, 0x1000);       /* the page of the "set pending" words */
	cp.mem = hakios_physmap_wc(CP_MEM_PA, CP_MEM_BYTES);
	if (cp.pmu == 0 || cp.mbox == 0 || cp.cfg == 0 || cp.mem == 0) {
		say("cp: a mapping failed (PMU %d, mailbox %d, AP2CP_CFG %d, the CP's memory %d)\n", cp.pmu != 0, cp.mbox != 0,
		    cp.cfg != 0, cp.mem != 0);
		set_state(101);
		return;
	}
	cp.ipc = (uint8_t *)cp.mem + CP_IPC_OFFSET;

	look_pmu("as found");
	look_mbox("as found");
	look_mem();
	if (!read_toc()) {
		set_state(101);
		return;
	}
	set_state(1);
	if (cp.stage < 2) {
		say("cp: stage 1 done: looked, nothing changed\n");
		return;
	}

	if (!load_and_check()) {
		set_state(102);
		look_pmu("after the failed load");
		return;
	}
	set_state(2);
	look_pmu("after the check");
	if (cp.stage < 3) {
		say("cp: stage 2 done: the image is loaded and accepted, the CP is not started\n");
		return;
	}

	if (!start()) {
		set_state(103);
		cp_stop("the start did not complete");
		return;
	}

	/* watching: everything the CP says, until the time is up or we are told to stop */
	until = now_ms() + (uint64_t)cp.wait_s * 1000;
	while (alive) {
		int go = cp_go;

		alive = poll_once();
		if (cp.stop && (!cp.normal_asked || cp.back_asked)) {
			why = "it was not in low power mode, or reported a registration state";
			break;
		}
		if (cp.link && !cp.link_acked && now_ms() - cp.init_end_ms > 150 && now_ms() - cp.phone_start_ms > 350) {
			cp.link_acked = true;
			cp.lpm_sent_ms = now_ms();
			say("cp: the CP has stopped repeating PHONE_START %llu ms after INIT_END: it took it, the link is up\n",
			    now_ms() - cp.init_end_ms);
		}
		if (cp.link && !cp.link_acked && now_ms() - cp.init_end_ms > 4000) {
			say("cp: the CP still repeats PHONE_START 4 s after INIT_END: it is stopped now\n");
			why = "it did not take INIT_END";
			break;
		}
		if (cp.link_acked && now_ms() >= kick && ipc_rd16(cp.ul[1].rp) != ipc_rd16(cp.ul[1].wp)) {
			/* what we sent is still in the ring: the CP is told again that there is data (it may not have been
			 * listening yet when it was first told) */
			send_msg(MSG_VALID | MSG_DATA);
			kick = now_ms() + 200;
		}
		if (cp.link_acked && cp.phone_state != 1 && !cp.normal_asked && now_ms() - cp.lpm_sent_ms > 5000) {
			say("cp: no \"low power mode\" from the CP 5 s after it was asked (its last phone state: %d): it is stopped now\n",
			    cp.phone_state);
			why = "it did not confirm low power mode";
			break;
		}
		if (cp.stage >= 5 && cp.link_acked && cp.phone_state == 1 && !cp.normal_asked && cp.version_answered) {
			/* stage 5, step 1: the SIM's status, read in low power mode */
			uint64_t waited = now_ms() - cp.lpm_sent_ms;

			if ((cp.sim_status == 0x82 || cp.sim_status == 0x83) && now_ms() - cp.sim_ms > 2000) {
				say("cp: STAGE 5: the SIM is ready (status %#x) and the CP is in low power mode: asking for NORMAL mode "
				    "(the radio on; the phone will register on its network)\n", cp.sim_status);
				cp.normal_asked = true;
				cp.normal_ms = now_ms();
				until = cp.normal_ms + 100000;          /* 100 s to register, at most */
				ask_phone_state(2);
			} else if (cp.sim_status >= 1 && cp.sim_status <= 8 && now_ms() - cp.sim_ms > 3000) {
				say("cp: STAGE 5: the SIM is locked (status %#x: it wants a PIN or PUK). NO attempt is made. Staying in low "
				    "power mode and stopping\n", cp.sim_status);
				why5 = "the SIM is locked";
				break;
			} else if (waited > 20000) {
				say("cp: STAGE 5: no SIM ready 20 s after the link came up (its last status %#x; 0x80 no card, 0x81 card "
				    "error, 0 initialising): normal mode is NOT asked for\n", cp.sim_status);
				why5 = "no SIM ready";
				break;
			}
		}
		if (cp.normal_asked && !cp.back_asked) {
			/* stage 5, steps 2 and 3: registered for two minutes, or not in time, then low power again */
			const char *back = NULL;

			if (cp.registered_ms != 0 && now_ms() - cp.registered_ms > 120000) {
				back = "registered for two minutes";
			} else if (cp.registered_ms == 0 && now_ms() - cp.normal_ms > 90000) {
				back = "NOT registered 90 s after normal mode was asked for";
				why5 = "not registered in time";
			} else if (cp.registered_ms != 0 && cp.reg_state != 2 && cp.reg_state != 6 &&
			    now_ms() - cp.registered_ms > 5000) {
				back = "the registration was lost";
				why5 = "registration lost";
			} else if (cp.stop || go == 2) {
				back = "asked to stop";
			}
			if (back != NULL) {
				say("cp: STAGE 5: %s (last registration state %d, %u changes, %u signal reports): asking for LOW POWER MODE\n",
				    back, cp.reg_state, cp.reg_changes, cp.signals);
				cp.back_asked = true;
				cp.back_ms = now_ms();
				cp.phone_state = -1;
				cp.stop = false;
				until = cp.back_ms + 12000;
				ask_phone_state(1);
			} else {
				until = now_ms() + 30000;       /* the stage's own clocks decide */
			}
		}
		if (cp.back_asked && cp.phone_state == 1 && now_ms() - cp.back_ms > 1500) {
			say("cp: STAGE 5: the CP is in low power mode again %llu ms after it was asked\n", now_ms() - cp.back_ms);
			break;
		}
		if (go != 0) {
			cp_go = 0;
			if (go == 1) {
				print_state();
			} else if (go == 2 && !cp.normal_asked) {
				break;                          /* (in normal mode: low power is asked for first, above) */
			} else if (go == 3 && cp.link) {
				ask_version();
			}
		}
		if (cp.link_acked && cp.phone_state == 1 && asked == 0) {
			asked = now_ms() + 2000;        /* only once it has said it is in low power mode */
		}
		if (asked > 1 && now_ms() >= asked) {
			asked = 1;
			ask_version();
		}
		if (cp.version_answered && !cp.bands_asked && cp.phone_state == 1) {
			cp.bands_asked = true;
			ask_bands();
		}
		if (now_ms() >= until) {
			break;
		}
		IOSleep(1);
	}
	if (cp.normal_asked) {
		say("cp: STAGE 5 SUMMARY: normal mode asked; registered: %s; last registration state %d; %u changes of it; %u "
		    "signal reports; low power again: %s; NV writes the CP asked for and that were dropped: %u\n",
		    cp.registered_ms != 0 ? "YES" : "no", cp.reg_state, cp.reg_changes, cp.signals,
		    cp.back_asked && cp.phone_state == 1 ? "confirmed" : "NOT confirmed", cp.writes_dropped);
	}
	print_state();
	cp_stop(!alive ? "it crashed" : why != NULL ? why : why5 != NULL ? why5 : "the watch is over, or asked to");
	set_state(alive && why == NULL ? 200 : 104);
}

/* loader_return(): the CP in reset before the phone is handed back, if this run started it */
void hakios_shannon_quiesce(void);
void
hakios_shannon_quiesce(void)
{
	if (cp.started) {
		cp_go = 2;
		for (uint32_t i = 0; i < 300 && cp.started; i++) {
			IOSleep(10);
		}
	}
}

void hakios_shannon_init(void);
void
hakios_shannon_init(void)
{
	thread_t thread;
	uint32_t n = 0;

	if (!PE_parse_boot_argn("cp", &n, sizeof(n)) || n == 0) {
		return;
	}
	cp.stage = n > 5 ? 5 : n;
	cp.rev = 26;
	cp.wait_s = cp.stage >= 4 ? 25 : 20;
	cp.delay_s = 30;
	cp.phone_state = cp.sim_status = cp.reg_state = -1;
	cp.vss_on = 1;
	PE_parse_boot_argn("cp_nv", &cp.nv_file, sizeof(cp.nv_file));
	PE_parse_boot_argn("cp_vss", &cp.vss_on, sizeof(cp.vss_on));
	PE_parse_boot_argn("cp_rev", &cp.rev, sizeof(cp.rev));
	PE_parse_boot_argn("cp_ds", &cp.ds, sizeof(cp.ds));
	PE_parse_boot_argn("cp_wait", &cp.wait_s, sizeof(cp.wait_s));
	PE_parse_boot_argn("cp_delay", &cp.delay_s, sizeof(cp.delay_s));
	if (cp.delay_s > 600) {
		cp.delay_s = 600;
	}
	if (cp.wait_s > 600) {
		cp.wait_s = 600;
	}
	if (kernel_thread_start(cp_thread, NULL, &thread) == KERN_SUCCESS) {
		thread_deallocate(thread);
	} else {
		say("cp: no thread\n");
	}
}
