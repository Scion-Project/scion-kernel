/*
 * A virtio network device as the link behind en0 (see sys/hakios.net.h for what a link is).
 *
 * Any transport of sys/hakios.virtio.h: "-device virtio-net-device" on QEMU virt's virtio-mmio
 * bus (-global virtio-mmio.force-legacy=false, boot-arg virtio_net=1, which turns that bus on;
 * qemu/run-darwin.sh does both with QEMU_NET=1), or a virtio-pci network device (QEMU's
 * virtio-net-pci, crosvm's), used unless boot-arg virtio_net=0.
 *
 * Nothing of what the device offers is taken (no checksums or segmentation left to it, no
 * merged receive buffers): a 12-byte header nobody reads, then the frame, one buffer each.
 * No interrupt is used; the interface's thread asks (virtio_poll()).
 *
 * Boot-arg virtio_net_wifi=1: iOS is told that en0 is a radio, the way the phone's is
 * (hakios.net.c, hakios.wlan.cpp, hakios.apple80211.c), and a station of this file's stands
 * behind it (the station boundary, sys/hakios.sta.h). It hears one open network, "Scion" (or
 * virtio_net_ssid=NAME), joins it at the start and whenever iOS asks, and then lets frames
 * pass. So Settings shows Wi-Fi on and that network joined, configd configures the interface
 * by itself (a "Wi-Fi" network service, DHCP), and the status bar has the Wi-Fi bars.
 *
 * Boot-arg virtio_net_vsock=PORT: the link is a stream on the virtio socket device instead (AVF
 * gives every guest one; Scion's "Network" setting, since the phone's crosvm can't be given a
 * network card). The guest listens on PORT; the Scion app connects (VirtualMachine.connectVsock)
 * and runs a user-mode network stack on its end (libslirp: DHCP, DNS and NAT through the app's
 * own sockets). One structure carries the frames, both ways:
 *
 *     u32 length, big-endian (1..HAKIOS_NET_FRAME_MAX), then that many bytes: the Ethernet frame
 *
 * repeated for as long as the connection lasts (QEMU's "-netdev stream" framing). The stream
 * cuts the bytes into packets as it likes, so the receiver puts frames back together; anything
 * else (a length of 0 or over the maximum) ends the connection. A new connection replaces the
 * old one: the app connects again whenever it has lost the stream. No interrupt either: the
 * interface's thread asks (vsock_poll()). Virtio 1.2, section 5.10, for the socket device.
 */
#include <kern/locks.h>
#include <kern/thread.h>
#include <libkern/libkern.h>
#include <mach/mach_types.h>
#include <mach/vm_param.h>
#include <net/ethernet.h>
#include <pexpert/pexpert.h>
#include <sys/kpi_mbuf.h>
#include <sys/errno.h>
#include <sys/hakios.net.h>
#include <sys/param.h>
#include <sys/hakios.sta.h>
#include <sys/hakios.virtio.h>
#include <sys/systm.h>

#define VIRTIO_NET_F_MAC        5               /* the device has an address of its own */
#define CFG_MAC                 0x00

#define QUEUE_SIZE              64              /* descriptors, and buffers, per queue */
#define QUEUE_RECEIVE           0
#define QUEUE_TRANSMIT          1
#define BUFFER_SIZE             2048            /* a buffer never crosses a page: one address will do for it */

/* before every frame, either way; nothing of it is used (no checksum or segmentation is left to the device) */
struct virtio_net_header {
	uint8_t flags, gso_type;
	uint16_t header_length, gso_size, checksum_start, checksum_offset, buffers;
};

struct queue {
	vio_queue_t *           vq;
	uint8_t *               buffers;        /* QUEUE_SIZE of BUFFER_SIZE */
	uint64_t                buffers_pa;
	uint16_t                free[QUEUE_SIZE];       /* transmit: the buffers the device does not have */
	unsigned                free_count;
};

static vio_dev_t *net_dev;
static struct queue receive_queue, transmit_queue;

