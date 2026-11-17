/* SPDX-License-Identifier: MIT-0 */
/*
 * Host test of drivers/display/sweetfb/hakios.sweetfb_present.h, the sweetfb_nowait present state machine: a window server
 * that queues a frame's GPU chains and swaps (held, as SwapEnd is, until the frame before it is done), a GPU that finishes
 * chains in order at random moments or stalls, vsync ticks, and the display going off and on, in random interleavings over
 * many seeds. Checked: only finished frames are shown by SFP_SHOW, frames are shown in order, every swap is released exactly
 * once, a recorded frame is taken within SFP_MAX_TICKS + 1 ticks of a lit display, with the GPU completing nothing is ever
 * left behind (no lost wake-up), the self-check never fires when the done number is honest, and it does fire (and the display
 * goes on) when the done number is stuck. Run by tools/sweetfb-present-test.sh under the address and UB sanitizers.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../drivers/display/sweetfb/hakios.sweetfb_present.h"

#define FRAMES_MAX 200000
static unsigned char released[FRAMES_MAX + 1];
static unsigned failures;
static uint64_t total_idle_fb[3], total_slow_fb[3], total_shown[3], total_deferred[3];

#define CHECK(c, ...) do { if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
	fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); if (failures > 20) exit(1); } } while (0)

static uint64_t rng;
static uint32_t
rnd(uint32_t n)
{
	rng ^= rng << 13; rng ^= rng >> 7; rng ^= rng << 17;
	return (uint32_t)(rng % n);
}

static void
release(uint32_t id, const char *why)
{
	if (id == 0) {
		return;
	}
	CHECK(id <= FRAMES_MAX, "id %u", id);
	CHECK(!released[id], "swap %u released twice (%s)", id, why);
	released[id] = 1;
}

struct world {
	struct sfp s;
	uint64_t q_seq, q_done;         /* the GPU queue: last chain given, last chain done */
	uint32_t next_id;               /* the window server's next swap id */
	uint64_t held_for;              /* the window server is in SwapEnd, held until q_done reaches this (0: not held) */
	uint32_t held_id;
	uint64_t held_seq;
	uint64_t prev_swap_seq;
	bool on;
	uint32_t last_shown;
	uint64_t stuck_done;            /* stale mode: the done number the ticks see never moves past this */
	bool stale;
	uint32_t ticks_lit_waiting;     /* lit ticks the current recorded frame has seen */
	uint32_t frames;
};

static void
swap_record(struct world *w, uint32_t id, uint64_t seq)
{
	release(sfp_swap(&w->s, id, seq), "replaced");
	w->ticks_lit_waiting = 0;
}

/* The window server: queue 1-12 chains, then SwapEnd (held while the frame before is not done). */
static void
ws_step(struct world *w)
{
	if (w->held_for != 0) {
		if (w->q_done >= w->held_for) {
			w->held_for = 0;
			swap_record(w, w->held_id, w->held_seq);
		}
		return;
	}
	if (w->next_id > FRAMES_MAX) {
		return;
	}
	w->q_seq += 1 + rnd(12);
	uint32_t id = w->next_id++;
	uint64_t seq = w->q_seq, prev = w->prev_swap_seq;
	w->prev_swap_seq = seq;
	w->frames++;
	if (prev != 0 && w->q_done < prev) {
		w->held_for = prev;
		w->held_id = id;
		w->held_seq = seq;
	} else {
		swap_record(w, id, seq);
	}
}

static void
take(struct world *w, enum sfp_action act, uint32_t id, uint64_t seq)
{
	if (act == SFP_SHOW) {
		CHECK(seq == 0 || w->q_done >= seq, "frame %u shown before its chains were done (%llu < %llu)", id,
		    (unsigned long long)w->q_done, (unsigned long long)seq);
	} else {
		w->q_done = w->q_seq > seq ? (seq > w->q_done ? seq : w->q_done) : w->q_seq;   /* the old path: asleep until done */
		CHECK(w->q_done >= seq, "fallback did not finish");
	}
	if (id != 0) {
		CHECK(id > w->last_shown, "frame %u shown after %u", id, w->last_shown);
		w->last_shown = id;
	}
	release(id, "shown");
	w->ticks_lit_waiting = 0;
}

