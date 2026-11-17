/* SPDX-License-Identifier: MIT-0 */
/*
 * tools/modem/shannon_test.c: drivers/modem/hakios.shannon.c's handling of what it does not control, on a computer,
 * under the address and undefined-behaviour sanitizers (tools/modem/shannon-test.sh builds it). The driver is included
 * as it is (the script takes out its #include <...> lines and replaces smc(), its one piece of arm64 assembly); the
 * kernel's calls it makes are stood in for below. The CP's IPC memory is a buffer of exactly the IPC memory and the
 * zero-copy region behind it, so the sanitizer stops any byte read or written past it; RADIO is a buffer.
 * What it checks:
 *   - the file requests on the RFS ring with cp_nv=2 (a made-up NV copy): NV reads answered from it, NV writes kept in
 *     it, each sent as the CP sends them (one frame, a frame longer than a buffer, a multi-frame message, unmarked
 *     frames): the copy and the answers as they must be, and the next request answered;
 *   - an NV write that names more data than its request carries: refused, and no byte of a later request lands in the
 *     NV copy; a long request that is not an NV write does not swallow the request after it;
 *   - the control messages (fmt_received), each kind at every length from 0 to past what its handler reads;
 *   - downlink packets (data_received, the zero-copy slots, the ordinary data ring) and pdp_ip0's input;
 *   - the boot loader's queue with its head and tail words bent;
 *   - RADIO's table of contents (read_toc) with every field bent, and the copies a table that passes allows;
 *   - mutated input (a fixed seed; ITERATIONS, 300000 by default) through each of them.
 * usage: shannon_test [ITERATIONS]
 */
#include <errno.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/uio.h>

/* ---- the kernel, as much of it as the driver uses (renamed where a host header may have the name) ---- */

#define boolean_t hk_boolean_t
#define vm_offset_t hk_vm_offset_t
#define ppnum_t hk_ppnum_t
#define wait_result_t hk_wait_result_t
#define thread_t hk_thread_t
#define kern_return_t hk_kern_return_t
#define errno_t hk_errno_t
#define ifnet_t hk_ifnet_t
#define mbuf_t hk_mbuf_t
#define socket_t hk_socket_t
#define protocol_family_t hk_protocol_family_t
#define kauth_cred_t hk_kauth_cred_t
#define user_addr_t hk_user_addr_t
#define in_addr hk_in_addr
#define sockaddr_in hk_sockaddr_in
#define sockaddr_dl hk_sockaddr_dl
#define rt_msghdr hk_rt_msghdr
#define in_aliasreq hk_in_aliasreq
#define sysctl_oid hk_sysctl_oid
#define sysctl_req hk_sysctl_req
#define strlcpy hk_strlcpy

typedef int boolean_t;
typedef uintptr_t vm_offset_t;
typedef uint32_t ppnum_t;
typedef int wait_result_t;
typedef void *thread_t;
typedef int kern_return_t;
typedef int errno_t;
typedef struct hk_ifnet *ifnet_t;
typedef struct hk_mbuf *mbuf_t;
typedef struct hk_socket *socket_t;
typedef uint32_t protocol_family_t;
typedef void *kauth_cred_t;
typedef uint64_t user_addr_t;

#ifndef __unused
#define __unused __attribute__((unused))
#endif
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#undef PAGE_SIZE
#undef PAGE_SHIFT
#define PAGE_SIZE 16384
#define PAGE_SHIFT 14
#define NSEC_PER_MSEC 1000000ull
#define KERN_SUCCESS 0
#define Z_WAITOK 1
#define Z_ZERO 2
#undef USER_ADDR_NULL
#define USER_ADDR_NULL 0
#define MBUF_DONTWAIT 1
#ifndef AF_LINK
#define AF_LINK 18
#endif
#define IFT_CELLULAR 0xff
#define IFNET_INIT_CURRENT_VERSION 2
#define IFNET_INIT_LEGACY 1
#define IFNET_INIT_NX_NOAUTO 2
#define IFNET_FAMILY_CELLULAR 15
#ifndef IFF_UP
#define IFF_UP 0x1
#define IFF_POINTOPOINT 0x10
#define IFF_RUNNING 0x40
#define IFF_NOARP 0x80
#define IFF_MULTICAST 0x8000
#endif
#ifndef SIOCSIFADDR
#define SIOCSIFADDR 1
#define SIOCAIFADDR 2
#define SIOCSIFDSTADDR 3
#define SIOCSIFFLAGS 4
#define SIOCADDMULTI 5
#define SIOCDELMULTI 6
#endif
#ifndef SIOCAIFADDR
#define SIOCAIFADDR 2
#endif
#ifndef RTM_VERSION
#define RTM_VERSION 5
#endif
#ifndef RTM_ADD
#define RTM_ADD 1
#endif
#ifndef RTF_UP
#define RTF_UP 0x1
#endif
#ifndef RTF_STATIC
#define RTF_STATIC 0x800
#endif
#ifndef RTA_DST
#define RTA_DST 0x1
#define RTA_GATEWAY 0x2
#define RTA_NETMASK 0x4
#endif

struct in_addr { uint32_t s_addr; };
struct sockaddr_in { uint8_t sin_len, sin_family; uint16_t sin_port; struct in_addr sin_addr; char sin_zero[8]; };
struct sockaddr_dl { uint8_t sdl_len, sdl_family; uint16_t sdl_index; uint8_t sdl_type, sdl_nlen, sdl_alen, sdl_slen;
	             char sdl_data[12]; };
