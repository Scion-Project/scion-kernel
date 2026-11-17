// SPDX-License-Identifier: MIT-0
/*
 * The Galaxy S9's two cameras in one run (boot-arg cam_switch=1, hakios.exynos_camera.c): which camera the next stream brings
 * up, when a running stream has to end for the other one, what its frames are labelled, and whether a frame a reader has
 * copied out may be given. Plain C without kernel headers: tools/exynos_camera_switch_test.c runs this same code on the build
 * machine under the address and undefined-behaviour sanitizers. The driver holds its s9cam_lock around every call, and calls
 * nothing here without cam_switch=1 (the one-camera run keeps its own `asked_sensor', as before).
 *
 * The rules the driver keeps with these (and the test checks):
 *   - a change of camera is asked by write() (HAKIOS_CAMERA_SENSOR) or the console's sysctl: camsw_ask(). On CAMSW_ASKED the
 *     driver drops the ring's newest frame (stream_newest_slot = -1): no frame of the old camera is given after the ask;
 *   - the stream's thread brings a camera up only between two streams, with nothing powered and no reader holding a slot:
 *     camsw_session_begin() says which, and whether that is a change (the driver then puts in that camera's numbers);
 *   - a frame is put in the ring only while the camera asked for is the one streaming (camsw_publish_ok()), labelled
 *     camsw.now; otherwise the stream ends (stopped and powered down in the order its bring-up is undone);
 *   - a reader takes camsw.generation with the frame's header and gives the frame only if it is the same once the frame is
 *     copied out (camsw_reader_ok()): a change asked meanwhile makes the read EAGAIN, never a frame of the other camera.
 */
#ifndef HAKIOS_EXYNOS_CAMERA_SWITCH_H
#define HAKIOS_EXYNOS_CAMERA_SWITCH_H

#include <stdbool.h>
#include <stdint.h>

#define CAMSW_REAR      1u      /* the frames' sensor numbers (hakios.camera.h): 1 the wide, the S5K2L3 on CSIS0 */
#define CAMSW_FRONT     2u      /* 2 the front, the S5K3H1 on CSIS1 */

enum camsw_answer {
	CAMSW_REFUSED,          /* not a camera of this phone (0 macro, 3 ultra-wide, 4 telephoto), or the rear one not offered */
	CAMSW_SAME,             /* the camera asked for already: nothing changes */
	CAMSW_ASKED,            /* the other camera: the stream (if one runs) ends, and the next stream is that camera's */
};

struct camsw {
	bool            rear_offered;   /* the rear camera's blocks and supplies were there at the start of the run */
	bool            streaming;      /* a stream is between camsw_session_begin() and camsw_session_end() */
	bool            pending;        /* the stream begun is a change of camera that has not given its first frame yet */
	uint32_t        asked;          /* the camera the reader wants (CAMSW_FRONT or CAMSW_REAR) */
	uint32_t        now;            /* the camera of the stream that runs, or of the last one */
	uint64_t        generation;     /* one more at each change of `asked' */
	/* counters (kern.hakios.camera_switch.*) */
	uint32_t        asks;           /* changes asked */
	uint32_t        done;           /* changes that gave their first frame */
	uint32_t        failed;         /* changes whose stream did not come up: both cameras off, readers given EIO */
	uint32_t        superseded;     /* changes whose stream was ended by another change before its first frame */
	uint32_t        refused;        /* asks for a camera this run does not have */
	uint32_t        dropped;        /* frames copied out by a reader and not given: a change was asked meanwhile */
};

static inline void
camsw_init(struct camsw *sw, bool boot_rear, bool rear_offered)
{
	*sw = (struct camsw){ 0 };
	sw->rear_offered = rear_offered || boot_rear;
	sw->asked = sw->now = boot_rear ? CAMSW_REAR : CAMSW_FRONT;
}

static inline const char *
camsw_name(uint32_t which)
{
	return which == CAMSW_REAR ? "the rear camera" : which == CAMSW_FRONT ? "the front camera" : "no camera of this phone";
}

/* HAKIOS_CAMERA_SENSOR's `a', or the console's: see enum camsw_answer */
static inline enum camsw_answer
camsw_ask(struct camsw *sw, uint32_t which)
{
	if (which != CAMSW_FRONT && (which != CAMSW_REAR || !sw->rear_offered)) {
		sw->refused++;
		return CAMSW_REFUSED;
	}
	if (which == sw->asked) {
		return CAMSW_SAME;
	}
	sw->asked = which;
	sw->generation++;
	sw->asks++;
	return CAMSW_ASKED;
}

/* a stream begins (nothing powered, no reader holding a slot): its camera is camsw.now; true if that is a change */
static inline bool
camsw_session_begin(struct camsw *sw)
{
	bool change = sw->asked != sw->now;

	sw->now = sw->asked;
	sw->streaming = true;
	sw->pending = change;
	return change;
}

/* may the frame just taken be put in the ring (labelled camsw.now)? false: the stream ends for the other camera */
static inline bool
camsw_publish_ok(const struct camsw *sw)
{
	return sw->streaming && sw->asked == sw->now;
}

/* the frame is in the ring; true if it is the first of a change of camera (the change is done) */
static inline bool
camsw_published(struct camsw *sw)
{
	if (!sw->pending) {
		return false;
	}
	sw->pending = false;
	sw->done++;
	return true;
}

/*
 * The stream has ended, everything of it powered down, no reader holding a slot. ok: it came up and ended because nobody
 * wanted it any more or the other camera was asked for; !ok: it did not come up, or its frames stopped. Returns true if it
 * was a change of camera that failed (the driver says so: both cameras are off and readers get EIO).
 */
static inline bool
camsw_session_end(struct camsw *sw, bool ok)
{
	bool failed = sw->pending && !ok;

	if (sw->pending) {
		if (!ok) {
			sw->failed++;
		} else {
			sw->superseded++;
		}
	}
	sw->pending = false;
	sw->streaming = false;
	return failed;
}

/* a reader's frame, copied out: given only if no change was asked since its header was taken */
static inline bool
camsw_reader_ok(struct camsw *sw, uint64_t generation)
{
	if (generation == sw->generation) {
		return true;
	}
	sw->dropped++;
	return false;
}

#endif /* HAKIOS_EXYNOS_CAMERA_SWITCH_H */
