// SPDX-License-Identifier: MIT-0
/*
 * hakios_dev_strict=1 (off by default; NOT run on a phone): who may open the device nodes and controls through which a
 * process reaches the phone's hardware (the touch screen, the speaker and microphone, the vibrator, the cameras and the
 * torch, the motion sensors, the video decoder, the Bluetooth UART). Their files are 0666, and they stay so: on this port
 * an app runs as mobile as backboardd and mediaserverd do, and the sandbox stand-in checks nothing (bsd/kern/sweet_amfi.c,
 * kern_minimac.c: the policy "Sandbox" has no vnode checks), so a file's owner and mode cannot tell a program that needs a
 * node from any other. With the argument an open is let through for root, and for a program the driver names by its
 * executable's path (the system volume's: an app cannot put a program there) that launchd started (its parent is pid 1:
 * an app that runs one of those programs itself, with a library of its own put in through DYLD_INSERT_LIBRARIES, which
 * the AMFI stand-in lets dyld honour, is its parent and is refused). Anything else gets EPERM and one line in the log
 * naming the node and the caller (the first 20 of each driver, then every 100th). Without it every open is let through,
 * as before. It is not a boundary against a process that can have launchd start a job of its making; nothing on this port
 * stops that while the AMFI and sandbox stand-ins allow everything: it narrows who can, and says who tried.
 * hakios_dev_strict_root() is the same for a sysctl that only root should use while it is on.
 *
 * A header of static functions, included by each driver that uses it, because the two kernels share no file the drivers
 * could call into (the iOS 27 kernel takes only the drivers profiles/ios-27.copy names).
 */
#ifndef HAKIOS_DEV_STRICT_H
#define HAKIOS_DEV_STRICT_H

#include <stdbool.h>
#include <stdint.h>
#include <libkern/libkern.h>
#include <pexpert/pexpert.h>
#include <sys/errno.h>
#include <sys/kauth.h>
#include <sys/proc.h>
#include <sys/systm.h>
#include <sys/vnode.h>

extern vnode_t proc_getexecutablevnode(proc_t);

static inline bool
hakios_dev_strict_enabled(void)
{
	static int on = -1;

	if (on < 0) {
		uint32_t v = 0;

		(void)PE_parse_boot_argn("hakios_dev_strict", &v, sizeof(v));
		on = v != 0;
	}
	return on != 0;
}

static inline void
hakios_dev_strict_refuse(const char *what, const char *path)
{
	static uint32_t refused;
	uint32_t n = refused++;
	char name[32] = "";

	if (n < 20 || n % 100 == 0) {
		proc_selfname(name, sizeof(name));
		printf("hakios_dev_strict: %s refused to %s[%d], uid %u, %s (%u refused)\n", what, name, proc_selfpid(),
		    kauth_cred_getuid(kauth_cred_get()), path, n + 1);
	}
}

/* 0, or EPERM (said in the log); programs: the executables' paths that may, ended by NULL */
static inline int
hakios_dev_open_check(const char *what, const char *const *programs)
{
	char path[200] = "";
	int len = sizeof(path);
	vnode_t vp;

	if (!hakios_dev_strict_enabled() || kauth_cred_getuid(kauth_cred_get()) == 0) {
		return 0;
	}
	vp = proc_getexecutablevnode(current_proc());
	if (vp != NULLVP) {
		if (vn_getpath(vp, path, &len) != 0) {
			path[0] = 0;
		}
		vnode_put(vp);
	}
	for (; path[0] != 0 && programs != NULL && *programs != NULL; programs++) {
		if (strcmp(path, *programs) == 0 && proc_selfppid() == 1) {
			return 0;
		}
	}
	if (path[0] != 0 && proc_selfppid() != 1) {
		strlcat(path, " (not started by launchd)", sizeof(path));
	}
	hakios_dev_strict_refuse(what, path[0] != 0 ? path : "(no executable path)");
	return EPERM;
}

static inline int
hakios_dev_strict_root(const char *what)
{
	if (!hakios_dev_strict_enabled() || kauth_cred_getuid(kauth_cred_get()) == 0) {
		return 0;
	}
	hakios_dev_strict_refuse(what, "root only");
	return EPERM;
}

#endif /* HAKIOS_DEV_STRICT_H */
