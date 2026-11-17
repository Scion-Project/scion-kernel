// SPDX-License-Identifier: MIT-0
/*
 * The Galaxy S9's camera switch (drivers/camera/exynos/hakios.exynos_camera_switch.h, what hakios.exynos_camera.c decides
 * a change of camera with under cam_switch=1) on the build machine: the header's functions, called in the order the
 * driver calls them, by a model of the driver's three parties (write() and the console asking, the stream's thread
 * bringing a camera up and down, readers copying frames out of a ring of four). First a few runs written out (a change
 * while a reader copies, two in quick succession, one asked back, a new sensor that does not answer, an ask the phone
 * cannot take), then a long random run of the three. Checked throughout:
 *   - at most one camera powered, and only the camera the stream is of; the camera changes only with nothing powered and
 *     no reader copying;
 *   - every frame given is labelled with the camera that took it, and is of the camera asked for when it is given;
 *   - a change that fails leaves nothing powered and the readers told (EIO) until the next open;
 *   - the counters add up.
 * The model is this file's own reading of the driver (stream_session(), s9dev_read(), s9dev_write(), switch_ask()); what
 * it checks is the decisions, not the hardware. Built with -fsanitize=address,undefined by tools/exynos-camera-switch-test.sh.
 */
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../drivers/camera/exynos/hakios.exynos_camera_switch.h"

#define SLOTS   4
#define READERS 3

static int failures;

