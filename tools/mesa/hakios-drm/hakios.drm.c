/*
 * The libdrm of xf86drm.h (see there): Linux's DRM requests to the kernel's GPU driver as calls
 * of its IOKit user client. A request's number (the low byte of the ioctl code) is the call's
 * selector, its structure goes in and comes back as it is; a buffer's memory is the client
 * memory of the type that is the buffer's handle.
 *
 * Two of the kernel's services speak so, one for each GPU; a phone has one of them:
 *   HakiosGPU    the POCO X5 Pro's Adreno, as Linux's msm driver, for Mesa's freedreno
 *                (drivers/gpu/hakios.gpu.cpp)
 *   HakiosMali   the Galaxy S9's Mali-G72, as Linux's panfrost driver, for Mesa's panfrost
 *                (drivers/gpu/mali/hakios.mali.cpp)
 * drmOpenWithType() opens the one asked for by the name of its Linux driver, and drmGetVersion()
 * answers with that name: it is what Mesa's panfrost picks its backend by (pan_kmod.c). Every
 * request of freedreno's goes to the kernel as it is. Of panfrost's, two are seen to here first,
 * because HakiosMali does not do them as Linux's driver does (submit(), wait_buffer()), and its
 * fences are made here, of the kernel's job numbers (the sync objects at the end).
 *
 * The driver knows its device by a file descriptor. It gets one that means nothing (/dev/null):
 * there is one GPU and one connection to it in a process.
 */
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <IOKit/IOKitLib.h>

#define HAKIOS_DRM_DEVICES 1           /* (xf86drm.h: the device list is this file's to give) */
#include "xf86drm.h"
#include "drm-uapi/panfrost_drm.h"

#define HAKIOS_GPU_SURFACE     0x7e            /* the request that is not Linux's (both services) */
#define HAKIOS_GPU_DESCRIBE    0xffffffffu     /* ... asked only what the buffer would be like */
#define HAKIOS_MALI_WAIT       0x7d            /* HakiosMali's own: wait for a job by its number */

/* The kernel's services, by the names of the Linux drivers they stand in for. */
static const struct service {
	const char *name;               /* Linux's: what the driver opens, and what drmGetVersion() says */
	const char *service;            /* the kernel's */
	int major, minor;               /* the version of Linux's interface that it has */
	const char *desc;
} services[] = {
	/* 1.7: buffers at addresses the kernel chooses and tells ("softpin"), nothing newer */
	{ "msm", "HakiosGPU", 1, 7, "Adreno on Darwin" },
	/*
	 * 1.2: buffers that grow on a fault and buffers that are not run (1.1; the kernel takes the
	 * flags and makes every buffer whole), and the AFBC features (1.2; the kernel answers 0, which
	 * Mesa takes as AFBC supported: see hakios_mali_parameter()).
	 * Not 1.3: Mesa would then ask for the frequency of the GPU's timestamps, as a parameter it
	 * must have, and the kernel does not answer that one.
	 */
	{ "panfrost", "HakiosMali", 1, 2, "Mali on Darwin" },
};
#define PANFROST        (&services[1])

static const struct service *opened;
static io_connect_t connection;
static int connection_fd = -1;

static int noop_call(unsigned number, void *data, size_t size);
static void *noop_map(uint32_t handle);

/* HAKIOS_DRM_NOOP=1: no kernel, a made-up Mali-G72 in this process (xf86drm.h; the end of this file) */
int
hakios_drm_noop(void)
{
	static int noop = -1;

	if (noop < 0) {
		const char *v = getenv("HAKIOS_DRM_NOOP");
		noop = v != NULL && v[0] != '\0' && strcmp(v, "0") != 0;
	}
	return noop;
}

/* The connection to the service of the driver asked for, made once in a process: 0, or -1. */
static int
open_service(const struct service *wanted)
{
	io_service_t service;

	if (wanted == NULL || (opened != NULL && opened != wanted)) {
		return -1;
	}
	if (connection_fd >= 0) {
		return 0;
	}
	if (hakios_drm_noop()) {
		if (wanted != PANFROST) {
			return -1;
		}
	} else {
		service = IOServiceGetMatchingService(0, IOServiceMatching(wanted->service));
		if (service == 0) {
			return -1;
		}
		if (IOServiceOpen(service, mach_task_self(), 0, &connection) != 0) {
			IOObjectRelease(service);
			return -1;
		}
		IOObjectRelease(service);
	}
	opened = wanted;
	connection_fd = open("/dev/null", O_RDWR);
	return 0;
}