struct rt_msghdr { unsigned short rtm_msglen; unsigned char rtm_version, rtm_type; unsigned short rtm_index;
	           int rtm_flags, rtm_addrs, rtm_pid, rtm_seq, rtm_errno, rtm_use; uint32_t rtm_inits; };
struct in_aliasreq { char ifra_name[16]; struct sockaddr_in ifra_addr, ifra_broadaddr, ifra_mask; };
struct ifnet_demux_desc { uint32_t type; void *data; uint32_t datalen; };
struct ifnet_stat_increment_param { uint32_t packets_in, bytes_in, errors_in, packets_out, bytes_out, errors_out,
	                            collisions, dropped; };
struct ifnet_init_eparams {
	uint32_t ver, len, flags;
	const void *uniqueid;
	uint32_t uniqueid_len;
	const char *name;
	uint32_t unit, family, type;
	errno_t (*output)(ifnet_t, mbuf_t);
	errno_t (*demux)(ifnet_t, mbuf_t, char *, protocol_family_t *);
	errno_t (*add_proto)(ifnet_t, protocol_family_t, const struct ifnet_demux_desc *, u_int32_t);
	errno_t (*del_proto)(ifnet_t, protocol_family_t);
	errno_t (*ioctl)(ifnet_t, unsigned long, void *);
};
struct ifnet_attach_proto_param {
	errno_t (*input)(ifnet_t, protocol_family_t, mbuf_t, char *);
	errno_t (*pre_output)(ifnet_t, protocol_family_t, mbuf_t *, const struct sockaddr *, void *, char *, char *);
};
struct sysctl_oid;
struct sysctl_req { user_addr_t newptr; char out[256]; size_t outlen; };
#define SYSCTL_HANDLER_ARGS (__unused struct sysctl_oid *oidp, __unused void *arg1, __unused int arg2, struct sysctl_req *req)
#define SYSCTL_DECL(n) extern int hk_sysctl_decl_##n
#define SYSCTL_NODE(...) extern int hk_sysctl_node
#define SYSCTL_INT(...) extern int hk_sysctl_int
#define SYSCTL_PROC(...) extern int hk_sysctl_proc

static int
SYSCTL_OUT(struct sysctl_req *r, const void *p, size_t n)
{
	if (n > sizeof(r->out)) {
		return ENOMEM;
	}
	memcpy(r->out, p, n);
	r->outlen = n;
	return 0;
}

#define LCK_GRP_DECLARE(n, s) int n __attribute__((unused))
#define LCK_MTX_DECLARE(n, g) int n
#define lck_mtx_lock(l) ((void)(l))
#define lck_mtx_unlock(l) ((void)(l))
#define __builtin_arm_dsb(x) ((void)(x))

static bool quiet = true;

static void __attribute__((format(printf, 1, 2)))
say(const char *fmt, ...)
{
	va_list ap;

	if (quiet) {
		return;
	}
	va_start(ap, fmt);
	vprintf(fmt, ap);
	va_end(ap);
}

static size_t
strlcpy(char *d, const char *s, size_t n)
{
	size_t l = strlen(s);

	if (n != 0) {
		size_t k = l < n - 1 ? l : n - 1;

		memcpy(d, s, k);
		d[k] = 0;
	}
	return l;
}

/* a clock that moves a millisecond each time it is read: the driver's waits end at once */
static uint64_t clock_ns;
static uint64_t mach_absolute_time(void) { return clock_ns += 1000000; }
static void absolutetime_to_nanoseconds(uint64_t t, uint64_t *ns) { *ns = t; }
static void IOSleep(unsigned ms) { clock_ns += (uint64_t)ms * 1000000; }
static void delay(unsigned us) { clock_ns += (uint64_t)us * 1000; }
static uint64_t test_smc(uint64_t f, uint64_t a, uint64_t b, uint64_t c) { (void)f; (void)a; (void)b; (void)c; return 0; }
static uint64_t kvtophys(vm_offset_t va) { (void)va; return 0; }
boolean_t pmap_valid_page(ppnum_t pn) { (void)pn; return 0; }
static vm_offset_t ml_io_map(vm_offset_t pa, size_t n) { (void)pa; (void)n; return 0; }
static vm_offset_t hakios_physmap_wc(uint64_t pa, uint64_t n) { (void)pa; (void)n; return 0; }
static void *kalloc_data(size_t n, int flags) { return (flags & Z_ZERO) ? calloc(1, n) : malloc(n); }
static void kfree_data(void *p, size_t n) { (void)n; free(p); }
static int PE_parse_boot_argn(const char *a, void *v, int n) { (void)a; (void)v; (void)n; return 0; }
static void thread_set_thread_name(thread_t t, const char *n) { (void)t; (void)n; }
static thread_t current_thread(void) { return NULL; }
static kern_return_t kernel_thread_start(void (*f)(void *, wait_result_t), void *a, thread_t *t) { (void)f; (void)a; (void)t; return 1; }
static void thread_deallocate(thread_t t) { (void)t; }
static bool is_root = true;
static kauth_cred_t kauth_cred_get(void) { return NULL; }
static int kauth_cred_issuser(kauth_cred_t c) { (void)c; return is_root; }

