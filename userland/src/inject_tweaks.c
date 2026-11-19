/*
 * libhakios.tweaks.dylib: the tweak loader of this port.
 *
 * A tweak is a dylib that a package manager put in /var/jb/usr/lib/TweakInject, beside a property
 * list of the same name whose Filter says which programs it belongs in. Something has to be inside
 * every program to read that directory and open the tweaks that name it; on a jailbroken phone the
 * jailbreak's own hook does it, and ElleKit ships one (ellekit/libinjector.dylib). That one opens
 * here but loads nothing and says nothing about why, so this is ours, which says what it does.
 *
 * ElleKit is still what does the work a tweak asks for: the tweaks link against
 * /var/jb/usr/lib/libsubstrate.dylib (libhooker, CydiaSubstrate), which is ElleKit's libellekit,
 * and that is the engine that replaces functions and methods. Only the finding and opening is ours.
 *
 * It goes into a program either as the one library the kernel inserts (etc/inject.conf, see
 * src/init.c) or because a library of ours that is already there opens it (src/inject_sb.c does,
 * for SpringBoard). Loading it twice is harmless: the second time does nothing.
 *
 * A Filter says any of:
 *   Bundles       the identifier of the program's main bundle (com.apple.springboard)
 *   Executables   the file name of the program (SpringBoard)
 *   Classes       an Objective-C class the program has
 * and takes the tweak if any of them matches, unless Mode is "Any", where all must. A list with no
 * Filter at all belongs in every program, as it does on a phone.
 *
 * Being the one library of ours in Settings, it also carries what Settings itself needs here
 * (location_services_row below).
 */
#include <CoreFoundation/CoreFoundation.h>
#include <dirent.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <mach-o/dyld.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/mount.h>
#include <sys/attr.h>
#include <unistd.h>

/*
 * Both places a package may put a tweak: the one ElleKit uses now, and the one Cydia Substrate used,
 * which is a symbolic link to it on a phone -- but a package unpacked on this machine can leave it a
 * real directory, and some packages still write there. Looking in both costs nothing.
 */
static const char *const tweak_dirs[] = {
	"/var/jb/usr/lib/TweakInject",
	"/var/jb/Library/MobileSubstrate/DynamicLibraries",
};