/* cookies are buffer number + 1 (never NULL) */
#define COOKIE(i)               ((void *)(uintptr_t)((i) + 1))
#define BUFFER_OF(c)            ((uint16_t)((uintptr_t)(c) - 1))

static void
give(struct queue *q, uint16_t id, uint32_t len, bool device_writes)
{
	struct vio_sg sg = { q->buffers_pa + (uint64_t)id * BUFFER_SIZE, len, device_writes };

	(void)vio_queue_add(q->vq, &sg, 1, COOKIE(id));
}

static bool
queue_make(struct queue *q, uint16_t index, bool device_writes)
{
	int error;

	if ((error = vio_queue_setup(net_dev, index, QUEUE_SIZE, &q->vq)) != 0) {
		printf("net: virtio queue %u: %d\n", index, error);
		return false;
	}
	if ((q->buffers = vio_dma_alloc(QUEUE_SIZE * BUFFER_SIZE, &q->buffers_pa)) == NULL) {
		return false;
	}
	for (uint16_t i = 0; i < vio_queue_size(q->vq); i++) {
		if (device_writes) {
			give(q, i, BUFFER_SIZE, true);          /* the device gets them all, to fill */
		} else {
			q->free[q->free_count++] = i;
		}
	}
	return true;
}

/* A frame to send. */
static bool
virtio_transmit(mbuf_t packet, size_t length)
{
	struct queue *q = &transmit_queue;
	uint8_t *buffer;
	uint16_t id;
	void *done;

	/* the buffers the device has done with */
	while ((done = vio_queue_get(q->vq, NULL)) != NULL) {
		q->free[q->free_count++] = BUFFER_OF(done);
	}
	if (q->free_count == 0 || length > BUFFER_SIZE - sizeof(struct virtio_net_header)) {
		return false;
	}
	id = q->free[--q->free_count];
	buffer = q->buffers + (size_t)id * BUFFER_SIZE;
	bzero(buffer, sizeof(struct virtio_net_header));
	if (mbuf_copydata(packet, 0, length, buffer + sizeof(struct virtio_net_header)) != 0) {
		q->free[q->free_count++] = id;
		return false;
	}
	give(q, id, (uint32_t)(sizeof(struct virtio_net_header) + length), false);
	vio_queue_kick(q->vq);
	return true;
}

/* Hand up what has arrived; returns how many frames that was. */
static unsigned
virtio_poll(void)
{
	struct queue *q = &receive_queue;
	unsigned frames = 0;
	uint32_t length;
	void *done;

	while ((done = vio_queue_get(q->vq, &length)) != NULL) {
		uint16_t id = BUFFER_OF(done);

		if (length > sizeof(struct virtio_net_header) && length <= BUFFER_SIZE) {
			hakios_net_input(q->buffers + (size_t)id * BUFFER_SIZE + sizeof(struct virtio_net_header),
			    length - sizeof(struct virtio_net_header));
		}
		give(q, id, BUFFER_SIZE, true);         /* the buffer goes back */
		frames++;
	}
	if (frames) {
		vio_queue_kick(q->vq);
	}
	return frames;
}

static bool
virtio_net_start(uint8_t address[ETHER_ADDR_LEN])
{
	uint64_t offered, features;
	int error;

	if ((net_dev = vio_find(VIRTIO_ID_NET, 0)) == NULL) {
		printf("net: no virtio network device (QEMU: -device virtio-net-pci or virtio-net-device)\n");
		return false;
	}
	if ((error = vio_init(net_dev, VIRTIO_BIT(VIRTIO_NET_F_MAC), &features)) != 0) {
		printf("net: %s: features refused (the device offers %#llx)\n", vio_name(net_dev), vio_device_features(net_dev));
		return false;
	}
	/* (read after vio_init: before it, a PCI device's registers may not be reachable yet, and read as all ones) */
	offered = vio_device_features(net_dev);
	if (!queue_make(&receive_queue, QUEUE_RECEIVE, true) || !queue_make(&transmit_queue, QUEUE_TRANSMIT, false)) {
		vio_fail(net_dev);
		return false;
	}
	if (features & VIRTIO_BIT(VIRTIO_NET_F_MAC)) {
		vio_cfg_read(net_dev, CFG_MAC, address, ETHER_ADDR_LEN);
	}
	vio_driver_ok(net_dev);
	vio_queue_kick(receive_queue.vq);
	printf("net: virtio network device on %s (it offers features %#llx)\n", vio_name(net_dev), offered);
	return true;
}