/* the partitions: RADIO and EFS as buffers, read as the UFS driver reads them (EINVAL past the end) */
static uint8_t *radio, *efs;
static uint64_t radio_size, efs_size;

static int
hakios_ufs_read_partition(const char *name, uint64_t off, uint8_t *dst, uint64_t len, uint64_t *bytes)
{
	uint8_t *p = strcmp(name, "RADIO") == 0 ? radio : strcmp(name, "EFS") == 0 ? efs : NULL;
	uint64_t size = p == radio ? radio_size : efs_size;

	if (p == NULL) {
		return ENOENT;
	}
	if (bytes != NULL) {
		*bytes = size;
	}
	if (len == 0) {
		return 0;
	}
	if (off > size || len > size - off) {
		return EINVAL;
	}
	memcpy(dst, p + off, len);
	return 0;
}

static const uint8_t *nv_file;
static uint64_t nv_file_size;

static const uint8_t *
hakios_image_file(const char *dir, const char *name, uint64_t *size, int *disk)
{
	(void)dir; (void)name; (void)disk;
	*size = nv_file_size;
	return nv_file;
}

/* the network stack: an mbuf is a length and a buffer; what pdp_ip0 gives the stack is checked and counted */
struct hk_mbuf { size_t n; uint8_t *p; };
static uint64_t stack_packets, stack_bytes;
static size_t mbuf_pkthdr_len(mbuf_t m) { return m->n; }
static errno_t mbuf_copydata(mbuf_t m, size_t off, size_t n, void *out) { if (off > m->n || n > m->n - off) return EINVAL; memcpy(out, m->p + off, n); return 0; }
static void mbuf_freem(mbuf_t m) { free(m->p); free(m); }
static errno_t mbuf_allocpacket(int how, size_t n, unsigned int *chunks, mbuf_t *m) { (void)how; (void)chunks; *m = calloc(1, sizeof(**m)); (*m)->n = n; (*m)->p = calloc(1, n); return 0; }
static errno_t mbuf_copyback(mbuf_t m, size_t off, size_t n, const void *d, int how) { (void)how; if (off > m->n || n > m->n - off) return EINVAL; memcpy(m->p + off, d, n); return 0; }
static void mbuf_pkthdr_setrcvif(mbuf_t m, ifnet_t i) { (void)m; (void)i; }
static void mbuf_pkthdr_setheader(mbuf_t m, void *h) { (void)m; (void)h; }
static void *mbuf_data(mbuf_t m) { return m->p; }
static errno_t ifnet_stat_increment_out(ifnet_t i, uint32_t a, uint32_t b, uint32_t c) { (void)i; (void)a; (void)b; (void)c; return 0; }
static errno_t
ifnet_input(ifnet_t i, mbuf_t m, const struct ifnet_stat_increment_param *st)
{
	(void)i;
	if (st->bytes_in != m->n || m->n == 0 || m->n > 2048 || (m->p[0] >> 4) != 4) {
		fprintf(stderr, "pdp_ip0 gave the stack a packet of %zu bytes (said %u), version %u\n", m->n, st->bytes_in,
		    m->n ? m->p[0] >> 4 : 0);
		abort();
	}
	stack_packets++;
	stack_bytes += m->n;
	mbuf_freem(m);
	return 0;
}
static errno_t ifnet_set_flags(ifnet_t i, uint16_t a, uint16_t b) { (void)i; (void)a; (void)b; return 0; }
static uint16_t ifnet_flags(ifnet_t i) { (void)i; return 0; }
static errno_t proto_input(protocol_family_t f, mbuf_t m) { (void)f; (void)m; return 0; }
static errno_t ifnet_allocate_extended(const struct ifnet_init_eparams *p, ifnet_t *i) { (void)p; (void)i; return ENOMEM; }
static errno_t ifnet_set_mtu(ifnet_t i, uint32_t m) { (void)i; (void)m; return 0; }
static errno_t ifnet_set_hdrlen(ifnet_t i, uint32_t m) { (void)i; (void)m; return 0; }
static errno_t ifnet_set_addrlen(ifnet_t i, uint32_t m) { (void)i; (void)m; return 0; }
static errno_t ifnet_set_baudrate(ifnet_t i, uint64_t m) { (void)i; (void)m; return 0; }
static errno_t ifnet_attach(ifnet_t i, const struct sockaddr_dl *d) { (void)i; (void)d; return 0; }
static errno_t ifnet_release(ifnet_t i) { (void)i; return 0; }
static errno_t ifnet_attach_protocol(ifnet_t i, protocol_family_t f, const struct ifnet_attach_proto_param *p) { (void)i; (void)f; (void)p; return 0; }
static uint32_t ifnet_index(ifnet_t i) { (void)i; return 1; }
static errno_t sock_socket(int a, int b, int c, void *d, void *e, socket_t *s) { (void)a; (void)b; (void)c; (void)d; (void)e; (void)s; return EPERM; }
static errno_t sock_ioctl(socket_t s, unsigned long c, void *a) { (void)s; (void)c; (void)a; return 0; }
static void sock_close(socket_t s) { (void)s; }
static errno_t sock_send(socket_t s, const struct msghdr *m, int f, size_t *n) { (void)s; (void)m; (void)f; (void)n; return 0; }

/* ---- the driver ---- */

