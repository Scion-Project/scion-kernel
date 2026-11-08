/*
 * Project Starbite: what Mesa's Panfrost takes from libdrm's xf86drm.h, for
 * iOS, where there is no DRM. The functions are in starbite_drm.c; they carry
 * Panfrost's DRM requests to the kernel's StarbiteMali user client
 * (xnu/src/StarbiteMali.cpp). Request numbers and structures are Linux's
 * (Mesa's include/drm-uapi/panfrost_drm.h).
 *
 * After HakiOS tools/mesa/hakios-drm (MIT, Copyright the HakiOS authors),
 * reworked for Panfrost and StarbiteMali.
 */
#ifndef STARBITE_XF86DRM_H
#define STARBITE_XF86DRM_H

#include <fcntl.h>
#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#include "drm-uapi/drm.h"

#define DRM_NODE_PRIMARY        0
#define DRM_NODE_RENDER         2
#define DRM_CLOEXEC             O_CLOEXEC
#define DRM_RDWR                O_RDWR
#define DRM_SYNCOBJ_CREATE_SIGNALED (1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL (1 << 0)
#define DRM_SYNCOBJ_WAIT_FLAGS_WAIT_FOR_SUBMIT (1 << 1)

typedef struct _drmVersion {
	int version_major, version_minor, version_patchlevel;
	int name_len;
	char *name;
	int date_len;
	char *date;
	int desc_len;
	char *desc;
} drmVersion, *drmVersionPtr;

int drmOpenWithType(const char *name, const char *busid, int type);
int drmIoctl(int fd, unsigned long request, void *arg);
drmVersionPtr drmGetVersion(int fd);
void drmFreeVersion(drmVersionPtr version);
int drmGetCap(int fd, uint64_t capability, uint64_t *value);
int drmCloseBufferHandle(int fd, uint32_t handle);

/*
 * Buffers as file descriptors: none here, but memory made elsewhere comes in
 * as Linux's comes in by file descriptor: an IOSurface, by its ID where
 * Panfrost expects the descriptor. drmPrimeFDToHandle maps the surface into
 * the GPU (StarbiteMali SURFACE); exports fail with ENOSYS.
 */
int drmPrimeHandleToFD(int fd, uint32_t handle, uint32_t flags, int *prime_fd);
int drmPrimeFDToHandle(int fd, int surface, uint32_t *handle);

/* Not libdrm's: what an IOSurface is like as a buffer (StarbiteMali's answer). */
struct starbite_drm_surface {
	uint32_t surface, handle;
	uint64_t size;
	uint32_t offset, row_bytes;
	uint32_t width, height, pixel_format, planes;
	uint32_t flags, pad;
};
int starbite_drm_surface(int fd, struct starbite_drm_surface *surface);

/*
 * User memory (16 KiB aligned, a whole number of pages) mapped into the
 * GPU as a buffer object: StarbiteMali's IMPORT_HOST, for PanVK's
 * VK_EXT_external_memory_host. The caller keeps its own mapping.
 */
int starbite_drm_import_host(int fd, void *addr, uint64_t size, uint32_t *handle);

/*
 * Sync objects: StarbiteMali runs every job chain to completion inside
 * SUBMIT, so each one is signalled as soon as it exists and every wait
 * returns at once.
 */
int drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle);
int drmSyncobjDestroy(int fd, uint32_t handle);
int drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count);
int drmSyncobjWait(int fd, uint32_t *handles, unsigned num_handles, int64_t timeout_nsec,
    unsigned flags, uint32_t *first_signaled);
int drmSyncobjTimelineWait(int fd, uint32_t *handles, uint64_t *points, unsigned num_handles,
    int64_t timeout_nsec, unsigned flags, uint32_t *first_signaled);
int drmSyncobjTransfer(int fd, uint32_t dst_handle, uint64_t dst_point, uint32_t src_handle,
    uint64_t src_point, uint32_t flags);
int drmSyncobjFDToHandle(int fd, int obj_fd, uint32_t *handle);
int drmSyncobjHandleToFD(int fd, uint32_t handle, int *obj_fd);
int drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd);
int drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd);

/*
 * Devices (PanVK enumerates through these): one platform device, the Mali, with a render node that
 * means nothing (/dev/null), as drmOpenWithType's descriptor.
 */
#define DRM_NODE_MAX            3
#define DRM_BUS_PCI             0
#define DRM_BUS_USB             1
#define DRM_BUS_PLATFORM        2
#define DRM_BUS_HOST1X          3
#define DRM_DEVICE_GET_PCI_REVISION (1 << 0)
typedef struct _drmPlatformBusInfo {
	char fullname[512];
} drmPlatformBusInfo, *drmPlatformBusInfoPtr;
typedef struct _drmPlatformDeviceInfo {
	char **compatible;
} drmPlatformDeviceInfo, *drmPlatformDeviceInfoPtr;
typedef struct _drmPciBusInfo {
	uint16_t domain;
	uint8_t bus, dev, func;
} drmPciBusInfo, *drmPciBusInfoPtr;
typedef struct _drmPciDeviceInfo {
	uint16_t vendor_id, device_id, subvendor_id, subdevice_id;
	uint8_t revision_id;
} drmPciDeviceInfo, *drmPciDeviceInfoPtr;
typedef struct _drmDevice {
	char **nodes;
	int available_nodes;
	int bustype;
	union {
		drmPciBusInfoPtr pci;
		void *usb, *host1x;
		drmPlatformBusInfoPtr platform;
	} businfo;
	union {
		drmPciDeviceInfoPtr pci;
		void *usb, *host1x;
		drmPlatformDeviceInfoPtr platform;
	} deviceinfo;
} drmDevice, *drmDevicePtr;
int drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices);
void drmFreeDevices(drmDevicePtr devices[], int count);
int drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device);
void drmFreeDevice(drmDevicePtr *device);
int drmDevicesEqual(drmDevicePtr a, drmDevicePtr b);
/* timelines: every point is reached at once too (jobs finish inside SUBMIT) */
int drmSyncobjTimelineSignal(int fd, const uint32_t *handles, uint64_t *points, uint32_t count);
int drmSyncobjQuery(int fd, uint32_t *handles, uint64_t *points, uint32_t count);
int drmSyncobjQuery2(int fd, uint32_t *handles, uint64_t *points, uint32_t count, uint32_t flags);
#endif