static struct hakios_net_link virtio_link = {
	.name = "a virtio network device",
	.address = { 0x02, 'r', 'e', 'd', 'w', 'd' },   /* if the device has none: one "locally administered" */
	.up = true,
	.transmit = virtio_transmit,
	.poll = virtio_poll,
};

/* ---- virtio_net_vsock=PORT: the frames on a stream of the virtio socket device ---------------------------- */

#define VSOCK_QUEUE_RECEIVE     0
#define VSOCK_QUEUE_TRANSMIT    1
#define VSOCK_QUEUE_EVENT       2
#define VSOCK_QUEUE_SIZE        64              /* descriptors, and buffers, per queue */
#define VSOCK_BUFFER            4096            /* one packet: its header and up to 4052 bytes of the stream; a page */
#define VSOCK_EVENTS            4
#define VSOCK_EVENT_SIZE        8
#define VSOCK_CFG_GUEST_CID     0x00
#define VSOCK_TYPE_STREAM       1
#define VSOCK_OP_REQUEST        1
#define VSOCK_OP_RESPONSE       2
#define VSOCK_OP_RST            3
#define VSOCK_OP_SHUTDOWN       4
#define VSOCK_OP_RW             5
#define VSOCK_OP_CREDIT_UPDATE  6
#define VSOCK_OP_CREDIT_REQUEST 7
#define VSOCK_EVENT_TRANSPORT_RESET 0
/* the room the peer is told the guest has: what arrives is handed up at once, so only a limit on what is in flight */
#define VSOCK_BUF_ALLOC         (256 * 1024)
#define VSOCK_PREFIX            4               /* the frame's length, before each frame */

/* before every packet, either way (little-endian, as the CPU) */
struct vsock_header {
	uint64_t src_cid, dst_cid;
	uint32_t src_port, dst_port, len;
	uint16_t type, op;
	uint32_t flags, buf_alloc, fwd_cnt;
} __attribute__((packed));

struct vsock_queue {
	vio_queue_t *           vq;
	uint8_t *               buffers;        /* VSOCK_QUEUE_SIZE of VSOCK_BUFFER */
	uint64_t                buffers_pa;
	uint16_t                free[VSOCK_QUEUE_SIZE];       /* transmit: the buffers the device does not have */
	unsigned                free_count;
};

static LCK_GRP_DECLARE(vsock_lck_grp, "hakios-vsock-net");
/* the transmit queue and the connection: transmit (the stack's threads) and poll (the interface's) both send */
static LCK_MTX_DECLARE(vsock_lock, &vsock_lck_grp);

static struct {
	vio_dev_t *             dev;
	struct vsock_queue      rx, tx;
	vio_queue_t *           events;
	uint8_t *               event_buffers;
	uint64_t                event_pa;
	uint64_t                cid;            /* the guest's */
	uint32_t                port;           /* the one listened on */
	bool                    connected;
	uint64_t                peer_cid;
	uint32_t                peer_port;
	uint32_t                peer_buf_alloc, peer_fwd_cnt;   /* the peer's room, as it last said */
	uint32_t                sent;           /* stream bytes sent on this connection */
	uint32_t                received;       /* stream bytes received, all handed on at once */
	uint32_t                received_said;  /* "received" as the peer was last told it */
	bool                    credit_asked;
	uint8_t                 frame[VSOCK_PREFIX + HAKIOS_NET_FRAME_MAX];     /* the frame being put together */
	uint32_t                have;
	uint64_t                connections, no_room, no_app;
} vs;