#include "shannon_body.c"

/* ---- the test ---- */

static unsigned checks, failures;

#define CHECK(c) do { checks++; if (!(c)) { failures++; fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #c); } } while (0)

static uint32_t rng = 20261117;

static uint32_t
rnd(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 17;
	rng ^= rng << 5;
	return rng;
}

static uint32_t mbox_words[0x1000 / 4];
static uint8_t *ipc_buf;                 /* exactly the IPC memory and the zero-copy region */
#define IPC_ALL (CP_IPC_BYTES + ZMB_BYTES)

static void
put32(uint8_t *p, uint32_t v)
{
	memcpy(p, &v, 4);
}

static uint32_t
get32(const uint8_t *p)
{
	uint32_t v;

	memcpy(&v, p, 4);
	return v;
}

/* the link and the rings as the driver sets them up once the CP has said INIT_START */
static void
link_up(uint32_t stage)
{
	memset(&cp, 0, sizeof(cp));
	memset(ipc_buf, 0, IPC_ALL);
	cp.mbox = (vm_offset_t)mbox_words;
	cp.ipc = ipc_buf;
	cp.stage = stage;
	cp.phone_state = cp.sim_status = cp.reg_state = -1;
	cp.tech = cp.bars = -1;
	cp.data_result = -1;
	write_rings();
	cp.up = true;
	cp.link = true;
	cp.link_acked = true;
}

/* one buffer put in downlink ring i as the CP puts it: n bytes of b (at most the buffer), its size word `word` */
static bool
put_dl(uint32_t i, const uint8_t *b, uint32_t n, uint32_t word)
{
	struct ring *r = &cp.dl[i];
	uint16_t wp = ipc_rd16(r->wp), rp = ipc_rd16(r->rp);

	if (wp >= r->len || rp >= r->len) {
		ipc_wr16(r->wp, 0);             /* (a counter bent by the test: the ring starts again) */
		ipc_wr16(r->rp, 0);
		return false;
	}
	if ((uint16_t)((wp + 1) % r->len) == rp) {
		return false;
	}
	memcpy(cp.ipc + r->buffers + (uint32_t)wp * r->size, b, MIN(n, r->size));
	ipc_wr32(r->size_v + 4u * wp, word);
	ipc_wr16(r->wp, (uint16_t)((wp + 1) % r->len));
	return true;
}

/* the answers the driver put in the RFS uplink ring: count, and the last one's fields */
struct answer {
	uint32_t len;
	uint8_t cmd, id, ok;
	uint32_t offset, length;
	uint8_t data[2048];
};

static int
take_answers(struct answer *last)
{
	struct ring *r = &cp.ul[RING_RFS];
	uint16_t wp = ipc_rd16(r->wp), rp = ipc_rd16(r->rp);
	int n = 0;

	if (wp >= r->len || rp >= r->len) {
		ipc_wr16(r->wp, 0);             /* (bent by the test) */
		ipc_wr16(r->rp, 0);
		return 0;
	}
	while (rp != wp) {
		const uint8_t *f = cp.ipc + r->buffers + (uint32_t)rp * r->size;
		uint32_t size = ipc_rd32(r->size_v + 4u * rp) & 0xffff;

		if (last != NULL) {
			CHECK(size >= 10 && size <= r->size && f[0] == 0xf8 && f[1] == CH_RFS);
		}
		if (last != NULL && size >= 10 && size <= r->size) {
			memset(last, 0, sizeof(*last));
			last->len = get32(f + 4);
			last->cmd = f[8];
			last->id = f[9];
			if (size >= 19) {
				last->ok = f[10];
				last->offset = get32(f + 11);
				last->length = get32(f + 15);
				memcpy(last->data, f + 19, size - 19);
			}
		}
		rp = (uint16_t)((rp + 1) % r->len);
		n++;
	}
	ipc_wr16(r->rp, rp);
	return n;
}

#define NV_BYTES 0x10000u

static uint8_t nv_expect[NV_BYTES];

static void
nv_setup(void)
{
	cp.nv_ram = calloc(1, NV_BYTES);
	cp.nv_ram_size = NV_BYTES;
	for (uint32_t i = 0; i < NV_BYTES; i++) {
		cp.nv_ram[i] = nv_expect[i] = (uint8_t)(i * 7 + 3);
	}
}

static void
nv_done(void)
{
	free(cp.nv_ram);
	cp.nv_ram = NULL;
	cp.nv_ram_size = 0;
}

/* an RFS request: its 6-byte header (length, command, id), then body */
static uint32_t
rfs_request(uint8_t *out, uint32_t len, uint8_t cmd, uint8_t id, const uint8_t *body, uint32_t n)
{
	put32(out, len);
	out[4] = cmd;
	out[5] = id;
	memcpy(out + 6, body, n);
	return 6 + n;
}

/* an NV write request of `length` bytes of `fill` at offset, the request's length word `len` (14 + length when right) */
static uint32_t
nv_write_request(uint8_t *out, uint32_t len, uint8_t id, uint32_t offset, uint32_t length, uint8_t fill, uint32_t carried)
{
	uint8_t body[8];

	put32(body, offset);
	put32(body + 4, length);
	rfs_request(out, len, 2, id, body, 8);
	memset(out + 14, fill, carried);
	return 14 + carried;
}