int
drmOpenWithType(const char *name, const char *busid, int type)
{
	const struct service *wanted = NULL;

	(void)busid;
	(void)type;
	for (size_t i = 0; i < sizeof(services) / sizeof(services[0]); i++) {
		if (strcmp(name, services[i].name) == 0) {
			wanted = &services[i];
		}
	}
	if (open_service(wanted) != 0) {
		return -1;
	}
	return dup(connection_fd);
}

static int
call(unsigned number, void *data, size_t size)
{
	size_t returned = size;
	kern_return_t result;

	if (hakios_drm_noop()) {
		return noop_call(number, data, size);
	}
	result = IOConnectCallStructMethod(connection, number, data, size, data, &returned);

	switch (result) {
	case 0:
		return 0;
	case kIOReturnNoMemory:
		return ENOMEM;
	case kIOReturnBadArgument:
		return EINVAL;
	case kIOReturnTimeout:
		return ETIMEDOUT;
	case kIOReturnBusy:
		return EBUSY;
	case kIOReturnNotFound:
		return ENOENT;
	case kIOReturnUnsupported:
		return ENOSYS;
	default:
		return EIO;
	}
}

static int submit(struct drm_panfrost_submit *request);
static int wait_buffer(struct drm_panfrost_wait_bo *request);

int
drmIoctl(int fd, unsigned long request, void *arg)
{
	unsigned number = (unsigned)(request & 0xff);
	int error;

	(void)fd;
	if (opened == PANFROST && number == DRM_COMMAND_BASE + DRM_PANFROST_SUBMIT) {
		error = submit(arg);
	} else if (opened == PANFROST && number == DRM_COMMAND_BASE + DRM_PANFROST_WAIT_BO) {
		error = wait_buffer(arg);
	} else {
		error = call(number, arg, IOCPARM_LEN(request));
	}
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmCommandWrite(int fd, unsigned long index, const void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), (void *)(uintptr_t)data, size);
}

int
drmCommandWriteRead(int fd, unsigned long index, void *data, unsigned long size)
{
	(void)fd;
	return -call((unsigned)(DRM_COMMAND_BASE + index), data, size);
}

/* The driver calls itself what Linux's is called, and is of the version whose interface it has. */
drmVersionPtr
drmGetVersion(int fd)
{
	drmVersionPtr version;

	(void)fd;
	if (opened == NULL) {
		errno = ENODEV;
		return NULL;
	}
	version = calloc(1, sizeof(*version));
	if (version != NULL) {
		version->version_major = opened->major;
		version->version_minor = opened->minor;
		version->name = strdup(opened->name);
		version->name_len = (int)strlen(opened->name);
		version->date = strdup("");
		version->desc = strdup(opened->desc);
		version->desc_len = (int)strlen(opened->desc);
	}
	return version;
}

void
drmFreeVersion(drmVersionPtr version)
{
	if (version != NULL) {
		free(version->name);
		free(version->date);
		free(version->desc);
		free(version);
	}
}

int
drmGetCap(int fd, uint64_t capability, uint64_t *value)
{
	(void)fd;
	(void)capability;
	*value = 0;
	return 0;
}

int
drmCloseBufferHandle(int fd, uint32_t handle)
{
	struct drm_gem_close request = { .handle = handle };

	return drmIoctl(fd, DRM_IOCTL_GEM_CLOSE, &request);
}

void *
hakios_drm_map(int fd, uint32_t handle, uint64_t size)
{
	mach_vm_address_t address = 0;
	mach_vm_size_t mapped = 0;

	(void)fd;
	(void)size;
	if (hakios_drm_noop()) {
		void *memory = noop_map(handle);
		return memory != NULL ? memory : (void *)-1;
	}
	/*
	 * Linux gives freedreno and panfrost their buffers write-combined, with no cache of this
	 * processor's between the program and the memory the GPU sees. So do the kernel's drivers
	 * here: the memory is made that way, and this mapping is as the memory is.
	 */
	if (IOConnectMapMemory64(connection, handle, mach_task_self(), &address, &mapped, kIOMapAnywhere) != 0) {
		return (void *)-1;              /* mmap()'s MAP_FAILED */
	}
	return (void *)(uintptr_t)address;
}

