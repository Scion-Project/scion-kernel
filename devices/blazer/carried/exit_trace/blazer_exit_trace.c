/* SPDX-License-Identifier: MIT
 * Scion: from ios-on-pixel-10 by TheAndersMadsen (Scion-Project/scion-blazer f04fd1f, runtime/exit_trace/blazer_exit_trace.c), imported with the author's
 * permission, credit kept (NOTICE). Carried, not built: devices/blazer/carried/README.md.
 * Copyright (c) 2026 hackios contributors
 * Original bounded exit observation. Uses XNU's existing user unwinder.
 * No process memory contents, hardware operations or changes to exit policy.
 */
#if defined(BLAZER) && BLAZER
#ifdef HKET_HOST_TEST
#include "exit_trace_test_shim.h"
#else
#include <arm/thread.h>
#include <kern/backtrace.h>
#include <kern/debug.h>
#include <kern/misc_protos.h>
#include <kern/startup.h>
#include <kern/task.h>
#include <kern/thread.h>
#include <mach/machine/thread_status.h>
#include <os/atomic_private.h>
#include <string.h>
#include <vm/vm_shared_region.h>
#include <vm/vm_shared_region_xnu.h>
#endif

#define HKET_RECORD_LIMIT 32u
#define HKET_FRAME_LIMIT 12u
/* XNU p_comm is MAXCOMLEN(16) plus NUL; keep BSD headers out of osfmk. */
#define HKET_COMM_BYTES 17u

/* Parsed during kernel startup, before user exits; no first-exit init race. */
static TUNABLE_STR(hket_name, 32, "hk_exittrace", "");
static unsigned int hket_records;

void blazer_exit_trace(struct proc *, int);
extern char *proc_name_address(struct proc *);
extern int proc_pid(struct proc *);

static unsigned int
hket_reserve(void)
{
	unsigned int old = os_atomic_load(&hket_records, relaxed);

	while (old < HKET_RECORD_LIMIT) {
		if (os_atomic_cmpxchg(&hket_records, old, old + 1u, relaxed)) {
			return old + 1u;
		}
		old = os_atomic_load(&hket_records, relaxed);
	}
	return 0;
}

/* Deliberately returns. The caller's noreturn attribute belongs to exit(). */
void
blazer_exit_trace(struct proc *p, int status)
{
	const arm_saved_state_t *ss;
	thread_t thread;
	task_t task;
	vm_shared_region_t region;
	uintptr_t frames[HKET_FRAME_LIMIT] = {0};
	struct backtrace_user_info info = BTUINFO_INIT;
	unsigned int id, count, shared_known = 0;
	uint32_t slide = 0;
	const char *name;

	if (p == NULL || hket_name[0] == '\0') {
		return;
	}
	name = proc_name_address(p);
	if (name == NULL || strncmp(name, hket_name, HKET_COMM_BYTES) != 0) {
		return;
	}
	thread = current_thread();
	task = get_threadtask(thread);
	ss = get_user_regs(thread);
	if (task == NULL || task == kernel_task || ss == NULL || !is_saved_state64(ss)) {
		return;
	}
	id = hket_reserve();
	if (id == 0) {
		return;
	}
	region = vm_shared_region_get(task);
	if (region != NULL) {
		slide = region->sr_slide;
		shared_known = 1;
		vm_shared_region_deallocate(region);
	}
	printf("hk_exittrace_v2: %s[%d] id %u exit(%d) pc 0x%llx lr 0x%llx "
	    "main 0x%llx shared_slide 0x%x shared_known %u\n",
	    name, proc_pid(p), id, status,
	    (unsigned long long)get_saved_state_pc(ss),
	    (unsigned long long)get_saved_state_lr(ss),
	    (unsigned long long)task->mach_header_vm_address, slide, shared_known);

	/* This API bounds copies and frame traversal, including malformed chains.
	 * Raw addresses are retained: PAC stripping depends on the kernel's actual
	 * architecture configuration. A host must not guess stripped addresses.
	 */
	count = backtrace_user(frames, HKET_FRAME_LIMIT, NULL, &info);
	if (count > HKET_FRAME_LIMIT) {
		printf("hk_exittrace_v2: %s[%d] id %u invalid_count %u\n",
		    name, proc_pid(p), id, count);
		count = HKET_FRAME_LIMIT;
	}
	for (unsigned int i = 0; i < count; i++) {
		printf("hk_exittrace_v2: %s[%d] id %u frame %u address 0x%llx\n",
		    name, proc_pid(p), id, i, (unsigned long long)frames[i]);
	}
	printf("hk_exittrace_v2: %s[%d] id %u end frames %u error %d info 0x%x\n",
	    name, proc_pid(p), id, count, info.btui_error,
	    (unsigned int)info.btui_info);
}
#endif
