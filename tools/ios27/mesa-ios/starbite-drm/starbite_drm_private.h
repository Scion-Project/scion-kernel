/*
 * Project Starbite: what starbite_drm.c and starbite_diag.c share (hidden in the library: Mesa builds with
 * -fvisibility=hidden, and nothing here is a libdrm function).
 */
#ifndef STARBITE_DRM_PRIVATE_H
#define STARBITE_DRM_PRIVATE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define SBM_GEM_CLOSE   0x100                           /* StarbiteMali's selector for DRM_IOCTL_GEM_CLOSE */

/* starbite_drm.c: one line on the kernel's console through StarbiteMali's LOG selector (nothing without a connection) */
void sbdrm_kernel_log(const char *text, size_t length);

/* starbite_diag.c: a buffer made (bytes, made = true) or closed (made = false), for STARBITE_DRM_LEAKS */
void sbdrm_leak_note(uint32_t handle, uint64_t bytes, bool made);
/* starbite_diag.c: one request of this selector (0-15, SBM_GEM_CLOSE, others), for STARBITE_DRM_STATS */
void sbdrm_count(uint32_t selector);
/* starbite_diag.c: an mmap answered from the shim's cache, for the same line */
void sbdrm_count_mmap_local(void);

#endif