static void
vsock_give(struct vsock_queue *q, uint16_t id, uint32_t len, bool device_writes)
{
	struct vio_sg sg = { q->buffers_pa + (uint64_t)id * VSOCK_BUFFER, len, device_writes };

	(void)vio_queue_add(q->vq, &sg, 1, COOKIE(id));
}

static void
vsock_give_event(uint16_t id)
{
	struct vio_sg sg = { vs.event_pa + (uint64_t)id * VSOCK_EVENT_SIZE, VSOCK_EVENT_SIZE, true };

	(void)vio_queue_add(vs.events, &sg, 1, COOKIE(id));
}

static bool
vsock_queue_make(struct vsock_queue *q, uint16_t index, bool device_writes)
{
	int error;

	if ((error = vio_queue_setup(vs.dev, index, VSOCK_QUEUE_SIZE, &q->vq)) != 0) {
		printf("net: virtio socket queue %u: %d\n", index, error);
		return false;
	}
	if ((q->buffers = vio_dma_alloc(VSOCK_QUEUE_SIZE * VSOCK_BUFFER, &q->buffers_pa)) == NULL) {
		return false;
	}
	for (uint16_t i = 0; i < vio_queue_size(q->vq); i++) {
		if (device_writes) {
			vsock_give(q, i, VSOCK_BUFFER, true);
		} else {
			q->free[q->free_count++] = i;
		}
	}
	return true;
}

/*
 * One packet from port `from` to (cid, port), with vsock_lock held: `head_length` bytes of `head`, then `length`
 * bytes of `packet` (if not NULL) after the header. False if no buffer was free.
 */
static bool
vsock_send(uint16_t op, uint32_t from, uint64_t cid, uint32_t port, const void *head, size_t head_length,
    mbuf_t packet, size_t length)
{
	struct vsock_queue *q = &vs.tx;
	struct vsock_header h = {
		.src_cid = vs.cid, .dst_cid = cid, .src_port = from, .dst_port = port,
		.len = (uint32_t)(head_length + length), .type = VSOCK_TYPE_STREAM, .op = op,
		.buf_alloc = VSOCK_BUF_ALLOC, .fwd_cnt = vs.received,
	};
	uint8_t *buffer;
	uint16_t id;
	void *done;

	while ((done = vio_queue_get(q->vq, NULL)) != NULL) {
		q->free[q->free_count++] = BUFFER_OF(done);
	}
	if (q->free_count == 0 || sizeof(h) + head_length + length > VSOCK_BUFFER) {
		return false;
	}
	id = q->free[--q->free_count];
	buffer = q->buffers + (size_t)id * VSOCK_BUFFER;
	memcpy(buffer, &h, sizeof(h));
	if (head_length != 0) {
		memcpy(buffer + sizeof(h), head, head_length);
	}
	if (packet != NULL && mbuf_copydata(packet, 0, length, buffer + sizeof(h) + head_length) != 0) {
		q->free[q->free_count++] = id;
		return false;
	}
	vs.received_said = vs.received;
	vsock_give(q, id, (uint32_t)(sizeof(h) + head_length + length), false);
	vio_queue_kick(q->vq);
	return true;
}

/* A control packet on the connection (vsock_lock held). */
static void
vsock_tell(uint16_t op)
{
	(void)vsock_send(op, vs.port, vs.peer_cid, vs.peer_port, NULL, 0, NULL, 0);
}

/* The connection is over (vsock_lock held). */
static void
vsock_drop(const char *why)
{
	if (vs.connected) {
		printf("net: vsock: the app's stream on port %u ended: %s\n", vs.port, why);
	}
	vs.connected = false;
	vs.have = 0;
}