/* how the link carries a request of n bytes: 0 one frame, 1 a frame longer than a buffer, 2 a multi-frame message,
 * 3 a first frame and unmarked frames after it */
static void
send_rfs(const uint8_t *req, uint32_t n, int how)
{
	uint8_t f[4096];
	uint32_t size = cp.dl[RING_RFS].size;

	if (how == 0) {
		f[0] = 0xf8;
		f[1] = CH_RFS;
		f[2] = (uint8_t)(n + 4);
		f[3] = (uint8_t)((n + 4) >> 8);
		memcpy(f + 4, req, n);
		CHECK(n + 4 <= size && put_dl(RING_RFS, f, n + 4, n + 4));
	} else if (how == 1) {
		uint32_t total = n + 6, first = MIN(total, size);

		f[0] = 0xfa;
		f[1] = CH_RFS;
		put32(f + 2, total);
		memcpy(f + 6, req, first - 6);
		CHECK(put_dl(RING_RFS, f, first, first));
		for (uint32_t at = first - 6; at < n;) {
			uint32_t k = MIN(n - at, size);

			CHECK(put_dl(RING_RFS, req + at, k, k));
			at += k;
		}
	} else {
		uint32_t chunk = size - 5;

		for (uint32_t at = 0; at < n;) {
			uint32_t k = MIN(n - at, how == 2 ? chunk : size - 4);

			if (how == 2) {
				f[0] = 0xfb;
				f[1] = CH_RFS;
				f[2] = (uint8_t)(k + 5);
				f[3] = (uint8_t)((k + 5) >> 8);
				f[4] = at + k < n ? 0x80 : 0;
				memcpy(f + 5, req + at, k);
				CHECK(put_dl(RING_RFS, f, k + 5, k + 5));
			} else {
				f[0] = 0xf8;
				f[1] = CH_RFS;
				f[2] = (uint8_t)(k + 4);
				f[3] = (uint8_t)((k + 4) >> 8);
				memcpy(f + 4, req + at, k);
				CHECK(put_dl(RING_RFS, f, k + 4, k + 4));
			}
			at += k;
		}
	}
	rings_receive();
}

static void
nv_read_answered(uint32_t offset, uint32_t length)
{
	uint8_t req[64], body[8];
	struct answer a;
	uint32_t n;

	put32(body, offset);
	put32(body + 4, length);
	n = rfs_request(req, 14, 1, 0x42, body, 8);
	send_rfs(req, n, 0);
	CHECK(take_answers(&a) == 1);
	CHECK(a.cmd == 1 && a.id == 0x42 && a.ok == 1 && a.offset == offset && a.length == length && a.len == 6 + 9 + length);
	CHECK(memcmp(a.data, nv_expect + offset, length) == 0);
}

static void
rfs_valid(void)
{
	static uint8_t req[8192];
	struct answer a;

	for (int how = 0; how < 4; how++) {
		static const uint32_t lengths[] = { 0, 1, 100, 2000, 2034, 2035, 4000, 6000 };

		for (unsigned k = 0; k < sizeof(lengths) / sizeof(lengths[0]); k++) {
			uint32_t length = lengths[k], offset = 0x123 + k * 0x800, n;

			if (how == 0 && 14 + length + 4 > 2048) {
				continue;
			}
			link_up(4);
			nv_setup();
			n = nv_write_request(req, 14 + length, (uint8_t)k, offset, length, (uint8_t)(0xa0 + k), length);
			send_rfs(req, n, how);
			memset(nv_expect + offset, 0xa0 + k, length);
			CHECK(take_answers(&a) == 1);
			CHECK(a.cmd == 2 && a.id == (uint8_t)k && a.ok == 1 && a.offset == offset && a.length == length);
			CHECK(memcmp(cp.nv_ram, nv_expect, NV_BYTES) == 0);
			CHECK(!cp.w_active && cp.rfs_more == 0 && cp.more[RING_RFS] == 0);
			nv_read_answered(offset, MIN(length, 64));
			nv_read_answered(0, 2029);
			nv_done();
			for (uint32_t i = 0; i < NV_BYTES; i++) {
				nv_expect[i] = (uint8_t)(i * 7 + 3);
			}
		}
	}
	/* reads: at the end of the copy, past it, and too long for one frame */
	link_up(4);
	nv_setup();
	nv_read_answered(NV_BYTES - 16, 16);
	{
		uint8_t r[32], body[8];

		put32(body, NV_BYTES - 15);
		put32(body + 4, 16);
		send_rfs(r, rfs_request(r, 14, 1, 7, body, 8), 0);
		CHECK(take_answers(&a) == 1 && a.cmd == 1 && a.ok == 0);
		put32(body, 0);
		put32(body + 4, 2030);
		send_rfs(r, rfs_request(r, 14, 1, 7, body, 8), 0);
		CHECK(take_answers(&a) == 1 && a.cmd == 1 && a.ok == 0);
		put32(body, 0xfffffff0u);
		put32(body + 4, 0x20);
		send_rfs(r, rfs_request(r, 14, 1, 7, body, 8), 0);
		CHECK(take_answers(&a) == 1 && a.cmd == 1 && a.ok == 0);
		/* a write out of the copy: refused, nothing kept */
		send_rfs(r, nv_write_request(r, 14 + 8, 9, NV_BYTES - 4, 8, 0x55, 8), 0);
		CHECK(take_answers(&a) == 1 && a.cmd == 2 && a.ok == 0);
		CHECK(memcmp(cp.nv_ram, nv_expect, NV_BYTES) == 0);
	}
	nv_done();
}