static void
say(const char *format, ...)
{
	char line[512];
	va_list args;
	int n, fd;

	va_start(args, format);
	n = vsnprintf(line, sizeof(line), format, args);
	va_end(args);
	if (n <= 0) {
		return;
	}
	fd = open("/dev/console", O_WRONLY | O_NOCTTY);
	if (fd >= 0) {
		(void)write(fd, line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
		close(fd);
	}
}

/* The name this program was started as, and its bundle identifier when it has one yet. */
static const char *
executable_name(void)
{
	static char path[1024];
	uint32_t size = sizeof(path);
	char *slash;

	if (_NSGetExecutablePath(path, &size) != 0) {
		return NULL;
	}
	slash = strrchr(path, '/');
	return slash != NULL ? slash + 1 : path;
}

static bool
bundle_identifier(char *out, size_t size)
{
	CFBundleRef bundle = CFBundleGetMainBundle();
	CFStringRef identifier;

	if (bundle == NULL) {
		return false;
	}
	identifier = CFBundleGetIdentifier(bundle);
	return identifier != NULL && CFStringGetCString(identifier, out, (CFIndex)size, kCFStringEncodingUTF8);
}

/* Does this array of strings hold `want`? */
static bool
array_has(CFArrayRef array, const char *want)
{
	CFIndex count = array != NULL ? CFArrayGetCount(array) : 0;

	for (CFIndex i = 0; i < count; i++) {
		CFStringRef string = CFArrayGetValueAtIndex(array, i);
		char text[256];

		if (CFGetTypeID(string) == CFStringGetTypeID() &&
		    CFStringGetCString(string, text, sizeof(text), kCFStringEncodingUTF8) && strcmp(text, want) == 0) {
			return true;
		}
	}
	return false;
}

static bool
array_has_class(CFArrayRef array)
{
	CFIndex count = array != NULL ? CFArrayGetCount(array) : 0;

	for (CFIndex i = 0; i < count; i++) {
		CFStringRef string = CFArrayGetValueAtIndex(array, i);
		char text[256];

		if (CFGetTypeID(string) == CFStringGetTypeID() &&
		    CFStringGetCString(string, text, sizeof(text), kCFStringEncodingUTF8) && objc_getClass(text) != NULL) {
			return true;
		}
	}
	return false;
}

static CFArrayRef
filter_array(CFDictionaryRef filter, const char *key)
{
	CFStringRef name = CFStringCreateWithCString(NULL, key, kCFStringEncodingUTF8);
	CFArrayRef array = NULL;

	if (name != NULL) {
		CFTypeRef value = CFDictionaryGetValue(filter, name);

		if (value != NULL && CFGetTypeID(value) == CFArrayGetTypeID()) {
			array = value;
		}
		CFRelease(name);
	}
	return array;
}

/* Whether the list at `path` names this program. */
static bool
wanted_here(const char *path, const char *name, const char *identifier)
{
	CFDictionaryRef plist, filter;
	CFArrayRef bundles, executables, classes;
	CFStringRef mode_key, filter_key;
	CFDataRef data;
	CFTypeRef mode;
	bool all, hits[3], wanted;
	int fd, used = 0;
	uint8_t buffer[65536];
	ssize_t n;

	fd = open(path, O_RDONLY);
	n = fd >= 0 ? read(fd, buffer, sizeof(buffer)) : -1;
	if (fd >= 0) {
		close(fd);
	}
	if (n <= 0) {
		return false;
	}
	data = CFDataCreate(NULL, buffer, (CFIndex)n);
	if (data == NULL) {
		return false;
	}
	plist = (CFDictionaryRef)CFPropertyListCreateWithData(NULL, data, kCFPropertyListImmutable, NULL, NULL);
	CFRelease(data);
	if (plist == NULL || CFGetTypeID(plist) != CFDictionaryGetTypeID()) {
		if (plist != NULL) {
			CFRelease(plist);
		}
		say("hakios: tweaks: %s is not a property list\n", path);
		return false;
	}
	filter_key = CFSTR("Filter");
	filter = CFDictionaryGetValue(plist, filter_key);
	if (filter == NULL || CFGetTypeID(filter) != CFDictionaryGetTypeID()) {
		CFRelease(plist);                       /* no Filter: it belongs everywhere */
		return true;
	}
	bundles = filter_array(filter, "Bundles");
	executables = filter_array(filter, "Executables");
	classes = filter_array(filter, "Classes");
	hits[0] = bundles != NULL && identifier != NULL && array_has(bundles, identifier);
	hits[1] = executables != NULL && name != NULL && array_has(executables, name);
	hits[2] = classes != NULL && array_has_class(classes);
	/* Mode "Any" means every kind that is given has to match; by default one is enough. */
	mode_key = CFSTR("Mode");
	mode = CFDictionaryGetValue(filter, mode_key);
	all = mode != NULL && CFGetTypeID(mode) == CFStringGetTypeID() &&
	    CFStringCompare(mode, CFSTR("Any"), 0) == kCFCompareEqualTo;
	used = (bundles != NULL) + (executables != NULL) + (classes != NULL);
	if (used == 0) {
		wanted = true;
	} else if (all) {
		wanted = (bundles == NULL || hits[0]) && (executables == NULL || hits[1]) && (classes == NULL || hits[2]);
	} else {
		wanted = hits[0] || hits[1] || hits[2];
	}
	CFRelease(plist);
	return wanted;
}

static void
load_the_tweaks(void)
{
	char identifier[256];
	const char *name = executable_name();
	const char *who = bundle_identifier(identifier, sizeof(identifier)) ? identifier : NULL;
	int opened = 0, tried = 0;

	for (unsigned d = 0; d < sizeof(tweak_dirs) / sizeof(tweak_dirs[0]); d++) {
		const char *where = tweak_dirs[d];
		struct dirent *entry;
		struct stat st;
		DIR *dir;

		/* the second is usually a link to the first: then it is the same directory, done already */
		if (d > 0 && lstat(where, &st) == 0 && S_ISLNK(st.st_mode)) {
			continue;
		}
		dir = opendir(where);
		if (dir == NULL) {
			continue;                       /* no prefix, or no tweaks in it */
		}
		while ((entry = readdir(dir)) != NULL) {
			char plist[1024], dylib[1024];
			size_t length = strlen(entry->d_name);

			if (length < 7 || strcmp(entry->d_name + length - 6, ".plist") != 0) {
				continue;
			}
			snprintf(plist, sizeof(plist), "%s/%s", where, entry->d_name);
			snprintf(dylib, sizeof(dylib), "%s/%.*s.dylib", where, (int)(length - 6), entry->d_name);
			if (!wanted_here(plist, name, who)) {
				continue;
			}
			tried++;
			if (dlopen(dylib, RTLD_NOW | RTLD_GLOBAL) != NULL) {
				opened++;
				say("hakios: tweaks: %s\n", dylib);
			} else {
				say("hakios: tweaks: %s: %s\n", dylib, dlerror());
			}
		}
		closedir(dir);
	}
	if (tried > 0) {
		say("hakios: tweaks: %s (%s): %d of %d opened\n", name != NULL ? name : "?",
		    who != NULL ? who : "no bundle", opened, tried);
	}
}

/*
 * Settings > Privacy & Security shows "Location Services" only once it has asked every SIM's carrier
 * bundle whether location services are allowed (-[PUIPrivacyController refreshLocationServicesAvailable],
 * PrivacySettingsUI, key LocationServicesAvailable): with no SIM, or no carrier saying otherwise, the answer
 * is yes. It asks CommCenter, which this system does not run, so the question fails ("the connection to
 * service named com.apple.commcenter.coretelephony.xpc was invalidated") and Settings returns without
 * an answer -- and with none, it leaves the row out. When it has no answer after asking, it gets the
 * one it would have had from a phone without a SIM.
 */
static IMP refresh_location_original;

static void
refresh_location(id self, SEL cmd)
{
	SEL get = sel_registerName("_cachedLocationServicesAvailableValue");
	SEL set = sel_registerName("set_cachedLocationServicesAvailableValue:");

	((void (*)(id, SEL))refresh_location_original)(self, cmd);
	if (((id (*)(id, SEL))objc_msgSend)(self, get) == nil) {
		((void (*)(id, SEL, id))objc_msgSend)(self, set, (id)kCFBooleanTrue);
	}
}

static void
location_services_row(void)
{
	Class privacy;
	Method method;

	/* in the shared cache, and Settings opens it for that pane anyway; opened now, it is hooked before it is used */
	dlopen("/System/Library/PrivateFrameworks/Settings/PrivacySettingsUI.framework/PrivacySettingsUI", RTLD_NOW);
	privacy = objc_getClass("PUIPrivacyController");
	method = privacy != NULL ? class_getInstanceMethod(privacy, sel_registerName("refreshLocationServicesAvailable")) : NULL;
	if (method == NULL || class_getInstanceMethod(privacy, sel_registerName("set_cachedLocationServicesAvailableValue:")) == NULL ||
	    class_getInstanceMethod(privacy, sel_registerName("_cachedLocationServicesAvailableValue")) == NULL) {
		say("hakios: tweaks: no -[PUIPrivacyController refreshLocationServicesAvailable], Location Services left as it is\n");
		return;
	}
	refresh_location_original = method_setImplementation(method, (IMP)refresh_location);
}

__attribute__((constructor))
static void
hakios_tweaks_init(void)
{
	static bool done;

	if (done) {
		return;
	}
	done = true;
	if (executable_name() != NULL && strcmp(executable_name(), "Preferences") == 0) {
		location_services_row();
	}
	load_the_tweaks();
}

/*
 * Settings' storage (General > iPhone Storage) takes the phone's capacity from MobileGestalt's DiskUsage, which counts the
 * root volume's container: here the system image (about 4 GB), with /private/var a volume of its own (HFS Plus, all of
 * USERDATA: ~118 GB) that the iPhone's single APFS container would have held. So the answer is given the data volume's
 * numbers (and the two together as the disk's), when /private/var is that kept volume; otherwise it is left as it is.
 */
extern CFTypeRef MGCopyAnswer(CFStringRef key, CFDictionaryRef options);
extern CFTypeRef MGCopyAnswerWithError(CFStringRef key, CFDictionaryRef options, int *error);

static void
set_i64(CFMutableDictionaryRef d, CFStringRef key, long long v)
{
	CFNumberRef n = CFNumberCreate(NULL, kCFNumberLongLongType, &v);

	if (n != NULL) {
		CFDictionarySetValue(d, key, n);
		CFRelease(n);
	}
}

static CFNumberRef
i64(long long v)
{
	return CFNumberCreate(NULL, kCFNumberLongLongType, &v);
}

/* the kept volume's numbers: 0 when /private/var is not it */
static int
kept_sizes(long long *data_cap, long long *data_free, long long *sys_cap, long long *sys_free)
{
	struct statfs root, var;

	if (statfs("/private/var", &var) != 0 || strcmp(var.f_fstypename, "hfs") != 0 || statfs("/", &root) != 0) {
		return 0;
	}
	*data_cap = (long long)var.f_blocks * var.f_bsize;
	*data_free = (long long)var.f_bavail * var.f_bsize;
	*sys_cap = (long long)root.f_blocks * root.f_bsize;
	*sys_free = (long long)root.f_bavail * root.f_bsize;
	return 1;
}

static void
say_key_once(CFStringRef key, const char *how)
{
	static CFMutableSetRef said;
	char name[96] = "";

	if (said == NULL) {
		said = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (said == NULL || CFSetContainsValue(said, key)) {
		return;
	}
	CFSetAddValue(said, key);
	CFStringGetCString(key, name, sizeof(name), kCFStringEncodingUTF8);
	say("hakios: tweaks: MobileGestalt asked for %s (%s) in %s\n", name, how, getprogname());
}

/* the answer to give instead, or NULL to give the real one */
static CFTypeRef
disk_answer(CFStringRef key, CFTypeRef real)
{
	long long dc, df, sc, sf;

	if (key == NULL || CFGetTypeID(key) != CFStringGetTypeID()) {
		return NULL;
	}
	if (CFStringFind(key, CFSTR("Disk"), 0).location != kCFNotFound || CFStringFind(key, CFSTR("Capacity"), 0).location !=
	    kCFNotFound || CFStringFind(key, CFSTR("Available"), 0).location != kCFNotFound) {
		say_key_once(key, real != NULL ? "answered" : "no answer");
	}
	if (!kept_sizes(&dc, &df, &sc, &sf)) {
		return NULL;
	}
	if (CFEqual(key, CFSTR("DiskUsage")) && real != NULL && CFGetTypeID(real) == CFDictionaryGetTypeID()) {
		CFMutableDictionaryRef d = CFDictionaryCreateMutableCopy(NULL, 0, real);

		if (d == NULL) {
			return NULL;
		}
		set_i64(d, CFSTR("TotalDataCapacity"), dc);
		set_i64(d, CFSTR("TotalDataAvailable"), df);
		set_i64(d, CFSTR("AmountDataAvailable"), df);
		set_i64(d, CFSTR("TotalSystemCapacity"), sc);
		set_i64(d, CFSTR("TotalSystemAvailable"), sf);
		set_i64(d, CFSTR("TotalDiskCapacity"), dc + sc);
		return d;
	}
	if (CFEqual(key, CFSTR("TotalDiskCapacity"))) {
		return i64(dc + sc);
	}
	if (CFEqual(key, CFSTR("TotalDataCapacity"))) {
		return i64(dc);
	}
	if (CFEqual(key, CFSTR("TotalDataAvailable")) || CFEqual(key, CFSTR("AmountDataAvailable"))) {
		return i64(df);
	}
	if (CFEqual(key, CFSTR("TotalSystemCapacity"))) {
		return i64(sc);
	}
	if (CFEqual(key, CFSTR("TotalSystemAvailable"))) {
		return i64(sf);
	}
	return NULL;
}

/*
 * Settings' storage (General > iPhone Storage) takes the capacity from MobileGestalt, which counts the root volume's
 * container: the system image here (about 4 GB). /private/var is a volume of its own (HFS Plus, all of USERDATA, ~118 GB)
 * that the iPhone's one APFS container would have held. So the disk's answers are the two volumes' together, when
 * /private/var is that kept volume; every key with Disk, Capacity or Available in its name is said once, for finding
 * the one that is read.
 */
static CFTypeRef
my_MGCopyAnswer(CFStringRef key, CFDictionaryRef options)
{
	CFTypeRef v = MGCopyAnswer(key, options), w = disk_answer(key, v);

	if (w != NULL) {
		if (v != NULL) {
			CFRelease(v);
		}
		return w;
	}
	return v;
}

static CFTypeRef
my_MGCopyAnswerWithError(CFStringRef key, CFDictionaryRef options, int *error)
{
	CFTypeRef v = MGCopyAnswerWithError(key, options, error), w = disk_answer(key, v);

	if (w != NULL) {
		if (v != NULL) {
			CFRelease(v);
		}
		if (error != NULL) {
			*error = 0;
		}
		return w;
	}
	return v;
}

__attribute__((constructor)) static void
hakios_tweaks_mg_hello(void)
{
	say("hakios: tweaks: in %s (MobileGestalt's disk answers watched)\n", getprogname());
}

__attribute__((used)) static const struct { const void *replacement, *replacee; } hakios_mg_interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)my_MGCopyAnswer, (const void *)MGCopyAnswer },
	{ (const void *)my_MGCopyAnswerWithError, (const void *)MGCopyAnswerWithError },
};