#define CHECK(cond, ...) do { \
	if (!(cond)) { \
		failures++; \
		fprintf(stderr, "FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); \
		fprintf(stderr, __VA_ARGS__); \
		fputc('\n', stderr); \
		if (failures > 20) { \
			exit(1); \
		} \
	} \
} while (0)

enum { R_IDLE, R_COPYING };
enum { T_IDLE, T_STREAMING, T_ENDING };
enum read_result { GIVEN, AGAIN, FAULT, NOTHING };

struct model {
	struct camsw sw;
	uint32_t powered;               /* 0 none, else the camera powered (CAMSW_REAR, CAMSW_FRONT) */
	uint32_t also_powered;          /* a second camera powered: must stay 0 */
	int thread;                     /* T_IDLE, T_STREAMING, T_ENDING (waiting for the readers to let go) */
	bool end_ok;                    /* the ending stream's verdict */
	bool failed;                    /* stream_failed: readers get EIO until the next open */
	bool fail_up[3];                /* the next bring-up of this camera fails (the sensor does not answer) */
	int newest;                     /* stream_newest_slot */
	uint64_t sequence;
	struct {
		uint32_t producer, label;
		uint64_t sequence;
	} slot[SLOTS];
	struct {
		int state, slot;
		uint64_t generation, sequence;
		uint32_t label, producer;
	} reader[READERS];
	int copying;                    /* stream_readers */
	uint64_t given, again, faults, changes_seen;
};

static void
model_init(struct model *m, bool boot_rear, bool rear_offered)
{
	memset(m, 0, sizeof(*m));
	camsw_init(&m->sw, boot_rear, rear_offered);
	m->newest = -1;
}

/* s9dev_write()'s HAKIOS_CAMERA_SENSOR (or the sysctl): switch_ask() */
static enum camsw_answer
model_ask(struct model *m, uint32_t which)
{
	enum camsw_answer answer = camsw_ask(&m->sw, which);

	if (answer == CAMSW_ASKED) {
		m->newest = -1;
	}
	CHECK(answer != CAMSW_ASKED || m->sw.asked == which, "asked %u, recorded %u", which, m->sw.asked);
	return answer;
}

/* s9dev_open(): a stream that failed is tried again */
static void
model_open(struct model *m)
{
	m->failed = false;
}

/* the stream's thread: one step of stream_loop()/stream_session() */
static void
model_thread(struct model *m, bool wanted)
{
	switch (m->thread) {
	case T_IDLE:
		if (m->failed || !wanted) {
			return;
		}
		/* the start of stream_session(): nothing powered, nobody copying, no frame to give */
		CHECK(m->powered == 0 && m->copying == 0 && m->newest == -1, "a stream begins with camera %u powered, %d copying, "
		    "newest %d", m->powered, m->copying, m->newest);
		bool change = camsw_session_begin(&m->sw);
		(void)change;
		CHECK(m->sw.now == CAMSW_FRONT || m->sw.now == CAMSW_REAR, "the stream's camera %u", m->sw.now);
		if (m->powered != 0) {
			m->also_powered = m->sw.now;
		}
		m->powered = m->sw.now;         /* sensor_up() */
		if (m->fail_up[m->sw.now]) {
			m->fail_up[m->sw.now] = false;
			m->powered = 0;         /* sensor_up()'s own power_down(), then all_down() */
			m->end_ok = false;
			m->thread = T_ENDING;
			return;
		}
		m->thread = T_STREAMING;
		return;
	case T_STREAMING:
		if (!wanted) {
			m->end_ok = true;
		} else if (m->sw.asked != m->sw.now) {
			m->end_ok = true;       /* the loop's top: the other camera asked for */
		} else {
			/* a frame end: put in the ring if the camera asked for is still this one */
			int s = (int)(m->sequence % SLOTS);

			CHECK(m->powered == m->sw.now, "streaming camera %u, powered %u", m->sw.now, m->powered);
			if (!camsw_publish_ok(&m->sw)) {
				m->end_ok = true;
			} else {
				m->sequence++;
				m->slot[s].producer = m->powered;
				m->slot[s].label = m->sw.now;
				m->slot[s].sequence = m->sequence;
				(void)camsw_published(&m->sw);
				m->newest = s;
				return;
			}
		}
		/* frames_halt(), all_down(): nothing powered; then the readers waited for */
		m->powered = 0;
		m->newest = -1;
		m->thread = T_ENDING;
		return;
	case T_ENDING:
		if (m->copying != 0) {
			return;                 /* (msleep on stream_readers) */
		}
		CHECK(m->powered == 0, "a stream ended with camera %u powered", m->powered);
		m->newest = -1;
		(void)camsw_session_end(&m->sw, m->end_ok);
		if (!m->end_ok) {
			m->failed = true;       /* stream_loop(): readers get EIO */
		}
		m->thread = T_IDLE;
		return;
	}
}

/* s9dev_read()'s first half: the header taken under the lock */
static enum read_result
model_read_begin(struct model *m, int i)
{
	if (m->reader[i].state != R_IDLE) {
		return NOTHING;
	}
	if (m->failed) {
		m->faults++;
		return FAULT;
	}
	if (m->newest < 0) {
		return NOTHING;                 /* (the read waits; after its time, EAGAIN) */
	}
	m->reader[i].state = R_COPYING;
	m->reader[i].slot = m->newest;
	m->reader[i].generation = m->sw.generation;
	m->reader[i].label = m->slot[m->newest].label;
	m->reader[i].producer = m->slot[m->newest].producer;
	m->reader[i].sequence = m->slot[m->newest].sequence;
	m->copying++;
	return GIVEN;
}

/* s9dev_read()'s end: the frame copied out; given, or EAGAIN */
static enum read_result
model_read_end(struct model *m, int i)
{
	bool ok;

	if (m->reader[i].state != R_COPYING) {
		return NOTHING;
	}
	m->reader[i].state = R_IDLE;
	m->copying--;
	/* overtaken: the WDMA came round to the slot (its frame is another now) */
	ok = m->slot[m->reader[i].slot].sequence == m->reader[i].sequence;
	if (ok && !camsw_reader_ok(&m->sw, m->reader[i].generation)) {
		ok = false;
	}
	if (!ok) {
		m->again++;
		return AGAIN;
	}
	CHECK(m->reader[i].label == m->reader[i].producer, "a frame of camera %u given labelled %u", m->reader[i].producer,
	    m->reader[i].label);
	CHECK(m->reader[i].producer == m->sw.asked, "a frame of camera %u given while camera %u is asked for",
	    m->reader[i].producer, m->sw.asked);
	m->given++;
	return GIVEN;
}

static void
model_invariants(const struct model *m)
{
	CHECK(m->also_powered == 0, "two cameras powered (%u and %u)", m->powered, m->also_powered);
	CHECK(m->powered == 0 || m->powered == m->sw.now, "camera %u powered while the stream is %u's", m->powered, m->sw.now);
	CHECK(m->thread != T_IDLE || m->powered == 0, "camera %u powered with no stream", m->powered);
	CHECK(m->sw.done + m->sw.failed + m->sw.superseded + (m->sw.pending ? 1u : 0u) <= m->sw.asks,
	    "%u done + %u failed + %u superseded + %d pending > %u asked", m->sw.done, m->sw.failed, m->sw.superseded,
	    m->sw.pending, m->sw.asks);
	CHECK(m->sw.asks == m->sw.generation, "%u asks, generation %llu", m->sw.asks, (unsigned long long)m->sw.generation);
	CHECK(m->copying >= 0 && m->copying <= READERS, "%d copying", m->copying);
}

static void
run_thread_until_streaming(struct model *m)
{
	for (int i = 0; i < 10 && m->thread != T_STREAMING; i++) {
		model_thread(m, true);
		model_invariants(m);
	}
}

static void
scenarios(void)
{
	struct model m;

	/* the asks this phone cannot take */
	model_init(&m, false, true);
	CHECK(model_ask(&m, 0) == CAMSW_REFUSED && model_ask(&m, 3) == CAMSW_REFUSED && model_ask(&m, 4) == CAMSW_REFUSED &&
	    model_ask(&m, ~0u) == CAMSW_REFUSED, "an ask for a camera the S9 has not");
	CHECK(model_ask(&m, CAMSW_FRONT) == CAMSW_SAME && m.sw.generation == 0 && m.sw.refused == 4, "the front again");
	model_init(&m, false, false);
	CHECK(model_ask(&m, CAMSW_REAR) == CAMSW_REFUSED && m.sw.asked == CAMSW_FRONT, "the rear camera not offered");
	model_init(&m, true, false);
	CHECK(m.sw.rear_offered && m.sw.now == CAMSW_REAR, "a run that begins with the rear camera offers it");

	/* a change while a reader copies a frame of the front camera: EAGAIN, then the rear camera's frames */
	model_init(&m, false, true);
	run_thread_until_streaming(&m);
	model_thread(&m, true);         /* a frame */
	CHECK(model_read_begin(&m, 0) == GIVEN, "the first header");
	CHECK(model_ask(&m, CAMSW_REAR) == CAMSW_ASKED, "the rear camera asked for");
	CHECK(model_read_begin(&m, 1) == NOTHING, "a second reader finds no frame after the ask");
	model_thread(&m, true);         /* the loop's top: the stream ends, powered down */
	CHECK(m.powered == 0 && m.thread == T_ENDING, "the front camera down");
	model_thread(&m, true);         /* waits for the reader */
	CHECK(m.thread == T_ENDING, "the stream's end waits for the reader copying");
	CHECK(model_read_end(&m, 0) == AGAIN && m.sw.dropped == 1, "the frame copied over the ask is not given");
	model_thread(&m, true);         /* the end */
	model_thread(&m, true);         /* the rear camera up */
	CHECK(m.powered == CAMSW_REAR && m.sw.pending, "the rear camera up, its change pending");
	model_thread(&m, true);         /* its first frame */
	CHECK(m.sw.done == 1 && !m.sw.pending, "the change done at its first frame");
	CHECK(model_read_begin(&m, 0) == GIVEN && model_read_end(&m, 0) == GIVEN && m.given == 1, "a rear frame given");
	model_invariants(&m);

	/* two in quick succession: rear, then front again before the stream saw it: no change, the frames go on */
	model_init(&m, false, true);
	run_thread_until_streaming(&m);
	model_thread(&m, true);
	CHECK(model_ask(&m, CAMSW_REAR) == CAMSW_ASKED && model_ask(&m, CAMSW_FRONT) == CAMSW_ASKED, "two asks");
	CHECK(m.newest == -1, "no frame given from before the asks");
	model_thread(&m, true);
	CHECK(m.thread == T_STREAMING && m.powered == CAMSW_FRONT && m.newest >= 0, "the front camera's stream goes on");
	CHECK(model_read_begin(&m, 0) == GIVEN && model_read_end(&m, 0) == GIVEN, "a front frame given");
	CHECK(m.sw.asks == 2 && m.sw.done == 0 && m.sw.generation == 2, "two asks, no change");

	/* two in quick succession taken in turn: rear (seen by the stream), then front while the rear comes up */
	model_init(&m, false, true);
	run_thread_until_streaming(&m);
	model_thread(&m, true);
	model_ask(&m, CAMSW_REAR);
	model_thread(&m, true);         /* front down */
	model_thread(&m, true);         /* end */
	model_thread(&m, true);         /* rear up */
	CHECK(m.powered == CAMSW_REAR, "the rear camera up");
	model_ask(&m, CAMSW_FRONT);
	model_thread(&m, true);         /* the rear's loop top: ends before its first frame */
	model_thread(&m, true);
	CHECK(m.sw.superseded == 1 && m.sw.failed == 0 && !m.failed, "the rear change superseded, not failed");
	run_thread_until_streaming(&m);
	model_thread(&m, true);
	CHECK(m.powered == CAMSW_FRONT && m.sw.done == 1, "the front camera's frames again");

	/* the new sensor does not answer: both cameras off, EIO until the next open, which tries it again */
	model_init(&m, false, true);
	run_thread_until_streaming(&m);
	model_thread(&m, true);
	model_ask(&m, CAMSW_REAR);
	m.fail_up[CAMSW_REAR] = true;
	model_thread(&m, true);         /* front down */
	model_thread(&m, true);         /* end */
	model_thread(&m, true);         /* rear up fails */
	model_thread(&m, true);         /* its end */
	CHECK(m.powered == 0 && m.failed && m.sw.failed == 1, "the failed change leaves nothing powered and says so");
	CHECK(model_read_begin(&m, 0) == FAULT, "a reader gets EIO");
	model_thread(&m, true);
	CHECK(m.thread == T_IDLE && m.powered == 0, "nothing is tried before the next open");
	model_open(&m);
	run_thread_until_streaming(&m);
	model_thread(&m, true);
	CHECK(m.powered == CAMSW_REAR && m.newest >= 0 && m.sw.done == 0, "the next open: the rear camera again, not a change");
	CHECK(model_read_begin(&m, 0) == GIVEN && model_read_end(&m, 0) == GIVEN, "its frame given");
	model_invariants(&m);
}

static uint64_t rng = 0x9E3779B97F4A7C15ull;

static uint32_t
next(void)
{
	rng ^= rng << 13;
	rng ^= rng >> 7;
	rng ^= rng << 17;
	return (uint32_t)(rng >> 32);
}

static void
random_runs(void)
{
	static const uint32_t asks[] = { CAMSW_FRONT, CAMSW_REAR, CAMSW_FRONT, CAMSW_REAR, 0, 3, 4 };
	uint64_t given = 0, again = 0, faults = 0;
	uint32_t done = 0, failed = 0, dropped = 0;

	for (int run = 0; run < 200; run++) {
		struct model m;
		bool wanted = true;

		model_init(&m, run % 3 == 1, run % 5 != 4);
		for (int step = 0; step < 20000; step++) {
			uint32_t r = next() % 100;

			if (r < 3) {
				(void)model_ask(&m, asks[next() % (sizeof(asks) / sizeof(asks[0]))]);
			} else if (r < 4) {
				m.fail_up[next() % 2 ? CAMSW_REAR : CAMSW_FRONT] = true;
			} else if (r < 5) {
				wanted = !wanted;
			} else if (r < 6) {
				model_open(&m);
			} else if (r < 40) {
				model_thread(&m, wanted);
			} else if (r < 70) {
				(void)model_read_begin(&m, (int)(next() % READERS));
			} else {
				(void)model_read_end(&m, (int)(next() % READERS));
			}
			model_invariants(&m);
		}
		given += m.given;
		again += m.again;
		faults += m.faults;
		done += m.sw.done;
		failed += m.sw.failed;
		dropped += m.sw.dropped;
	}
	/* the random runs must have met every case, or they prove little */
	CHECK(given > 1000 && again > 100 && faults > 10 && done > 100 && failed > 10 && dropped > 10,
	    "the random runs: %llu given, %llu EAGAIN, %llu EIO, %u done, %u failed, %u dropped", (unsigned long long)given,
	    (unsigned long long)again, (unsigned long long)faults, done, failed, dropped);
	printf("exynos_camera_switch_test: 200 random runs of 20000 steps: %llu frames given, %llu EAGAIN, %llu EIO; "
	    "%u changes done, %u failed, %u frames dropped over a change\n", (unsigned long long)given,
	    (unsigned long long)again, (unsigned long long)faults, done, failed, dropped);
}

int
main(void)
{
	scenarios();
	random_runs();
	if (failures != 0) {
		fprintf(stderr, "exynos_camera_switch_test: %d FAILED\n", failures);
		return 1;
	}
	printf("exynos_camera_switch_test: ok\n");
	return 0;
}