/*
 * The case the 17 November pass noted: an NV write whose request is shorter than the data it names. Before the fix the
 * write stayed open after its request had ended, and the bytes of a later long frame on the ring went into the NV copy
 * at its offset; and a long frame's rest was counted twice (cp.more[] and cp.rfs_more), so the request after it was
 * taken for data and never answered.
 */
static void
rfs_short_write(void)
{
	static uint8_t req[8192];
	struct answer a;
	uint8_t body[8];
	uint32_t n;

	for (int how = 0; how < 4; how++) {
		link_up(4);
		nv_setup();
		/* the request says 14 + 10 bytes, the write 100 */
		n = nv_write_request(req, 14 + 10, 1, 0x200, 100, 0x11, 10);
		send_rfs(req, n, 0);
		(void)take_answers(&a);
		CHECK(!cp.w_active && cp.rfs_more == 0);
		/* a long request that is no NV write (a file open with 3000 bytes in it), as the link may bring it */
		memset(body, 0, sizeof(body));
		n = rfs_request(req, 6 + 3000, 0x11, 2, body, 0);
		memset(req + 6, 0xee, 3000);
		send_rfs(req, 6 + 3000, how == 0 ? 1 : how);
		(void)take_answers(NULL);
		CHECK(memcmp(cp.nv_ram + 0x200 + 10, nv_expect + 0x200 + 10, 90) == 0);
		CHECK(!cp.w_active && cp.rfs_more == 0);
		/* and the next request is answered */
		put32(body, 0);
		put32(body + 4, 4);
		send_rfs(req, rfs_request(req, 14, 1, 3, body, 8), 0);
		CHECK(take_answers(&a) == 1 && a.cmd == 1 && a.id == 3 && a.ok == 1);
		nv_done();
	}
	/* a write whose request carries all its data is kept whole when the request also carries more after it */
	link_up(4);
	nv_setup();
	n = nv_write_request(req, 14 + 40 + 20, 4, 0x300, 40, 0x22, 60);
	send_rfs(req, n, 0);
	memset(nv_expect + 0x300, 0x22, 40);
	CHECK(take_answers(&a) == 1 && a.cmd == 2 && a.ok == 1 && a.length == 40);
	CHECK(memcmp(cp.nv_ram, nv_expect, NV_BYTES) == 0);
	nv_read_answered(0x300, 40);
	nv_done();
	for (uint32_t i = 0; i < NV_BYTES; i++) {
		nv_expect[i] = (uint8_t)(i * 7 + 3);
	}
}

/* every control message kind the driver reads, at every length from 0 to 200 bytes, with random bytes */
static void
fmt_lengths(void)
{
	static const uint8_t kinds[][3] = {
		{ 0x0a, 0x01, 2 }, { 0x08, 0x07, 2 }, { 0x80, 0x00, 2 }, { 0x0d, 0x10, 3 }, { 0x0d, 0x09, 3 }, { 0x0d, 0x01, 2 },
		{ 0x01, 0x07, 3 }, { 0x01, 0x02, 3 }, { 0x05, 0x01, 3 }, { 0x05, 0x07, 3 }, { 0x05, 0x05, 2 }, { 0x08, 0x05, 3 },
		{ 0x08, 0x03, 3 }, { 0x07, 0x01, 3 }, { 0x07, 0x06, 3 }, { 0x42, 0x42, 1 },
	};

	link_up(7);
	cp.data_step = 1;
	cp.sim_asked = true;
	for (unsigned k = 0; k < sizeof(kinds) / sizeof(kinds[0]); k++) {
		for (uint32_t n = 0; n <= 200; n++) {
			uint8_t *m = malloc(n ? n : 1);

			for (uint32_t i = 0; i < n; i++) {
				m[i] = (uint8_t)rnd();
			}
			if (n >= 7) {
				m[4] = kinds[k][0];
				m[5] = kinds[k][1];
				m[6] = kinds[k][2];
				if (k == 12 && n >= 20) {
					memcpy(m + 9, "2720312345", 10);          /* a run of digits: the operator code is its first 6 */
				}
			}
			fmt_received(m, n);
			free(m);
			CHECK(memchr(cp.oper, 0, sizeof(cp.oper)) != NULL);
		}
	}
	CHECK(strcmp(cp.oper, "272031") == 0 || cp.oper[0] != 0);
	{
		struct sysctl_req req = { 0 };

		CHECK(cp_info_sysctl(NULL, NULL, 0, &req) == 0 && req.outlen > 0 && req.out[req.outlen - 1] == 0);
		CHECK(cp_addr_sysctl(NULL, NULL, 0, &req) == 0 && req.out[req.outlen - 1] == 0);
		is_root = false;
		CHECK(cp_addr_sysctl(NULL, NULL, 0, &req) == EPERM);
		is_root = true;
	}
}