/*
 * MobileGestalt was not asked (seen 14 November 2026): Settings' storage reads the root volume's size from the file system.
 * In Settings alone, a statfs that lands on the root volume (the system image) is given the disk's size as the iPhone's one
 * container would have it: both volumes' blocks, and the kept volume's free space. Every path asked is said once.
 */
extern int statfs(const char *, struct statfs *);
extern int fstatfs(int, struct statfs *);

static int
in_settings(void)
{
	static int is = -1;

	if (is < 0) {
		const char *name = getprogname();

		is = name != NULL && strcmp(name, "Preferences") == 0;
	}
	return is;
}

static void
fix_root(const char *what, struct statfs *sf)
{
	static CFMutableSetRef said;
	struct statfs var;

	if (!in_settings() || strcmp(sf->f_mntonname, "/") != 0) {
		return;
	}
	if (said == NULL) {
		said = CFSetCreateMutable(NULL, 0, &kCFTypeSetCallBacks);
	}
	if (said != NULL && what != NULL) {
		CFStringRef k = CFStringCreateWithCString(NULL, what, kCFStringEncodingUTF8);

		if (k != NULL && !CFSetContainsValue(said, k)) {
			CFSetAddValue(said, k);
			say("hakios: tweaks: Settings asks the size of the root volume through %s\n", what);
		}
		if (k != NULL) {
			CFRelease(k);
		}
	}
	if (statfs("/private/var", &var) != 0 || strcmp(var.f_fstypename, "hfs") != 0 || sf->f_bsize == 0) {
		return;
	}
	unsigned long long var_blocks = (unsigned long long)var.f_blocks * var.f_bsize / sf->f_bsize;
	unsigned long long var_free = (unsigned long long)var.f_bavail * var.f_bsize / sf->f_bsize;

	sf->f_blocks += var_blocks;
	sf->f_bfree = var_free;
	sf->f_bavail = var_free;
}