/* A frame to send: its length, then the frame, as one packet. */
static bool
vsock_transmit(mbuf_t packet, size_t length)
{
	uint8_t prefix[VSOCK_PREFIX] = { (uint8_t)(length >> 24), (uint8_t)(length >> 16), (uint8_t)(length >> 8), (uint8_t)length };
	bool sent = false;

	lck_mtx_lock(&vsock_lock);
	if (!vs.connected) {
		vs.no_app++;
	} else if ((int64_t)vs.peer_buf_alloc - (int64_t)(uint32_t)(vs.sent - vs.peer_fwd_cnt) < (int64_t)(VSOCK_PREFIX + length)) {
		vs.no_room++;           /* the app has not read what it has; dropped (TCP sends again), and ask once */
		if (!vs.credit_asked) {
			vsock_tell(VSOCK_OP_CREDIT_REQUEST);
			vs.credit_asked = true;
		}
	} else if ((sent = vsock_send(VSOCK_OP_RW, vs.port, vs.peer_cid, vs.peer_port, prefix, sizeof(prefix), packet, length))) {
		vs.sent += (uint32_t)(VSOCK_PREFIX + length);
	}
	lck_mtx_unlock(&vsock_lock);
	return sent;
}

/* Bytes of the stream, in order: the frames in them handed up. False if they are not frames. Poll's thread only. */
static bool
vsock_stream(const uint8_t *p, uint32_t n)
{
	while (n > 0) {
		uint32_t want = VSOCK_PREFIX, take;

		if (vs.have >= VSOCK_PREFIX) {
			uint32_t length = (uint32_t)vs.frame[0] << 24 | (uint32_t)vs.frame[1] << 16 | (uint32_t)vs.frame[2] << 8 | vs.frame[3];

			if (length == 0 || length > HAKIOS_NET_FRAME_MAX) {
				return false;
			}
			want = VSOCK_PREFIX + length;
		}
		take = MIN(n, want - vs.have);
		memcpy(vs.frame + vs.have, p, take);
		vs.have += take;
		p += take;
		n -= take;
		if (vs.have == want && want > VSOCK_PREFIX) {
			hakios_net_input(vs.frame + VSOCK_PREFIX, want - VSOCK_PREFIX);
			vs.have = 0;
		}
	}
	return true;
}

/* One packet from the device. Poll's thread only (the only one that changes the connection). */
static void
vsock_packet(const struct vsock_header *h, const uint8_t *payload, uint32_t length)
{
	bool ours = vs.connected && h->src_cid == vs.peer_cid && h->src_port == vs.peer_port;
	bool frames;

	lck_mtx_lock(&vsock_lock);
	if (h->dst_port != vs.port || h->type != VSOCK_TYPE_STREAM || (!ours && h->op != VSOCK_OP_REQUEST)) {
		if (h->op != VSOCK_OP_RST) {    /* nobody listens there, or a connection that is gone */
			(void)vsock_send(VSOCK_OP_RST, h->dst_port, h->src_cid, h->src_port, NULL, 0, NULL, 0);
		}
		lck_mtx_unlock(&vsock_lock);
		return;
	}
	if (h->op == VSOCK_OP_REQUEST) {
		if (vs.connected) {
			vsock_tell(VSOCK_OP_RST);
			vsock_drop("the app connected again");
		}
		vs.connected = true;
		vs.peer_cid = h->src_cid;
		vs.peer_port = h->src_port;
		vs.sent = vs.received = vs.received_said = 0;
		vs.have = 0;
		vs.connections++;
	}
	vs.peer_buf_alloc = h->buf_alloc;
	vs.peer_fwd_cnt = h->fwd_cnt;
	vs.credit_asked = false;
	switch (h->op) {
	case VSOCK_OP_REQUEST:
		vsock_tell(VSOCK_OP_RESPONSE);
		printf("net: vsock: the app connected from cid %llu port %u; frames pass on port %u\n", h->src_cid, h->src_port, vs.port);
		break;
	case VSOCK_OP_RW:
		vs.received += length;
		lck_mtx_unlock(&vsock_lock);
		frames = vsock_stream(payload, length);         /* (the stack is not entered with the lock held) */
		lck_mtx_lock(&vsock_lock);
		if (!frames && vs.connected) {
			vsock_tell(VSOCK_OP_RST);
			vsock_drop("what came was not frames");
		}
		break;
	case VSOCK_OP_CREDIT_REQUEST:
		vsock_tell(VSOCK_OP_CREDIT_UPDATE);
		break;
	case VSOCK_OP_SHUTDOWN:
		vsock_tell(VSOCK_OP_RST);
		vsock_drop("the app closed it");
		break;
	case VSOCK_OP_RST:
		vsock_drop("the app reset it");
		break;
	default:                        /* CREDIT_UPDATE, RESPONSE: only the room, taken above */
		break;
	}
	lck_mtx_unlock(&vsock_lock);
}

