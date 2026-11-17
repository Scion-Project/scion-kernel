/* SPDX-License-Identifier: MIT */
/* camera_oslog.c: the program's os_log to /private/var/local/oslog/<name>.log and, on topic, to the console (cam_trace) */
#ifndef SCIONCAM_OSLOG_H
#define SCIONCAM_OSLOG_H

/* level 1: what is logged; 2: also os_log's info and debug levels. Returns how many entry points were hooked (4 expected;
 * +10 when level 2 turned the other levels on). */
int scioncam_oslog_start(const char *name, int level);

#endif