void
hakios_drm_unmap(int fd, uint32_t handle, void *address)
{
	(void)fd;
	if (hakios_drm_noop()) {
		return;                 /* (the memory goes with the buffer: GEM_CLOSE) */
	}
	IOConnectUnmapMemory64(connection, handle, mach_task_self(), (mach_vm_address_t)(uintptr_t)address);
}

int
hakios_drm_surface(int fd, struct hakios_drm_surface *surface)
{
	int error;

	(void)fd;
	surface->handle = HAKIOS_GPU_DESCRIBE;
	error = call(HAKIOS_GPU_SURFACE, surface, sizeof(*surface));
	if (error != 0) {
		errno = error;
		return -1;
	}
	return 0;
}

int
drmPrimeFDToHandle(int fd, int surface, uint32_t *handle)
{
	struct hakios_drm_surface request = { .surface = (uint32_t)surface };
	int error = call(HAKIOS_GPU_SURFACE, &request, sizeof(request));

	(void)fd;
	if (error != 0) {
		errno = error;
		return -1;
	}
	*handle = request.handle;
	return 0;
}

static int
none(void)
{
	errno = ENOSYS;
	return -1;
}

/* ---- the Mali's jobs ------------------------------------------------------------------------- */

/*
 * HakiosMali puts every job chain it is given into a queue, which a thread of its own runs one
 * chain at a time, in the order given; a submission comes back at once with the job's number, and
 * its request 0x7d waits for a number (struct mali_wait: the number, then how long at most). Since
 * the jobs end in that order, a job that is done is done with every job before it.
 */
static pthread_mutex_t lock = PTHREAD_MUTEX_INITIALIZER;
static uint64_t submitted;              /* the last job number the kernel gave (to this process) */
static uint64_t finished;               /* a job known to be done, and with it every one before it */

/*
 * Mesa's deadlines are moments of its clock (os_time_get_nano(): CLOCK_MONOTONIC on this system),
 * as Linux's are; the kernel's waits take a length of time. What is left until one, then.
 */