/* What has arrived: packets of the stream, and the device's events. Returns how many packets that was. */
static unsigned
vsock_poll(void)
{
	unsigned packets = 0;
	uint32_t length;
	void *done;

	while ((done = vio_queue_get(vs.rx.vq, &length)) != NULL) {
		uint16_t id = BUFFER_OF(done);
		const uint8_t *buffer = vs.rx.buffers + (size_t)id * VSOCK_BUFFER;
		struct vsock_header h;

		if (length >= sizeof(h) && length <= VSOCK_BUFFER) {
			memcpy(&h, buffer, sizeof(h));
			vsock_packet(&h, buffer + sizeof(h), MIN(h.len, length - (uint32_t)sizeof(h)));
		}
		vsock_give(&vs.rx, id, VSOCK_BUFFER, true);     /* the buffer goes back */
		packets++;
	}
	if (packets) {
		vio_queue_kick(vs.rx.vq);
	}
	while ((done = vio_queue_get(vs.events, NULL)) != NULL) {
		uint16_t id = BUFFER_OF(done);
		uint32_t event;

		memcpy(&event, vs.event_buffers + (size_t)id * VSOCK_EVENT_SIZE, sizeof(event));
		if (event == VSOCK_EVENT_TRANSPORT_RESET) {     /* (the VM was moved or restored: connections are gone) */
			lck_mtx_lock(&vsock_lock);
			vsock_drop("the transport was reset");
			vs.cid = vio_cfg_read64(vs.dev, VSOCK_CFG_GUEST_CID);
			lck_mtx_unlock(&vsock_lock);
		}
		vsock_give_event(id);
		vio_queue_kick(vs.events);
	}
	/* the peer learns of the room made with every packet sent; without traffic back, tell it now and then */
	lck_mtx_lock(&vsock_lock);
	if (vs.connected && vs.received - vs.received_said >= VSOCK_BUF_ALLOC / 4) {
		vsock_tell(VSOCK_OP_CREDIT_UPDATE);
	}
	lck_mtx_unlock(&vsock_lock);
	return packets;
}

static void
vsock_counts(char *line, size_t room)
{
	snprintf(line, room, "vsock port %u: %s, %llu connections; frames dropped: %llu with no room, %llu with no app",
	    vs.port, vs.connected ? "the app is connected" : "waiting for the app", vs.connections, vs.no_room, vs.no_app);
}

static bool
vsock_start(uint32_t port)
{
	int error;

	if ((vs.dev = vio_find(VIRTIO_ID_VSOCK, 0)) == NULL) {
		return false;
	}
	if ((error = vio_init(vs.dev, 0, NULL)) != 0) {
		printf("net: %s: the virtio socket device refused (it offers %#llx)\n", vio_name(vs.dev), vio_device_features(vs.dev));
		return false;
	}
	if (!vsock_queue_make(&vs.rx, VSOCK_QUEUE_RECEIVE, true) || !vsock_queue_make(&vs.tx, VSOCK_QUEUE_TRANSMIT, false) ||
	    vio_queue_setup(vs.dev, VSOCK_QUEUE_EVENT, VSOCK_EVENTS, &vs.events) != 0 ||
	    (vs.event_buffers = vio_dma_alloc(PAGE_SIZE, &vs.event_pa)) == NULL) {
		printf("net: %s: no queues for the virtio socket device\n", vio_name(vs.dev));
		vio_fail(vs.dev);
		return false;
	}
	for (uint16_t i = 0; i < vio_queue_size(vs.events); i++) {
		vsock_give_event(i);
	}
	vs.cid = vio_cfg_read64(vs.dev, VSOCK_CFG_GUEST_CID);
	vs.port = port;
	vio_driver_ok(vs.dev);
	vio_queue_kick(vs.rx.vq);
	vio_queue_kick(vs.events);
	printf("net: virtio socket device on %s: guest cid %llu, the app's frames on port %u\n", vio_name(vs.dev), vs.cid, port);
	return true;
}