static int
my_statfs(const char *path, struct statfs *sf)
{
	int r = statfs(path, sf);

	if (r == 0) {
		fix_root(path, sf);
	}
	return r;
}

static int
my_fstatfs(int fd, struct statfs *sf)
{
	int r = fstatfs(fd, sf);

	if (r == 0) {
		fix_root("fstatfs", sf);
	}
	return r;
}

__attribute__((used)) static const struct { const void *replacement, *replacee; } hakios_statfs_interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)my_statfs, (const void *)statfs },
	{ (const void *)my_fstatfs, (const void *)fstatfs },
};

/*
 * statfs was fixed and the header still said 4 GB (14 November 2026): the capacity comes through getattrlist (NSURL's volume
 * capacity keys). In Settings alone, a getattrlist of a volume's space on the root volume gets the same both-volumes answer.
 * Only the common case is touched: no common attributes but ATTR_CMN_RETURNED_ATTRS, and volume attributes of known sizes;
 * anything else is said once and left as it is.
 */
extern int getattrlist(const char *, void *, void *, size_t, unsigned int);
extern int fgetattrlist(int, void *, void *, size_t, unsigned int);

/* the size in the packed buffer of each volume attribute bit (0: unknown here), in bit order */
static int
vol_attr_size(uint32_t bit)
{
	switch (bit) {
	case ATTR_VOL_FSTYPE: case ATTR_VOL_SIGNATURE: case ATTR_VOL_IOBLOCKSIZE: case ATTR_VOL_OBJCOUNT:
	case ATTR_VOL_FILECOUNT: case ATTR_VOL_DIRCOUNT: case ATTR_VOL_MAXOBJCOUNT: case ATTR_VOL_MOUNTFLAGS:
		return 4;
	case ATTR_VOL_SIZE: case ATTR_VOL_SPACEFREE: case ATTR_VOL_SPACEAVAIL: case ATTR_VOL_MINALLOCATION:
	case ATTR_VOL_ALLOCATIONCLUMP: case ATTR_VOL_ENCODINGSUSED:
#ifdef ATTR_VOL_SPACEUSED
	case ATTR_VOL_SPACEUSED:
#endif
		return 8;
	case ATTR_VOL_MOUNTPOINT: case ATTR_VOL_NAME: case ATTR_VOL_MOUNTEDDEVICE:
		return (int)sizeof(attrreference_t);
	case ATTR_VOL_CAPABILITIES:
		return (int)sizeof(vol_capabilities_attr_t);
	case ATTR_VOL_ATTRIBUTES:
		return (int)sizeof(vol_attributes_attr_t);
	case ATTR_VOL_INFO:
		return -1;      /* asks for nothing itself */
	default:
		return 0;
	}
}