static int64_t
left_ns(int64_t deadline)
{
	struct timespec t;
	int64_t now;

	clock_gettime(CLOCK_MONOTONIC, &t);
	now = (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
	return deadline > now ? deadline - now : 0;
}

/*
 * Job `seq` done before `deadline` (INT64_MAX: whenever that is): 0, or an errno. The kernel waits
 * ten seconds at most at a time and is asked again until the deadline; a job does not run for that
 * long, since the kernel resets the GPU under one that has not ended within a second.
 */
static int
wait_job(uint64_t seq, int64_t deadline)
{
	struct {
		uint64_t seq;
		int64_t timeout_ns;
	} request;                              /* struct mali_wait */
	bool done;
	int error;

	pthread_mutex_lock(&lock);
	done = seq <= finished;
	pthread_mutex_unlock(&lock);
	if (done) {
		return 0;
	}
	do {
		request.seq = seq;
		request.timeout_ns = left_ns(deadline);
		error = call(HAKIOS_MALI_WAIT, &request, sizeof(request));
	} while (error == ETIMEDOUT && left_ns(deadline) > 0);
	if (error == 0) {
		pthread_mutex_lock(&lock);
		finished = seq > finished ? seq : finished;
		pthread_mutex_unlock(&lock);
	}
	return error;
}

static uint64_t *syncobj(uint32_t handle);

/*
 * A job chain to the GPU. Linux's driver gives back nothing and puts the job's fence into the sync
 * object out_sync; HakiosMali gives back the job's number where the chain's address was (jc). The
 * address is put back, as Linux leaves it, and the number goes into the sync object. The fences to
 * wait for before the job (in_syncs) are looked at neither here nor by the kernel: they can only
 * stand for jobs given before this one, which the queue has run by the time this one begins.
 */
static int
submit(struct drm_panfrost_submit *request)
{
	uint64_t chain = request->jc, seq, *fence;
	int error = call(DRM_COMMAND_BASE + DRM_PANFROST_SUBMIT, request, sizeof(*request));

	if (error != 0) {
		return error;
	}
	seq = request->jc;
	request->jc = chain;
	pthread_mutex_lock(&lock);
	submitted = seq > submitted ? seq : submitted;
	if (request->out_sync != 0 && (fence = syncobj(request->out_sync)) != NULL) {
		*fence = seq;
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

/*
 * Whether the GPU is done with a buffer. HakiosMali waits for the last job that named it, for a
 * length of time; Linux's timeout_ns is a moment (panfrost_drm.h: "absolute"), which panfrost gives
 * as 0 (only a look) or INT64_MAX (until it is done).
 */
static int
wait_buffer(struct drm_panfrost_wait_bo *request)
{
	struct drm_panfrost_wait_bo asked = *request;
	int error;

	do {
		asked.timeout_ns = left_ns(request->timeout_ns);
		error = call(DRM_COMMAND_BASE + DRM_PANFROST_WAIT_BO, &asked, sizeof(asked));
	} while (error == ETIMEDOUT && left_ns(request->timeout_ns) > 0);
	return error;
}

/* ---- sync objects ---------------------------------------------------------------------------- */

/*
 * Linux's fences that a program holds by a handle. Here, for panfrost, each is the number of the
 * job it waits for, kept in this process: the kernel has no fences, it has numbers. 0 is no job,
 * nothing to wait for -- a sync object made signalled, and one made without a fence (Linux would
 * refuse to wait on that, which panfrost never asks). Waiting for several is waiting for the
 * highest of their numbers (all of them) or the lowest (any), since the jobs end in order.
 * freedreno waits by requests of its own and has none of these, as before (ENOSYS).
 */
#define SYNCOBJ_FREE    UINT64_MAX
static uint64_t *syncobjs;              /* handle n is syncobjs[n - 1]: its job, or SYNCOBJ_FREE */
static uint32_t syncobj_count;

/* (with the lock held) NULL: no such sync object */
static uint64_t *
syncobj(uint32_t handle)
{
	if (handle == 0 || handle > syncobj_count || syncobjs[handle - 1] == SYNCOBJ_FREE) {
		return NULL;
	}
	return &syncobjs[handle - 1];
}

int
drmSyncobjCreate(int fd, uint32_t flags, uint32_t *handle)
{
	uint32_t i;

	(void)fd;
	(void)flags;            /* (signalled or without a fence: no job either way) */
	if (opened != PANFROST) {
		return none();
	}
	pthread_mutex_lock(&lock);
	for (i = 0; i < syncobj_count && syncobjs[i] != SYNCOBJ_FREE; i++) {
	}
	if (i == syncobj_count) {
		uint32_t count = syncobj_count != 0 ? 2 * syncobj_count : 64;
		uint64_t *grown = realloc(syncobjs, count * sizeof(*grown));

		if (grown == NULL) {
			pthread_mutex_unlock(&lock);
			errno = ENOMEM;
			return -1;
		}
		for (uint32_t k = syncobj_count; k < count; k++) {
			grown[k] = SYNCOBJ_FREE;
		}
		syncobjs = grown;
		syncobj_count = count;
	}
	syncobjs[i] = 0;
	*handle = i + 1;
	pthread_mutex_unlock(&lock);
	return 0;
}

int
drmSyncobjDestroy(int fd, uint32_t handle)
{
	uint64_t *fence;

	(void)fd;
	if (opened != PANFROST) {
		return none();
	}
	pthread_mutex_lock(&lock);
	if ((fence = syncobj(handle)) != NULL) {
		*fence = SYNCOBJ_FREE;
	}
	pthread_mutex_unlock(&lock);
	if (fence == NULL) {
		errno = EINVAL;
		return -1;
	}
	return 0;
}

int
drmSyncobjReset(int fd, const uint32_t *handles, uint32_t count)
{
	uint64_t *fence;

	(void)fd;
	if (opened != PANFROST) {
		return none();
	}
	pthread_mutex_lock(&lock);
	for (uint32_t i = 0; i < count; i++) {
		if ((fence = syncobj(handles[i])) != NULL) {
			*fence = 0;
		}
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

/* As libdrm's: 0, or minus an errno (ETIME: not done by the deadline, a moment of Mesa's clock). */
int
drmSyncobjWait(int fd, uint32_t *handles, unsigned count, int64_t timeout_nsec, unsigned flags,
    uint32_t *first_signaled)
{
	bool all = (flags & DRM_SYNCOBJ_WAIT_FLAGS_WAIT_ALL) != 0;
	uint64_t seq = 0, *fence;
	unsigned first = 0;
	int error;

	(void)fd;
	if (opened != PANFROST) {
		return -ENOSYS;
	}
	pthread_mutex_lock(&lock);
	for (unsigned i = 0; i < count; i++) {
		if ((fence = syncobj(handles[i])) == NULL) {
			pthread_mutex_unlock(&lock);
			return -EINVAL;
		}
		if (i == 0 || (all ? *fence > seq : *fence < seq)) {
			seq = *fence;
			first = i;
		}
	}
	pthread_mutex_unlock(&lock);
	error = wait_job(seq, timeout_nsec);
	if (error != 0) {
		return error == ETIMEDOUT ? -ETIME : -error;
	}
	if (first_signaled != NULL) {
		*first_signaled = first;
	}
	return 0;
}

/*
 * A sync object's fence as a file descriptor (a "sync file"): panfrost makes one of its context's
 * sync object for every fence it hands out, and takes it into a new sync object at once
 * (pan_fence.c). Here it is another descriptor of /dev/null, with the job number kept beside its
 * number. The program closes it when it likes, without a word, so a number that comes back may be
 * another descriptor by then: one made here again (the entry is made anew whenever a number is
 * made here, so it is the newest), or one that was not made here at all, which is taken for the
 * last job given to the GPU so far -- later than the one meant, if anything, never earlier.
 */
#define SYNC_FILES      64
static struct {
	bool used;
	int fd;
	uint64_t seq;
} sync_files[SYNC_FILES];
static unsigned sync_file_next;

int
drmSyncobjExportSyncFile(int fd, uint32_t handle, int *sync_file_fd)
{
	uint64_t *fence;
	int file;

	(void)fd;
	if (opened != PANFROST) {
		return none();
	}
	if ((file = fcntl(connection_fd, F_DUPFD_CLOEXEC, 0)) < 0) {
		return -1;
	}
	pthread_mutex_lock(&lock);
	if ((fence = syncobj(handle)) == NULL) {
		pthread_mutex_unlock(&lock);
		close(file);
		errno = EINVAL;
		return -1;
	}
	for (unsigned i = 0; i < SYNC_FILES; i++) {
		if (sync_files[i].used && sync_files[i].fd == file) {
			sync_files[i].used = false;     /* (a number made here before, closed since) */
		}
	}
	sync_files[sync_file_next % SYNC_FILES].used = true;
	sync_files[sync_file_next % SYNC_FILES].fd = file;
	sync_files[sync_file_next % SYNC_FILES].seq = *fence;
	sync_file_next++;
	pthread_mutex_unlock(&lock);
	*sync_file_fd = file;
	return 0;
}

int
drmSyncobjImportSyncFile(int fd, uint32_t handle, int sync_file_fd)
{
	uint64_t *fence, seq;

	(void)fd;
	if (opened != PANFROST) {
		return none();
	}
	pthread_mutex_lock(&lock);
	if ((fence = syncobj(handle)) == NULL) {
		pthread_mutex_unlock(&lock);
		errno = EINVAL;
		return -1;
	}
	seq = submitted;
	for (unsigned i = 0; i < SYNC_FILES; i++) {
		if (sync_files[i].used && sync_files[i].fd == sync_file_fd) {
			seq = sync_files[i].seq;
		}
	}
	*fence = seq;
	pthread_mutex_unlock(&lock);
	return 0;
}

/*
 * Buffers and sync objects as file descriptors of their own, for handing them to other programs and
 * drivers, and timelines (panthor's): none of that here.
 */
#define NONE(name, ...) int name(__VA_ARGS__) { return none(); }
NONE(drmPrimeHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) uint32_t flags, __attribute__((unused)) int *prime_fd)
NONE(drmSyncobjFDToHandle, __attribute__((unused)) int fd, __attribute__((unused)) int obj_fd,
    __attribute__((unused)) uint32_t *handle)
NONE(drmSyncobjHandleToFD, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t handle,
    __attribute__((unused)) int *obj_fd)
NONE(drmSyncobjTimelineWait, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t *handles,
    __attribute__((unused)) uint64_t *points, __attribute__((unused)) unsigned count,
    __attribute__((unused)) int64_t timeout_nsec, __attribute__((unused)) unsigned flags,
    __attribute__((unused)) uint32_t *first_signaled)
NONE(drmSyncobjTransfer, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t dst_handle,
    __attribute__((unused)) uint64_t dst_point, __attribute__((unused)) uint32_t src_handle,
    __attribute__((unused)) uint64_t src_point, __attribute__((unused)) uint32_t flags)
NONE(drmSyncobjTimelineSignal, __attribute__((unused)) int fd, __attribute__((unused)) const uint32_t *handles,
    __attribute__((unused)) uint64_t *points, __attribute__((unused)) uint32_t count)
NONE(drmSyncobjQuery, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t *handles,
    __attribute__((unused)) uint64_t *points, __attribute__((unused)) uint32_t count)
NONE(drmSyncobjQuery2, __attribute__((unused)) int fd, __attribute__((unused)) uint32_t *handles,
    __attribute__((unused)) uint64_t *points, __attribute__((unused)) uint32_t count,
    __attribute__((unused)) uint32_t flags)

/*
 * Signalled by the CPU: no job to wait for, as a sync object made signalled (0). Vulkan's common
 * code calls this for vkSignalSemaphore and for fences and semaphores it signals itself.
 */
int
drmSyncobjSignal(int fd, const uint32_t *handles, uint32_t count)
{
	uint64_t *fence;

	(void)fd;
	if (opened != PANFROST) {
		return none();
	}
	pthread_mutex_lock(&lock);
	for (uint32_t i = 0; i < count; i++) {
		if ((fence = syncobj(handles[i])) == NULL) {
			pthread_mutex_unlock(&lock);
			errno = EINVAL;
			return -1;
		}
	}
	for (uint32_t i = 0; i < count; i++) {
		*syncobj(handles[i]) = 0;
	}
	pthread_mutex_unlock(&lock);
	return 0;
}

/* ---- the devices there are (xf86drm.h) ------------------------------------------------------- */

static drmDevicePtr
mali_device(void)
{
	drmDevicePtr device;

	/* one allocation: the device, its node table, its bus and device information, the strings */
	size_t size = sizeof(*device) + DRM_NODE_MAX * sizeof(char *) + sizeof(drmPlatformBusInfo) +
	    sizeof(drmPlatformDeviceInfo) + 2 * sizeof(char *) + sizeof("/dev/null") + sizeof("arm,mali-bifrost");
	char *p = calloc(1, size);

	if (p == NULL) {
		return NULL;
	}
	device = (drmDevicePtr)(void *)p;
	p += sizeof(*device);
	device->nodes = (char **)(void *)p;
	p += DRM_NODE_MAX * sizeof(char *);
	device->businfo.platform = (drmPlatformBusInfoPtr)(void *)p;
	p += sizeof(drmPlatformBusInfo);
	device->deviceinfo.platform = (drmPlatformDeviceInfoPtr)(void *)p;
	p += sizeof(drmPlatformDeviceInfo);
	device->deviceinfo.platform->compatible = (char **)(void *)p;
	p += 2 * sizeof(char *);
	device->nodes[DRM_NODE_RENDER] = strcpy(p, "/dev/null");
	p += sizeof("/dev/null");
	device->deviceinfo.platform->compatible[0] = strcpy(p, "arm,mali-bifrost");
	strcpy(device->businfo.platform->fullname, "/mali@17500000");
	device->available_nodes = 1 << DRM_NODE_RENDER;
	device->bustype = DRM_BUS_PLATFORM;
	return device;
}

/*
 * The Mali, when HakiosMali is there (or HAKIOS_DRM_NOOP is set), and nothing else: the Adreno's
 * service speaks msm, which no Vulkan driver here opens by enumeration. With devices NULL, how many
 * there are, as libdrm.
 */
int
drmGetDevices2(uint32_t flags, drmDevicePtr devices[], int max_devices)
{
	(void)flags;
	if (open_service(PANFROST) != 0) {
		return 0;
	}
	if (devices == NULL) {
		return 1;
	}
	if (max_devices < 1) {
		return 0;
	}
	if ((devices[0] = mali_device()) == NULL) {
		return -ENOMEM;
	}
	return 1;
}

int
drmGetDevices(drmDevicePtr devices[], int max_devices)
{
	return drmGetDevices2(0, devices, max_devices);
}

int
drmGetDevice2(int fd, uint32_t flags, drmDevicePtr *device)
{
	(void)fd;
	(void)flags;
	if (opened != PANFROST) {
		return -ENODEV;
	}
	return (*device = mali_device()) != NULL ? 0 : -ENOMEM;
}

void
drmFreeDevice(drmDevicePtr *device)
{
	if (device != NULL) {
		free(*device);
		*device = NULL;
	}
}

/* there is one device: two descriptions of a device are the same one when both are there */
int
drmDevicesEqual(drmDevicePtr a, drmDevicePtr b)
{
	return a != NULL && b != NULL && a->bustype == b->bustype;
}

void
drmFreeDevices(drmDevicePtr devices[], int count)
{
	for (int i = 0; devices != NULL && i < count; i++) {
		drmFreeDevice(&devices[i]);
	}
}

/* ---- HAKIOS_DRM_NOOP: a Mali-G72 that runs nothing ------------------------------------------- */

/*
 * What HakiosMali answers, made up in this process (see hakios_drm_noop() in xf86drm.h). The
 * parameters are those of tools/mali-drmshim/hakios_mali_shim.c, the Linux drm-shim check of the
 * same GPU, where each value and the reason for it is written down: a Mali-G72 MP18 as the S9's
 * (GPU_ID 0x6221). Buffers are this process's memory at made-up GPU addresses between 32 MB and
 * 4 GB, as the kernel's are (drivers/gpu/mali/hakios.mali_va.h); a job is done as soon as it is
 * given, and none is run.
 */
static struct noop_buffer {
	void *memory;
	uint64_t size, address;
} *noop_buffers;
static uint32_t noop_count;
static uint64_t noop_next = 32ull << 20;
static uint64_t noop_jobs;

static void *
noop_map(uint32_t handle)
{
	void *memory = NULL;

	pthread_mutex_lock(&lock);
	if (handle != 0 && handle <= noop_count) {
		memory = noop_buffers[handle - 1].memory;
	}
	pthread_mutex_unlock(&lock);
	return memory;
}

static int
noop_param(struct drm_panfrost_get_param *param)
{
	static const struct {
		uint32_t param;
		uint64_t value;
	} g72[] = {
		{ DRM_PANFROST_PARAM_GPU_PROD_ID, 0x6221 },
		{ DRM_PANFROST_PARAM_GPU_REVISION, 0x0030 },
		{ DRM_PANFROST_PARAM_SHADER_PRESENT, 0x3ffff },
		{ DRM_PANFROST_PARAM_TILER_PRESENT, 0x1 },
		{ DRM_PANFROST_PARAM_L2_PRESENT, 0x1 },
		{ DRM_PANFROST_PARAM_AS_PRESENT, 0xff },
		{ DRM_PANFROST_PARAM_JS_PRESENT, 0x7 },
		{ DRM_PANFROST_PARAM_L2_FEATURES, 0x07120206 },
		{ DRM_PANFROST_PARAM_TILER_FEATURES, 0x809 },
		{ DRM_PANFROST_PARAM_MEM_FEATURES, 0x1 },
		{ DRM_PANFROST_PARAM_MMU_FEATURES, 0x2830 },
		{ DRM_PANFROST_PARAM_MAX_THREADS, 384 },
		{ DRM_PANFROST_PARAM_THREAD_MAX_WORKGROUP_SZ, 384 },
		{ DRM_PANFROST_PARAM_THREAD_MAX_BARRIER_SZ, 384 },
		{ DRM_PANFROST_PARAM_COHERENCY_FEATURES, 0x1 },
		{ DRM_PANFROST_PARAM_TEXTURE_FEATURES0, 0xffffffff },
		{ DRM_PANFROST_PARAM_TEXTURE_FEATURES1, 0xffffffff },
		{ DRM_PANFROST_PARAM_TEXTURE_FEATURES2, 0xffffffff },
		{ DRM_PANFROST_PARAM_TEXTURE_FEATURES3, 0xffffffff },
		{ DRM_PANFROST_PARAM_NR_CORE_GROUPS, 1 },
		{ DRM_PANFROST_PARAM_THREAD_TLS_ALLOC, 384 },
	};

	param->value = 0;               /* the rest (stack, core features, thread features, AFBC): 0 */
	if (param->param >= DRM_PANFROST_PARAM_JS_FEATURES0 && param->param <= DRM_PANFROST_PARAM_JS_FEATURES15) {
		param->value = param->param - DRM_PANFROST_PARAM_JS_FEATURES0 < 3 ? 0x20e : 0;
	}
	for (size_t i = 0; i < sizeof(g72) / sizeof(g72[0]); i++) {
		if (g72[i].param == param->param) {
			param->value = g72[i].value;
		}
	}
	return 0;
}

static int
noop_call(unsigned number, void *data, size_t size)
{
	int error = 0;

	(void)size;
	pthread_mutex_lock(&lock);
	switch (number) {
	case DRM_COMMAND_BASE + DRM_PANFROST_GET_PARAM:
		error = noop_param(data);
		break;
	case DRM_COMMAND_BASE + DRM_PANFROST_CREATE_BO: {
		struct drm_panfrost_create_bo *create = data;
		uint64_t length = (create->size + 4095) & ~4095ull;
		struct noop_buffer *grown;
		void *memory;

		if (length == 0 || noop_next + length > (4ull << 30) ||
		    (memory = calloc(1, (size_t)length)) == NULL) {
			error = ENOMEM;
			break;
		}
		grown = realloc(noop_buffers, (noop_count + 1) * sizeof(*grown));
		if (grown == NULL) {
			free(memory);
			error = ENOMEM;
			break;
		}
		noop_buffers = grown;
		/* code is kept off 16 MB lines (hakios.mali_va.h): every buffer starts on a 2 MB line here */
		noop_buffers[noop_count] = (struct noop_buffer){ memory, length, noop_next };
		create->handle = ++noop_count;
		create->offset = noop_next;
		noop_next = (noop_next + length + (2ull << 20) - 1) & ~((2ull << 20) - 1);
		break;
	}
	case DRM_COMMAND_BASE + DRM_PANFROST_MMAP_BO: {
		struct drm_panfrost_mmap_bo *map = data;

		map->offset = (uint64_t)map->handle << 12;
		break;
	}
	case DRM_COMMAND_BASE + DRM_PANFROST_GET_BO_OFFSET: {
		struct drm_panfrost_get_bo_offset *offset = data;

		if (offset->handle == 0 || offset->handle > noop_count) {
			error = EINVAL;
		} else {
			offset->offset = noop_buffers[offset->handle - 1].address;
		}
		break;
	}
	case DRM_COMMAND_BASE + DRM_PANFROST_SUBMIT: {
		struct drm_panfrost_submit *submit = data;

		submit->jc = ++noop_jobs;       /* (submit() above puts the chain back) */
		break;
	}
	case DRM_COMMAND_BASE + DRM_PANFROST_MADVISE: {
		struct drm_panfrost_madvise *madvise = data;

		madvise->retained = 1;
		break;
	}
	case 0x09: {                         /* DRM_IOCTL_GEM_CLOSE */
		struct drm_gem_close *close = data;

		if (close->handle != 0 && close->handle <= noop_count) {
			free(noop_buffers[close->handle - 1].memory);
			noop_buffers[close->handle - 1].memory = NULL;
		}
		break;
	}
	case DRM_COMMAND_BASE + DRM_PANFROST_WAIT_BO:
	case HAKIOS_MALI_WAIT:
		break;                          /* every job is done */
	default:
		error = ENOSYS;                 /* IOSurfaces (0x7e), performance counters */
		break;
	}
	pthread_mutex_unlock(&lock);
	return error;
}