static struct hakios_net_link vsock_link = {
	.name = "the Scion app's network (virtio socket)",
	.address = { 0x02, 'r', 'e', 'd', 'w', 'd' },
	.up = true,
	.transmit = vsock_transmit,
	.poll = vsock_poll,
	.counts = vsock_counts,
};

/* ---- virtio_net_wifi=1: a station with one network, always in reach ------------------------------------- */

#define VSTA_FREQUENCY          2437            /* channel 6 */
#define VSTA_RSSI               (-45)

static struct hakios_sta_bss vsta_bss;
static enum hakios_sta_phase vsta_phase = HAKIOS_STA_IDLE;
static int vsta_error;

static void
vsta_link(bool up)
{
	hakios_net_link_state(up);
}

static int
vsta_power(bool on)
{
	if (!on && vsta_phase == HAKIOS_STA_JOINED) {
		vsta_link(false);
	}
	vsta_phase = on ? HAKIOS_STA_IDLE : HAKIOS_STA_OFF;
	return 0;
}

static int
vsta_scan(__unused const struct hakios_sta_scan *what)
{
	return vsta_phase == HAKIOS_STA_OFF ? ENETDOWN : 0;
}

static void
vsta_heard(void (*each)(const struct hakios_sta_bss *bss, void *context), void *context)
{
	each(&vsta_bss, context);
}

static int
vsta_join(const struct hakios_sta_join *what)
{
	bool ours = what->ssid_length == vsta_bss.ssid_length && memcmp(what->ssid, vsta_bss.ssid, what->ssid_length) == 0;

	if (vsta_phase == HAKIOS_STA_OFF) {
		return ENETDOWN;
	}
	if (!ours) {
		vsta_error = ENOENT;            /* (nothing else is in reach) */
		return ENOENT;
	}
	if (what->security != HAKIOS_STA_OPEN) {
		vsta_error = EAUTH;             /* (it is an open network: a key is the wrong one) */
		return EAUTH;
	}
	vsta_error = 0;
	if (vsta_phase != HAKIOS_STA_JOINED) {
		vsta_phase = HAKIOS_STA_JOINED;
		vsta_link(true);
	}
	return 0;
}

static int
vsta_leave(void)
{
	if (vsta_phase == HAKIOS_STA_JOINED) {
		vsta_link(false);
		vsta_phase = HAKIOS_STA_IDLE;
	}
	return 0;
}

static void
vsta_state(struct hakios_sta_state *state)
{
	state->phase = vsta_phase;
	state->bss = vsta_bss;
	state->last_error = vsta_error;
}

static unsigned
vsta_channels(struct hakios_sta_channel *channels, unsigned most)
{
	static const uint16_t frequencies[] = { 2412, 2417, 2422, 2427, 2432, 2437, 2442, 2447, 2452, 2457, 2462, 5180, 5200, 5220, 5240 };
	unsigned count = 0;

	for (unsigned i = 0; i < sizeof(frequencies) / sizeof(frequencies[0]) && count < most; i++) {
		channels[count++] = (struct hakios_sta_channel){ .frequency = frequencies[i] };
	}
	return count;
}

static struct hakios_sta_ops vsta_ops = {
	.power = vsta_power,
	.scan = vsta_scan,
	.heard = vsta_heard,
	.join = vsta_join,
	.leave = vsta_leave,
	.state = vsta_state,
	.channels = vsta_channels,
};