static void
tick(struct world *w)
{
	if (!w->on) {
		return;                 /* (display off: the fade; nothing taken) */
	}
	uint32_t id;
	uint64_t seq;
	uint64_t done = w->stale && w->q_done > w->stuck_done ? w->stuck_done : w->q_done;
	bool idle = w->q_done >= w->q_seq;
	bool had = w->s.recorded;
	enum sfp_action act = sfp_tick(&w->s, done, idle, &id, &seq);
	if (act == SFP_SHOW || act == SFP_FALLBACK) {
		take(w, act, id, seq);
	} else if (had) {
		w->ticks_lit_waiting++;
		CHECK(w->ticks_lit_waiting <= SFP_MAX_TICKS + 1, "a frame waited %u lit ticks", w->ticks_lit_waiting);
	}
}

/* One random schedule. mode 0: honest; 1: GPU stalls for long stretches; 2: the done number the ticks see is stuck. */
static void
run(uint64_t seed, int mode, uint32_t steps)
{
	struct world w;
	memset(&w, 0, sizeof(w));
	memset(released, 0, sizeof(released));
	rng = seed * 2654435761u + 1;
	w.next_id = 1;
	w.on = true;
	w.stale = mode == 2;
	w.stuck_done = 0;
	uint32_t stall = 0;
	uint32_t p_ws = 1 + rnd(8), p_gpu = 1 + rnd(16), p_tick = 1 + rnd(8);
	for (uint32_t i = 0; i < steps; i++) {
		uint32_t r = rnd(p_ws + p_gpu + p_tick + 1);
		if (mode == 1 && stall == 0 && rnd(500) == 0) {
			stall = 50 + rnd(400);
		}
		if (r < p_ws) {
			ws_step(&w);
		} else if (r < p_ws + p_gpu) {
			if (stall > 0) {
				stall--;
			} else if (w.q_done < w.q_seq) {
				w.q_done++;     /* one chain retired by the worker */
			}
		} else if (r < p_ws + p_gpu + p_tick) {
			tick(&w);
		} else if (rnd(50) == 0) {
			w.on = !w.on;
			if (w.on) {
				sfp_redraw(&w.s);
			}
		}
		if (mode == 2 && rnd(3000) == 0) {
			w.stuck_done = w.q_done;        /* the stuck number catches up now and then */
		}
	}
	/* the end: the window server stops, the GPU drains, the display comes on: everything shown or released */
	w.next_id = FRAMES_MAX + 1;
	while (w.held_for != 0) {
		w.q_done = w.q_seq;
		ws_step(&w);
	}
	w.q_done = w.q_seq;
	if (!w.on) {
		w.on = true;
		sfp_redraw(&w.s);
	}
	if (mode == 2) {
		w.stale = false;
	}
	tick(&w);
	CHECK(!w.s.recorded, "seed %llu mode %d: a frame left recorded with the GPU drained (a lost wake-up)",
	    (unsigned long long)seed, mode);
	for (uint32_t id = 1; id < w.frames + 1 && id <= FRAMES_MAX; id++) {
		CHECK(released[id], "seed %llu mode %d: swap %u never released", (unsigned long long)seed, mode, id);
	}
	total_idle_fb[mode] += w.s.idle_fallbacks;
	total_slow_fb[mode] += w.s.fallbacks;
	total_shown[mode] += w.s.shown;
	total_deferred[mode] += w.s.deferred;
	if (mode == 0) {
		CHECK(w.s.idle_fallbacks == 0, "seed %llu: the self-check fired on an honest schedule (%u)",
		    (unsigned long long)seed, w.s.idle_fallbacks);
	}
}