/* an IPv4/UDP packet to our address, checksums right */
static uint32_t
udp_packet(uint8_t *p, uint32_t payload)
{
	uint32_t total = 20 + 8 + payload;
	uint16_t s;

	memset(p, 0, total);
	p[0] = 0x45;
	p[2] = (uint8_t)(total >> 8);
	p[3] = (uint8_t)total;
	p[8] = 64;
	p[9] = 17;
	memcpy(p + 12, cp.dns4, 4);
	memcpy(p + 16, cp.ip4, 4);
	s = ip_sum(p, 20, 0);
	p[10] = (uint8_t)(s >> 8);
	p[11] = (uint8_t)s;
	p[21] = 53;
	p[22] = (uint8_t)(cp.dns_port >> 8);
	p[23] = (uint8_t)cp.dns_port;
	p[24] = (uint8_t)((8 + payload) >> 8);
	p[25] = (uint8_t)(8 + payload);
	for (uint32_t i = 0; i < payload; i++) {
		p[28 + i] = (uint8_t)i;
	}
	return total;
}

static void
data_path(uint32_t iterations)
{
	static uint8_t pkt[4096];
	uint64_t before;

	link_up(7);
	pdp.ifp = (ifnet_t)(uintptr_t)1;
	cp.if_kb = 1u << 20;
	cp.have4 = true;
	memcpy(cp.ip4, (const uint8_t[4]){ 10, 1, 2, 3 }, 4);
	memcpy(cp.dns4, (const uint8_t[4]){ 10, 9, 9, 9 }, 4);
	before = stack_packets;
	for (uint32_t n = 0; n <= 2048; n += 1 + n / 8) {
		uint32_t t = udp_packet(pkt, n > 28 ? n - 28 : 0);

		data_received(10, pkt, t);
	}
	CHECK(stack_packets > before);
	/* mutated packets, through the ordinary data ring and the zero-copy slots */
	for (uint32_t it = 0; it < iterations; it++) {
		uint32_t t = udp_packet(pkt, rnd() % 1900), flips = rnd() % 6;

		for (uint32_t f = 0; f < flips; f++) {
			pkt[rnd() % (t + 8)] = (uint8_t)rnd();
		}
		if (rnd() % 4 == 0) {
			t = rnd() % 2100;
		}
		switch (it % 3) {
		case 0:
			data_received((uint8_t)rnd(), pkt, t ? MIN(t, 2048) : 1);
			break;
		case 1:
			(void)put_dl(RING_HIPRIO, pkt, t, (rnd() % 3 == 0 ? rnd() : t) | (rnd() & 0xffff0000u));
			rings_receive();
			break;
		default: {
			struct ring *r = &cp.dl[RING_DATA];
			uint16_t slot = cp.zc_pre;

			if (rnd() % 8 == 0) {
				ipc_wr16(r->wp, (uint16_t)rnd());
			} else {
				memcpy(cp.ipc + zc_offset(slot), pkt, MIN(t, ZMB_CELL - ZMB_HEADROOM));
				ipc_wr32(r->size_v + 4u * slot, rnd() % 8 == 0 ? rnd() : t);
				if (rnd() % 16 == 0) {
					put32(cp.ipc + r->buffers + slot * 8u, rnd());
				}
				ipc_wr16(r->wp, (uint16_t)((slot + 1) % r->len));
			}
			zc_receive();
		}
		}
	}
	pdp.ifp = NULL;
}

/* random frames on every ring, every size word, every counter */
static void
rings_noise(uint32_t iterations)
{
	static uint8_t f[4096];

	link_up(7);
	nv_setup();
	cp.rfs_open = 1;
	for (uint32_t it = 0; it < iterations; it++) {
		uint32_t i = rnd() % NRINGS, n = rnd() % 4200;

		if (rnd() % 64 == 0) {
			nv_done();
			link_up(7);
			nv_setup();
			cp.rfs_open = 1;
		}
		for (uint32_t k = 0; k < MIN(n, sizeof(f)); k++) {
			f[k] = (uint8_t)rnd();
		}
		if (rnd() % 2) {
			f[0] = (uint8_t)(0xf8 | (rnd() & 3));
			f[1] = (uint8_t)ring_cfgs[i].ch;
			if (rnd() % 2) {
				put32(f + 2, rnd() % 6000);
			}
			if (i == RING_RFS && rnd() % 2) {
				uint32_t hdr = (f[0] & 2) ? ((f[0] & 3) == 3 ? 5 : 6) : 4;

				put32(f + hdr, rnd() % 5000);
				f[hdr + 4] = (uint8_t)(1 + rnd() % 2);
				put32(f + hdr + 6, rnd() % (NV_BYTES + 64));
				put32(f + hdr + 10, rnd() % 3000);
			}
		}
		if (rnd() % 32 == 0) {
			ipc_wr16(cp.dl[i].wp, (uint16_t)rnd());
		} else if (rnd() % 32 == 0) {
			ipc_wr16(cp.dl[i].rp, (uint16_t)rnd());
		} else {
			(void)put_dl(i, f, n, rnd() % 8 == 0 ? rnd() : n);
		}
		if (rnd() % 64 == 0) {
			ipc_wr16(cp.ul[RING_RFS].rp, (uint16_t)rnd());
		}
		rings_receive();
		(void)take_answers(NULL);
		if (cp.w_active) {
			CHECK(cp.w_off <= cp.nv_ram_size && cp.w_len <= cp.nv_ram_size - cp.w_off && cp.w_got <= cp.w_len);
		}
	}
	nv_done();
}