/* The network: what a beacon of an open 802.11g access point on channel 6 would say. */
static void
vsta_start(const uint8_t address[ETHER_ADDR_LEN])
{
	static const uint8_t rates[] = { 0x82, 0x84, 0x8b, 0x96, 0x0c, 0x12, 0x18, 0x24 };     /* 1 2 5.5 11 basic; 6 9 12 18 */
	static const uint8_t more_rates[] = { 0x30, 0x48, 0x60, 0x6c };                       /* 24 36 48 54 */
	static struct hakios_sta_join join;
	char name[33] = "Scion";
	struct hakios_sta_bss *b = &vsta_bss;
	uint8_t *e = b->elements;
	size_t length;

	PE_parse_boot_argn("virtio_net_ssid", name, sizeof(name));
	name[32] = 0;
	length = strlen(name);
	if (length == 0) {
		strlcpy(name, "Scion", sizeof(name));
		length = 5;
	}
	memcpy(b->ssid, name, length);
	b->ssid_length = (uint8_t)length;
	/* the access point's address: ours with another last byte, and "locally administered" */
	memcpy(b->bssid, address, ETHER_ADDR_LEN);
	b->bssid[0] = (uint8_t)((b->bssid[0] | 0x02) & ~0x01);
	b->bssid[5] ^= 0x01;
	b->frequency = VSTA_FREQUENCY;
	b->rssi = VSTA_RSSI;
	b->beacon_interval = 100;
	b->capability = 0x0421;                 /* an access point (ESS), short preamble, short slot time; no privacy */
	*e++ = 0;                               /* SSID */
	*e++ = (uint8_t)length;
	memcpy(e, name, length);
	e += length;
	*e++ = 1;                               /* supported rates */
	*e++ = sizeof(rates);
	memcpy(e, rates, sizeof(rates));
	e += sizeof(rates);
	*e++ = 3;                               /* the channel */
	*e++ = 1;
	*e++ = 6;
	*e++ = 50;                              /* extended rates */
	*e++ = sizeof(more_rates);
	memcpy(e, more_rates, sizeof(more_rates));
	e += sizeof(more_rates);
	b->elements_length = (uint16_t)(e - b->elements);

	memcpy(vsta_ops.address, address, ETHER_ADDR_LEN);
	hakios_sta_attach(&vsta_ops);
	/* joined at the start, and again whenever iOS turns Wi-Fi back on (as the phone joins the network of its file) */
	bzero(&join, sizeof(join));
	memcpy(join.ssid, b->ssid, length);
	join.ssid_length = b->ssid_length;
	join.security = HAKIOS_STA_OPEN;
	if (hakios_sta_ask_join_standing(&join) != 0) {
		printf("net: the virtual station did not take the standing join\n");
	}
	printf("net: en0 is a radio (virtio_net_wifi=1): one open network, \"%s\", joined at the start\n", name);
}

void
hakios_virtio_net_init(void)
{
	uint32_t enabled = 1, wifi = 0, port = 0;
	struct hakios_net_link *link;
	bool started;

	if (PE_parse_boot_argn("virtio_net", &enabled, sizeof(enabled)) && !enabled) {
		return;
	}
	PE_parse_boot_argn("virtio_net_vsock", &port, sizeof(port));
	if (port != 0 && vio_find(VIRTIO_ID_VSOCK, 0) != NULL) {
		link = &vsock_link;
	} else if (vio_find(VIRTIO_ID_NET, 0) != NULL) {
		link = &virtio_link;
	} else {
		if (port != 0) {
			printf("net: virtio_net_vsock=%u, but there is no virtio socket device\n", port);
		}
		return;                 /* quietly otherwise: most machines have neither */
	}
	if (PE_parse_boot_argn("virtio_net_wifi", &wifi, sizeof(wifi)) && wifi) {
		link->wireless = true;
		link->up = false;               /* until the station has joined its network */
	}
	started = link == &vsock_link ? vsock_start(port) : virtio_net_start(link->address);
	if (!started || !hakios_net_attach(link)) {
		printf("net: no network interface\n");
		return;
	}
	if (link->wireless) {
		vsta_start(link->address);
	}
}