static void
fixed_cases(void)
{
	struct sfp s;
	uint32_t id;
	uint64_t seq;
	memset(&s, 0, sizeof(s));
	CHECK(sfp_tick(&s, 0, true, &id, &seq) == SFP_NOTHING, "empty");
	CHECK(sfp_swap(&s, 1, 10) == 0, "first swap replaces nothing");
	CHECK(sfp_tick(&s, 9, false, &id, &seq) == SFP_WAIT, "not done: wait");
	CHECK(sfp_tick(&s, 10, false, &id, &seq) == SFP_SHOW && id == 1 && seq == 10, "done: show");
	CHECK(sfp_tick(&s, 10, true, &id, &seq) == SFP_NOTHING, "shown once");
	/* a newer frame replaces an older pending one */
	sfp_swap(&s, 2, 20);
	CHECK(sfp_tick(&s, 15, false, &id, &seq) == SFP_WAIT, "wait");
	CHECK(sfp_swap(&s, 3, 30) == 2 && s.replaced == 1, "replaced");
	CHECK(sfp_tick(&s, 30, true, &id, &seq) == SFP_SHOW && id == 3, "newest shown");
	/* the redraw after display on: the last frame again, already released */
	sfp_redraw(&s);
	CHECK(sfp_tick(&s, 30, true, &id, &seq) == SFP_SHOW && id == 0 && seq == 30, "redraw");
	/* seq 0: nothing to wait for */
	sfp_swap(&s, 4, 0);
	CHECK(sfp_tick(&s, 0, false, &id, &seq) == SFP_SHOW && id == 4, "seq 0");
	/* the self-check: GPU idle, done never reaching: the old path after SFP_IDLE_TICKS waits */
	sfp_swap(&s, 5, 50);
	for (int i = 0; i < SFP_IDLE_TICKS; i++) {
		CHECK(sfp_tick(&s, 40, true, &id, &seq) == SFP_WAIT, "idle wait %d", i);
	}
	CHECK(sfp_tick(&s, 40, true, &id, &seq) == SFP_FALLBACK && id == 5 && s.idle_fallbacks == 1, "idle fallback");
	/* the GPU busy and slow: the old path after SFP_MAX_TICKS */
	sfp_swap(&s, 6, 60);
	for (int i = 0; i < SFP_MAX_TICKS; i++) {
		CHECK(sfp_tick(&s, 55, false, &id, &seq) == SFP_WAIT, "busy wait %d", i);
	}
	CHECK(sfp_tick(&s, 55, false, &id, &seq) == SFP_FALLBACK && id == 6 && s.fallbacks == 1, "slow fallback");
}

int
main(int argc, char **argv)
{
	uint32_t seeds = argc > 1 ? (uint32_t)strtoul(argv[1], NULL, 0) : 300;
	fixed_cases();
	for (uint32_t seed = 1; seed <= seeds; seed++) {
		for (int mode = 0; mode < 3; mode++) {
			run(seed, mode, 20000 + (seed % 7) * 10000);
		}
	}
	for (int m = 0; m < 3; m++) {
		printf("mode %d: shown %llu, ticks that waited %llu, old-path takes: GPU slow %llu, self-check %llu\n", m,
		    (unsigned long long)total_shown[m], (unsigned long long)total_deferred[m], (unsigned long long)total_slow_fb[m],
		    (unsigned long long)total_idle_fb[m]);
	}
	CHECK(total_deferred[0] > 0 && total_shown[0] > 0, "the honest schedules never deferred a frame: not a test");
	CHECK(total_slow_fb[1] > 0, "the stalls never reached the slow fallback: not a test");
	CHECK(total_idle_fb[2] > 0, "the stuck done number never reached the self-check: not a test");
	if (failures) {
		fprintf(stderr, "sweetfb_present_test: %u failures\n", failures);
		return 1;
	}
	printf("sweetfb_present_test: %u seeds x 3 modes: only finished frames shown, in order, each swap released once, "
	    "nothing left behind, the self-check quiet when honest and acting when not\n", seeds);
	return 0;
}