/* the boot loader's queue: head and tail words bent */
static void
boot_queue(uint32_t iterations)
{
	link_up(3);
	for (uint32_t it = 0; it < iterations; it++) {
		uint32_t word = 0;

		ipc_wr32(IPC_RAW_TX_HEAD, rnd() % 8 ? rnd() % IPC_RAW_TX_BYTES : rnd());
		ipc_wr32(IPC_RAW_TX_TAIL, rnd() % 8 ? rnd() % IPC_RAW_TX_BYTES : rnd());
		(void)boot_send(rnd());
		ipc_wr32(IPC_RAW_RX_HEAD, rnd() % 8 ? rnd() % IPC_RAW_RX_BYTES : rnd());
		ipc_wr32(IPC_RAW_RX_TAIL, rnd() % 8 ? rnd() % IPC_RAW_RX_BYTES : rnd());
		(void)boot_receive(&word, 1);
	}
}

/* RADIO with a table of contents the driver knows: TOC, BOOT (with its memory table), MAIN, VSS, NV */
static void
radio_make(void)
{
	static const struct { const char *name; uint32_t offset, address, size; } e[] = {
		{ "TOC", 0, 0, 0x400 }, { "BOOT", 0x400, 0x40000000, 0x3000 }, { "MAIN", 0x4000, 0x40010000, 0x8000 },
		{ "VSS", 0xc000, 0x47900000, 0x1000 }, { "NV", 0, 0x44900000, 0x2000 },
	};
	const uint32_t table = 0x400 + 0xa0;

	memset(radio, 0, radio_size);
	for (uint32_t i = 1; i < 4; i++) {
		for (uint32_t k = 0; k < e[i].size; k++) {
			radio[e[i].offset + k] = (uint8_t)(k ^ i);
		}
	}
	/* BOOT's memory table: "MEM", count 2: the IPC memory where the driver knows it, and one other */
	put32(radio + table, 0x4d454d00u);
	put32(radio + table + 12, 2);
	put32(radio + table + 16, 0x495043);
	put32(radio + table + 20, CP_IPC_OFFSET);
	put32(radio + table + 24, CP_IPC_BYTES);
	put32(radio + table + 28, 0x534853);
	for (uint32_t i = 0; i < sizeof(e) / sizeof(e[0]); i++) {
		struct toc_entry t = { { 0 }, e[i].offset, e[i].address, e[i].size, 0, 0 };

		strlcpy(t.name, e[i].name, sizeof(t.name));
		if (i > 0 && i < 4) {
			t.crc = crc32_of(radio + e[i].offset, e[i].size);
		}
		memcpy(radio + 32 * i, &t, sizeof(t));
	}
}

static void
radio_toc(uint32_t iterations)
{
	uint8_t *mem = malloc(CP_MEM_BYTES);         /* the CP's memory: a copy past it is the sanitizer's */
	uint32_t passed = 0;

	radio_size = 0x10000;
	radio = malloc(radio_size);
	memset(&cp, 0, sizeof(cp));
	cp.mem = (vm_offset_t)mem;
	radio_make();
	CHECK(read_toc() && cp.mem_table && cp.boot != NULL && cp.main != NULL && cp.nv != NULL && cp.vss != NULL);
	CHECK(copy_entry(cp.boot) && copy_entry(cp.main) && fill_nv());
	for (uint32_t it = 0; it < iterations; it++) {
		uint32_t flips = 1 + rnd() % 4;

		radio_make();
		for (uint32_t f = 0; f < flips; f++) {
			uint32_t at = rnd() % (32 * 6);

			if (rnd() % 2) {
				put32(radio + (at & ~3u), rnd() % 3 ? rnd() : rnd() % 0x20000);
			} else {
				radio[at] = (uint8_t)rnd();
			}
		}
		if (rnd() % 4 == 0) {
			put32(radio + 0x400 + 0xa0 + 12, rnd() % 8);
		}
		memset(&cp, 0, sizeof(cp));
		cp.mem = (vm_offset_t)mem;
		if (!read_toc()) {
			continue;
		}
		passed++;
		/* what load_and_check() then does with a table that passed: the copies and the NV place */
		(void)copy_entry(cp.boot);
		(void)copy_entry(cp.main);
		if (cp.vss != NULL && !(cp.vss->address - cp.boot->address + (uint64_t)cp.vss->size > CP_IPC_OFFSET ||
		    cp.vss->address < cp.main->address + cp.main->size)) {
			(void)copy_entry(cp.vss);
		}
		cp.nv_file = rnd() % 2;
		nv_file_size = cp.nv != NULL ? cp.nv->size : 0;
		nv_file = nv_file_size <= 0x10000 ? radio : NULL;
		(void)fill_nv();
	}
	CHECK(passed > 0);
	free(radio);
	radio = NULL;
	free(mem);
	memset(&cp, 0, sizeof(cp));
}

int
main(int argc, char **argv)
{
	uint32_t iterations = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 300000;

	quiet = getenv("SHANNON_TEST_VERBOSE") == NULL;
	ipc_buf = malloc(IPC_ALL);
	rfs_valid();
	rfs_short_write();
	fmt_lengths();
	data_path(iterations);
	rings_noise(iterations);
	boot_queue(iterations / 10);
	radio_toc(iterations / 3);
	free(ipc_buf);
	printf("shannon_test: %u checks, %u failed (%u mutated inputs a kind)\n", checks, failures, iterations);
	return failures != 0;
}