static void
fix_attrs(const char *what, int on_root, struct attrlist *al, void *buf, size_t size)
{
	static uint32_t said_masks[16];
	static int nsaid;
	struct statfs var, root;
	uint8_t *p = buf, *end = (uint8_t *)buf + size;
	uint32_t vol, cmn;
	int known = 1;

	if (!in_settings() || !on_root || al == NULL || buf == NULL || al->bitmapcount != ATTR_BIT_MAP_COUNT || al->volattr == 0) {
		return;
	}
	cmn = al->commonattr;
	vol = al->volattr;
	if (cmn & ~(uint32_t)ATTR_CMN_RETURNED_ATTRS) {
		known = 0;
	}
	if (size < 4) {
		return;
	}
	p += 4;                                 /* the length */
	if (cmn & ATTR_CMN_RETURNED_ATTRS) {
		attribute_set_t set;

		if (p + sizeof(set) > end) {
			return;
		}
		memcpy(&set, p, sizeof(set));
		vol = set.volattr;              /* only what came back is packed */
		p += sizeof(set);
	}
	for (uint32_t bit = 1; bit != 0 && known; bit <<= 1) {
		if ((vol & bit) && vol_attr_size(bit) == 0) {
			known = 0;
		}
	}
	int seen = 0;

	for (int i = 0; i < nsaid; i++) {
		seen |= said_masks[i] == (al->volattr ^ (known ? 0 : 0x80000000u));
	}
	if (!seen && nsaid < 16) {
		said_masks[nsaid++] = al->volattr ^ (known ? 0 : 0x80000000u);
		say("hakios: tweaks: Settings asks volume attributes %#x (common %#x) of the root volume through %s%s\n", al->volattr,
		    al->commonattr, what, known ? "" : ": a layout not handled, left as it is");
	}
	if (!known || statfs("/private/var", &var) != 0 || strcmp(var.f_fstypename, "hfs") != 0 || statfs("/", &root) != 0) {
		return;
	}
	long long data_cap = (long long)var.f_blocks * var.f_bsize, data_free = (long long)var.f_bavail * var.f_bsize;
	long long sys_cap = (long long)root.f_blocks * root.f_bsize, sys_used = sys_cap - (long long)root.f_bfree * root.f_bsize;

	for (uint32_t bit = 1; bit != 0; bit <<= 1) {
		int n;
		long long v;

		if (!(vol & bit)) {
			continue;
		}
		n = vol_attr_size(bit);
		if (n < 0) {
			continue;
		}
		if (p + n > end) {
			return;
		}
		if (n == 8 && (bit == ATTR_VOL_SIZE || bit == ATTR_VOL_SPACEFREE || bit == ATTR_VOL_SPACEAVAIL
#ifdef ATTR_VOL_SPACEUSED
		    || bit == ATTR_VOL_SPACEUSED
#endif
		    )) {
			v = bit == ATTR_VOL_SIZE ? data_cap + sys_cap : (bit == ATTR_VOL_SPACEFREE || bit == ATTR_VOL_SPACEAVAIL) ?
			    data_free : data_cap + sys_cap - data_free;
			(void)sys_used;
			memcpy(p, &v, 8);
		}
		p += n;
	}
}

static int
on_root_path(const char *path)
{
	struct statfs sf;

	return path != NULL && statfs(path, &sf) == 0 && strcmp(sf.f_mntonname, "/") == 0;
}

static int
on_root_fd(int fd)
{
	struct statfs sf;

	return fstatfs(fd, &sf) == 0 && strcmp(sf.f_mntonname, "/") == 0;
}

static int
my_getattrlist(const char *path, void *al, void *buf, size_t size, unsigned int options)
{
	int r = getattrlist(path, al, buf, size, options);

	if (r == 0 && in_settings()) {
		fix_attrs("getattrlist", on_root_path(path), al, buf, size);
	}
	return r;
}

static int
my_fgetattrlist(int fd, void *al, void *buf, size_t size, unsigned int options)
{
	int r = fgetattrlist(fd, al, buf, size, options);

	if (r == 0 && in_settings()) {
		fix_attrs("fgetattrlist", on_root_fd(fd), al, buf, size);
	}
	return r;
}

__attribute__((used)) static const struct { const void *replacement, *replacee; } hakios_attr_interposers[]
    __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)my_getattrlist, (const void *)getattrlist },
	{ (const void *)my_fgetattrlist, (const void *)fgetattrlist },
};
