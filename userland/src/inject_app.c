/*
 * libhakios.app.dylib: code of ours inside an application, to find out why it dies (see inject.c
 * for how it gets there; build.sh's INJECT names the programs).
 *
 * An application that is launched and gone again in a third of a second leaves nothing behind here:
 * there is no crash reporter in this system, and what it writes on its standard error goes nowhere.
 * This gives it a standard error -- the console -- so that the Swift runtime's last words are read,
 * and says the frames of the stack when a signal kills it, under the dyld shared cache's slide,
 * which `ipsw dyld a2s <cache> <address>` turns back into names.
 */
#include <dispatch/dispatch.h>
#include <spawn.h>
#include <sys/stat.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <execinfo.h>
#include <dlfcn.h>
#include <Security/Security.h>
#include <sys/types.h>
#include <sys/ucontext.h>
#include <mach/mach.h>

/* <sys/codesign.h>'s, not in the SDK's public headers */
extern int csops(pid_t pid, unsigned int ops, void *useraddr, size_t usersize);
#define CS_OPS_STATUS                   0
#define CS_OPS_ENTITLEMENTS_BLOB        7
#define CS_OPS_TEAMID                   14
#define CS_OPS_DER_ENTITLEMENTS_BLOB    16
#define CS_VALID                        0x00000001
#define CS_DEBUGGED                     0x10000000
#define CS_PLATFORM_BINARY              0x04000000
#include <fcntl.h>
#include <errno.h>
#include <stdbool.h>
#include <mach-o/dyld.h>
#include <signal.h>
#include <stdarg.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

extern const void *_dyld_get_shared_cache_range(size_t *length);

#define CACHE_BASE 0x180000000UL

static uintptr_t slide;
static int console = -1;
static const char *who = "?";

static void
say(const char *text, size_t length)
{
	if (console < 0) {
		console = open("/dev/console", O_WRONLY);
	}
	(void)write(console >= 0 ? console : 2, text, length);
}

static void
killed(int number, siginfo_t *info, void *context)
{
	void *frames[32];
	char line[900];
	int n = backtrace(frames, 32), k;

	/*
	 * Where it stopped and why (step 113: Instagram's SIGTRAP stayed at the same place with the keychain working): the
	 * trapping instruction's image and offset, the instruction itself (brk #n: its number says whose trap), and the first
	 * argument registers, each also read as a C string when it points at one (an assertion's message often is).
	 */
	if (context != NULL) {
		ucontext_t *uc = (ucontext_t *)context;
		uintptr_t pc = (uintptr_t)uc->uc_mcontext->__ss.__pc;
		Dl_info dl;
		uint32_t insn = 0;
		vm_size_t got = 0;
		char where[160] = "?", regs[600];
		int r = 0;

		if (dladdr((void *)pc, &dl) != 0 && dl.dli_fname != NULL) {
			const char *base = strrchr(dl.dli_fname, '/');

			snprintf(where, sizeof(where), "%s+%#lx (%s)", base != NULL ? base + 1 : dl.dli_fname,
			    (unsigned long)(pc - (uintptr_t)dl.dli_fbase), dl.dli_sname != NULL ? dl.dli_sname : "-");
		}
		(void)vm_read_overwrite(mach_task_self(), (vm_address_t)pc, 4, (vm_address_t)&insn, &got);
		for (int i = 0; i < 4 && r < (int)sizeof(regs) - 140; i++) {
			uint64_t x = uc->uc_mcontext->__ss.__x[i];
			char str[100] = "";

			if (x > 0x100000000ULL && vm_read_overwrite(mach_task_self(), (vm_address_t)x, sizeof(str) - 1,
			    (vm_address_t)str, &got) == KERN_SUCCESS) {
				str[sizeof(str) - 1] = 0;
				for (char *q = str; *q; q++) {
					if ((unsigned char)*q < 0x20 || (unsigned char)*q > 0x7e) {
						*q = 0;
						break;
					}
				}
			}
			r += snprintf(regs + r, sizeof(regs) - (size_t)r, " x%d=%#llx%s%s%s", i, (unsigned long long)x,
			    strlen(str) >= 4 ? " \"" : "", strlen(str) >= 4 ? str : "", strlen(str) >= 4 ? "\"" : "");
		}
		k = snprintf(line, sizeof(line), "hakios: %s: SIGNAL %d stopped at %s, instruction %08x%s (brk #%u);%s\n", who,
		    number, where, insn, (insn & 0xffe0001f) == 0xd4200000 ? "" : " (not a brk)", (insn >> 5) & 0xffff, regs);
		say(line, (size_t)(k > 0 && k < (int)sizeof(line) ? k : (int)sizeof(line) - 1));
	}
	k = snprintf(line, sizeof(line), "hakios: %s: SIGNAL %d at %p (the cache's slide %#lx), the stack:", who, number,
	    info != NULL ? info->si_addr : NULL, (unsigned long)slide);
	for (int i = 0; i < n && k < (int)sizeof(line) - 60; i++) {
		Dl_info dl;
		size_t cache = 0;
		uintptr_t start = (uintptr_t)_dyld_get_shared_cache_range(&cache);

		/* a frame in the shared cache as the cache's address; one in the app's own code as image+offset */
		if ((uintptr_t)frames[i] - start < cache || dladdr(frames[i], &dl) == 0 || dl.dli_fname == NULL) {
			k += snprintf(line + k, sizeof(line) - (size_t)k, " %#lx", (unsigned long)((uintptr_t)frames[i] - slide));
		} else {
			const char *base = strrchr(dl.dli_fname, '/');

			k += snprintf(line + k, sizeof(line) - (size_t)k, " %s+%#lx", base != NULL ? base + 1 : dl.dli_fname,
			    (unsigned long)((uintptr_t)frames[i] - (uintptr_t)dl.dli_fbase));
		}
	}
	line[k++] = '\n';
	say(line, (size_t)k);
	signal(number, SIG_DFL);
	raise(number);
}

/*
 * Two things an application does on its way to a download, said out loud. Sileo queues a package and
 * nothing happens: no connection is made and no error is shown, so the question is how far it gets --
 * does it ask UIKit for a background task (a round trip to another daemon, which can wait for ever if
 * that daemon does not answer), and does it ever start a URL session task, for what address.
 */
static unsigned long (*begin_task)(id, SEL, id);
static void (*resume_task)(id, SEL);

static unsigned long
watched_begin_task(id self, SEL cmd, id handler)
{
	unsigned long task;
	char line[160];
	int n;

	n = snprintf(line, sizeof(line), "hakios: %s: asking UIKit for a background task\n", who);
	say(line, (size_t)(n > 0 ? n : 0));
	task = begin_task(self, cmd, handler);
	n = snprintf(line, sizeof(line), "hakios: %s: the background task is %lu\n", who, task);
	say(line, (size_t)(n > 0 ? n : 0));
	return task;
}

static void
watched_resume_task(id self, SEL cmd)
{
	const char *(*string)(id, SEL) = (const char *(*)(id, SEL))objc_msgSend;
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	const char *url = "?";
	char line[700];
	id request, address, text;
	int n;

	request = get(self, sel_registerName("originalRequest"));
	address = request != NULL ? get(request, sel_registerName("URL")) : NULL;
	text = address != NULL ? get(address, sel_registerName("absoluteString")) : NULL;
	if (text != NULL) {
		url = string(text, sel_registerName("UTF8String"));
	}
	n = snprintf(line, sizeof(line), "hakios: %s: starting a session task for %s\n", who,
	    url != NULL ? url : "?");
	say(line, (size_t)(n > 0 ? n : 0));
	resume_task(self, cmd);
}

/*
 * One setting of Sileo's, set for it at every start: "Show Packages From Canister" (ShowProvisional,
 * on by default). Canister is the search service Sileo asks about packages, and where its servers
 * cannot be reached the asking never stops -- 3810 session tasks in two minutes in one run here,
 * and the download of a package never gets its turn among them. With the setting off Sileo keeps to
 * the repositories themselves, which work. The prefix lives in memory and is new at every boot, so
 * there is nowhere to keep this but here. It is done on the main queue, not here in the library's
 * own initializer: at that point Foundation is loaded but has not run its own initializers yet, and
 * asking it for the defaults then kills the application before it says a word.
 */
static void
keep_to_the_repositories(void)
{
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	id (*from_text)(id, SEL, const char *) = (id (*)(id, SEL, const char *))objc_msgSend;
	void (*set_bool)(id, SEL, signed char, id) = (void (*)(id, SEL, signed char, id))objc_msgSend;
	id defaults = (id)objc_getClass("NSUserDefaults");
	id key = from_text((id)objc_getClass("NSString"), sel_registerName("stringWithUTF8String:"), "ShowProvisional");

	if (defaults == NULL || key == NULL) {
		return;
	}
	defaults = get(defaults, sel_registerName("standardUserDefaults"));
	if (defaults != NULL) {
		set_bool(defaults, sel_registerName("setBool:forKey:"), 0, key);
		say("hakios: Sileo: its Canister search is turned off (it is not reachable here)\n", 73);
	}
}

/*
 * apt-get install, not its simulation (-s...): the one that ends in "Unable to fetch some archives"
 * when Sileo runs it and installs the package when a terminal runs the same command.
 */
static int
apt_install(char *const argv[])
{
	int install = 0;

	if (argv == NULL || argv[0] == NULL || strcmp(argv[0], "apt-get") != 0) {
		return 0;
	}
	for (int i = 1; argv[i] != NULL; i++) {
		if (argv[i][0] == '-' && argv[i][1] == 's') {
			return 0;
		}
		install |= strcmp(argv[i], "install") == 0;
	}
	return install;
}

/*
 * What apt says goes to Sileo, which shows a line of it at most. Run the same apt-get, with the same
 * descriptors, credential and environment, under a shell that first says on the console who it runs
 * as, what lies in apt's archives and what Sileo's index says of the package, and afterwards how apt
 * ended and what the archives hold then. This is how "Unable to fetch some archives" was found: the
 * archives were empty, Sileo's /bin/mv having never run.
 */
static int
spawn_apt_said(pid_t *pid, const posix_spawn_file_actions_t *actions, const posix_spawnattr_t *attr,
    char *const argv[], char *const envp[])
{
	static char script[] =
	    "exec 7>/dev/console; J=/var/jb\n"
	    "echo \"hakios: apt-get runs as $($J/usr/bin/id), in $(pwd)\" >&7\n"
	    "$J/bin/ls -ln $J/var/cache/apt/archives $J/var/lib/apt/sileolists >&7 2>&1\n"
	    "for a in \"$@\"; do case $a in -*|install) ;; *) p=${a%%=*}\n"
	    "  echo \"hakios: $p in sileolists:\" >&7\n"
	    "  $J/usr/bin/grep -h -A30 \"^Package: $p\\$\" $J/var/lib/apt/sileolists/*Packages 2>&1 | $J/usr/bin/head -40 >&7 ;; esac; done\n"
	    "$J/usr/bin/apt-get \"$@\"\n"
	    "s=$?; echo \"hakios: apt-get exits $s; its archives after:\" >&7\n"
	    "$J/bin/ls -ln $J/var/cache/apt/archives >&7 2>&1; exit $s\n";
	char *wrapped[96];
	int k = 0;

	for (int i = 0; envp != NULL && envp[i] != NULL; i++) {
		char line[600];
		int n = snprintf(line, sizeof(line), "hakios: %s: apt-get's environment: %s\n", who, envp[i]);

		say(line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
	}
	wrapped[k++] = "sh";
	wrapped[k++] = "-c";
	wrapped[k++] = script;
	wrapped[k++] = "apt-get";
	for (int i = 1; argv[i] != NULL && k < 95; i++) {
		wrapped[k++] = argv[i];
	}
	wrapped[k] = NULL;
	return posix_spawn(pid, "/var/jb/bin/sh", actions, attr, wrapped, envp);
}

/*
 * Every program the application starts, with its arguments. Sileo does everything to the system by
 * spawning its set-user-ID helper, which execs apt or dpkg; the arguments are the whole question
 * when apt's answer to Sileo differs from apt's answer on the command line.
 */
static int
watched_spawn(pid_t *pid, const char *path, const posix_spawn_file_actions_t *actions,
    const posix_spawnattr_t *attr, char *const argv[], char *const envp[])
{
	char line[1200];
	int n = snprintf(line, sizeof(line), "hakios: %s: (uid %d) spawns %s", who, (int)getuid(),
	    path != NULL ? path : "?");

	for (int i = 0; argv != NULL && argv[i] != NULL && n < (int)sizeof(line) - 40; i++) {
		n += snprintf(line + n, sizeof(line) - (size_t)n, " %s", argv[i]);
	}
	/*
	 * The size of what it is about to move, while the file is still there: apt refuses the
	 * package Sileo puts in its archives ("Unable to fetch some archives") and takes the very
	 * same package when apt itself fetched it, so the first thing to know is whether the file
	 * is whole.
	 */
	if (argv != NULL && argv[0] != NULL && strcmp(argv[0], "mv") == 0 && argv[1] != NULL) {
		struct stat st;

		if (stat(argv[1], &st) == 0) {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " [%lld bytes]", (long long)st.st_size);
		} else {
			n += snprintf(line + n, sizeof(line) - (size_t)n, " [no such file]");
		}
	}
	n += snprintf(line + n, sizeof(line) - (size_t)n, "\n");
	say(line, (size_t)n);
	if (apt_install(argv)) {
		return spawn_apt_said(pid, actions, attr, argv, envp);
	}
	n = posix_spawn(pid, path, actions, attr, argv, envp);
	if (n != 0) {
		char said[300];
		int k = snprintf(said, sizeof(said), "hakios: %s: ... and that spawn failed: %s\n", who, strerror(n));

		say(said, (size_t)(k < (int)sizeof(said) ? k : (int)sizeof(said) - 1));
	}
	return n;
}

/*
 * A file the application moves or copies itself (NSFileManager, under Swift's FileManager), when it
 * fails: from where, to where, and what the error says. Sileo moves each download into place this
 * way, and says only "The operation couldn't be completed. No such file or directory".
 */
static const char *
text_of(id object)
{
	id (*get)(id, SEL) = (id (*)(id, SEL))objc_msgSend;
	const char *(*utf8)(id, SEL) = (const char *(*)(id, SEL))objc_msgSend;
	id text;

	if (object == NULL) {
		return "(nil)";
	}
	text = (class_respondsToSelector(object_getClass(object), sel_registerName("path")) ?
	    get(object, sel_registerName("path")) : get(object, sel_registerName("description")));
	return text != NULL ? utf8(text, sel_registerName("UTF8String")) : "?";
}

static signed char (*move_url)(id, SEL, id, id, id *), (*copy_url)(id, SEL, id, id, id *);
static signed char (*move_path)(id, SEL, id, id, id *), (*copy_path)(id, SEL, id, id, id *);

static signed char
file_said(const char *what, signed char ok, id from, id to, id error)
{
	if (!ok) {
		char line[1400];
		int n = snprintf(line, sizeof(line), "hakios: %s: %s %s -> %s failed: %s\n", who, what, text_of(from),
		    text_of(to), error != NULL ? text_of(error) : "(no error given)");

		say(line, (size_t)(n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
	}
	return ok;
}

static signed char
watched_move_url(id self, SEL cmd, id from, id to, id *error)
{
	id mine = NULL;
	signed char ok = move_url(self, cmd, from, to, error != NULL ? error : &mine);

	return file_said("moveItemAtURL", ok, from, to, error != NULL ? *error : mine);
}

static signed char
watched_copy_url(id self, SEL cmd, id from, id to, id *error)
{
	id mine = NULL;
	signed char ok = copy_url(self, cmd, from, to, error != NULL ? error : &mine);

	return file_said("copyItemAtURL", ok, from, to, error != NULL ? *error : mine);
}

static signed char
watched_move_path(id self, SEL cmd, id from, id to, id *error)
{
	id mine = NULL;
	signed char ok = move_path(self, cmd, from, to, error != NULL ? error : &mine);

	return file_said("moveItemAtPath", ok, from, to, error != NULL ? *error : mine);
}

static signed char
watched_copy_path(id self, SEL cmd, id from, id to, id *error)
{
	id mine = NULL;
	signed char ok = copy_path(self, cmd, from, to, error != NULL ? error : &mine);

	return file_said("copyItemAtPath", ok, from, to, error != NULL ? *error : mine);
}

/*
 * The keychain (19 November): Instagram, installed with AppSync, logs in, dies (SIGTRAP) and asks to log in again; YouTube's
 * Google sign-in goes nowhere. An app re-signed outside the App Store may not reach the keychain group its token is kept in
 * (errSecMissingEntitlement -34018) or find its item gone. Every SecItem call that does not succeed is logged with its
 * status and the item's class, service and access group; successes are counted.
 */
static unsigned keychain_ok, keychain_said;

static void
keychain_said_it(const char *call, OSStatus status, CFDictionaryRef query)
{
	char line[400], cls[48] = "-", svc[96] = "-", grp[96] = "-";
	CFTypeRef v;

	if (status == errSecSuccess) {
		keychain_ok++;
		return;
	}
	if (keychain_said++ >= 60) {
		return;
	}
	if (query != NULL) {
		if ((v = CFDictionaryGetValue(query, kSecClass)) != NULL && CFGetTypeID(v) == CFStringGetTypeID()) {
			CFStringGetCString(v, cls, sizeof(cls), kCFStringEncodingUTF8);
		}
		if ((v = CFDictionaryGetValue(query, kSecAttrService)) != NULL && CFGetTypeID(v) == CFStringGetTypeID()) {
			CFStringGetCString(v, svc, sizeof(svc), kCFStringEncodingUTF8);
		}
		if ((v = CFDictionaryGetValue(query, kSecAttrAccessGroup)) != NULL && CFGetTypeID(v) == CFStringGetTypeID()) {
			CFStringGetCString(v, grp, sizeof(grp), kCFStringEncodingUTF8);
		}
	}
	int n = snprintf(line, sizeof(line), "hakios: %s: keychain %s -> %d (class %s, service %s, group %s; %u succeeded so far)\n",
	    who, call, (int)status, cls, svc, grp, keychain_ok);

	say(line, (size_t)(n > 0 && n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
}

/*
 * Why the keychain says -34018 (step 111: every call of Instagram's and YouTube's, none succeeded): the kernel gives a
 * process's entitlements to securityd only while its signature is "valid" (CS_VALID or CS_DEBUGGED, bsd/kern/kern_proc.c's
 * csops), and only those its signature carries. What this process has of both is said once at its start.
 */
static void
signature_said(void)
{
	uint32_t flags = 0;
	struct { uint32_t magic, length; } header = { 0, 0 };
	char team[64] = "", line[400];
	static char xml[16384];
	int status = csops(getpid(), CS_OPS_STATUS, &flags, sizeof(flags));
	int xml_rc = csops(getpid(), CS_OPS_ENTITLEMENTS_BLOB, xml, sizeof(xml));
	int der_rc = csops(getpid(), CS_OPS_DER_ENTITLEMENTS_BLOB, &header, sizeof(header));
	int der_errno = der_rc != 0 ? errno : 0;
	int team_rc = csops(getpid(), CS_OPS_TEAMID, team, sizeof(team));
	uint32_t xml_len = xml_rc == 0 ? __builtin_bswap32(((uint32_t *)xml)[1]) : 0;
	const char *body = xml + 8;
	bool groups = xml_rc == 0 && strstr(body, "keychain-access-groups") != NULL;
	bool appid = xml_rc == 0 && strstr(body, "application-identifier") != NULL;
	int n = snprintf(line, sizeof(line), "hakios: %s: code signature: csops status %d flags %#x (valid %d, debugged %d, platform %d); "
	    "entitlements XML %s (%u bytes; keychain-access-groups %s, application-identifier %s), DER rc %d errno %d; team %s\n",
	    who, status, flags, !!(flags & CS_VALID), !!(flags & CS_DEBUGGED), !!(flags & CS_PLATFORM_BINARY),
	    xml_rc == 0 ? "read" : "NOT READ", xml_len, groups ? "yes" : "NO", appid ? "yes" : "NO", der_rc, der_errno,
	    team_rc == 0 ? team + 8 : "(none)");

	say(line, (size_t)(n > 0 && n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
}

static OSStatus
watched_item_add(CFDictionaryRef attributes, CFTypeRef *result)
{
	OSStatus status = SecItemAdd(attributes, result);

	keychain_said_it("SecItemAdd", status, attributes);
	return status;
}

static OSStatus
watched_item_copy(CFDictionaryRef query, CFTypeRef *result)
{
	OSStatus status = SecItemCopyMatching(query, result);

	keychain_said_it("SecItemCopyMatching", status, query);
	return status;
}

static OSStatus
watched_item_update(CFDictionaryRef query, CFDictionaryRef attributes)
{
	OSStatus status = SecItemUpdate(query, attributes);

	keychain_said_it("SecItemUpdate", status, query);
	return status;
}

static OSStatus
watched_item_delete(CFDictionaryRef query)
{
	OSStatus status = SecItemDelete(query);

	keychain_said_it("SecItemDelete", status, query);
	return status;
}

/*
 * The errors an app makes (step 113: YouTube's Google sign-in still goes nowhere with the keychain working, and says
 * nothing): every NSError made, its domain and code, the first 80 that are not URL cancellations.
 */
static id (*error_init)(id, SEL, id, long, id);
static unsigned errors_said;

static id
watched_error_init(id self, SEL cmd, id domain, long code, id info)
{
	id made = error_init(self, cmd, domain, code, info);
	const char *d = domain != nil ? ((const char *(*)(id, SEL))objc_msgSend)(domain, sel_registerName("UTF8String")) : "?";

	if (d != NULL && !(strcmp(d, "NSURLErrorDomain") == 0 && code == -999) && errors_said < 80) {
		char line[400];
		id desc = info != nil ? ((id (*)(id, SEL, id))objc_msgSend)(info, sel_registerName("objectForKey:"),
		    (id)CFSTR("NSLocalizedDescription")) : nil;
		const char *t = desc != nil && ((BOOL (*)(id, SEL, Class))objc_msgSend)(desc, sel_registerName("isKindOfClass:"),
		    objc_getClass("NSString")) ? ((const char *(*)(id, SEL))objc_msgSend)(desc, sel_registerName("UTF8String")) : "";
		int n = snprintf(line, sizeof(line), "hakios: %s: NSError %s %ld %.200s\n", who, d, code, t != NULL ? t : "");

		errors_said++;
		say(line, (size_t)(n > 0 && n < (int)sizeof(line) ? n : (int)sizeof(line) - 1));
		/*
		 * (step 115: YouTube's sign-in sheet is a view service that never starts -- _UIViewServiceInterfaceErrorDomain 0,
		 * _UIViewServiceErrorDomain 1 -- and runningboardd is never asked to launch SafariViewService: what the error carries
		 * and who made it)
		 */
		/*
		 * (step 117: Instagram still dies in IGLocalizedNextString right after "file not found" (NSCocoaErrorDomain 260,
		 * NSPOSIXErrorDomain 2), a JSON that does not parse (3840) and OSStatus -10813: which file, which data)
		 */
		bool files = strcmp(who, "Instagram") == 0 && ((strcmp(d, "NSCocoaErrorDomain") == 0 && code != 4099) ||
		    strcmp(d, "NSPOSIXErrorDomain") == 0 || strcmp(d, "NSOSStatusErrorDomain") == 0 || strncmp(d, "IG", 2) == 0);

		if (files || strstr(d, "ViewService") != NULL || strstr(d, "AuthenticationServices") != NULL || strstr(d, "WebAuthentication") != NULL ||
		    strstr(d, "SafariServices") != NULL) {
			id text = info != nil ? ((id (*)(id, SEL))objc_msgSend)(info, sel_registerName("description")) : nil;
			const char *u = text != nil ? ((const char *(*)(id, SEL))objc_msgSend)(text, sel_registerName("UTF8String")) : "(none)";
			char more[900];
			void *frames[14];
			int fn = backtrace(frames, 14), m;

			m = snprintf(more, sizeof(more), "hakios: %s:   its userInfo: %.600s\n", who, u != NULL ? u : "?");
			for (char *q = more + 20; *q && q < more + m - 1; q++) {
				if (*q == '\n') {
					*q = ' ';
				}
			}
			say(more, (size_t)(m > 0 && m < (int)sizeof(more) ? m : (int)sizeof(more) - 1));
			m = snprintf(more, sizeof(more), "hakios: %s:   made from:", who);
			for (int i = 2; i < fn && m < (int)sizeof(more) - 60; i++) {
				Dl_info dl;
				size_t cache = 0;
				uintptr_t start = (uintptr_t)_dyld_get_shared_cache_range(&cache);

				if ((uintptr_t)frames[i] - start < cache || dladdr(frames[i], &dl) == 0 || dl.dli_fname == NULL) {
					m += snprintf(more + m, sizeof(more) - (size_t)m, " %#lx", (unsigned long)((uintptr_t)frames[i] - slide));
				} else {
					const char *base = strrchr(dl.dli_fname, '/');

					m += snprintf(more + m, sizeof(more) - (size_t)m, " %s+%#lx", base != NULL ? base + 1 : dl.dli_fname,
					    (unsigned long)((uintptr_t)frames[i] - (uintptr_t)dl.dli_fbase));
				}
			}
			more[m++] = '\n';
			say(more, (size_t)m);
		}
	}
	return made;
}

__attribute__((used)) static struct {
	const void *ours, *theirs;
} keychain_interpose[] __attribute__((section("__DATA,__interpose"))) = {
	{ (const void *)watched_item_add, (const void *)SecItemAdd },
	{ (const void *)watched_item_copy, (const void *)SecItemCopyMatching },
	{ (const void *)watched_item_update, (const void *)SecItemUpdate },
	{ (const void *)watched_item_delete, (const void *)SecItemDelete },
};

__attribute__((used)) static struct {
	const void *ours, *theirs;
} spawn_interpose __attribute__((section("__DATA,__interpose"))) = {
	(const void *)watched_spawn, (const void *)posix_spawn
};

/* Put ours in the place of a method, keeping the one that was there. */
static void
watch(const char *name, const char *selector, IMP ours, void *theirs)
{
	Class class = objc_getClass(name);
	Method method = class != NULL ? class_getInstanceMethod(class, sel_registerName(selector)) : NULL;

	if (method != NULL && method_getImplementation(method) != ours) {
		*(IMP *)theirs = method_setImplementation(method, ours);
	}
}

__attribute__((constructor))
static void
hakios_app_init(void)
{
	static const int signals[] = { SIGTRAP, SIGABRT, SIGILL, SIGSEGV, SIGBUS, SIGSYS };
	struct sigaction action = { 0 };
	const char *name = getprogname();
	char line[200];
	size_t length = 0;
	int n;

	if (name == NULL) {
		return;
	}
	who = name;
	slide = (uintptr_t)_dyld_get_shared_cache_range(&length) - CACHE_BASE;
	/*
	 * An application's standard error goes nowhere here, and that is where the Swift runtime says
	 * what it is about to die of ("Fatal error: ..."), so it is pointed at the console.
	 */
	console = open("/dev/console", O_WRONLY);
	if (console >= 0) {
		dup2(console, 2);
	}
	action.sa_sigaction = killed;
	action.sa_flags = SA_SIGINFO | SA_NODEFER | SA_RESETHAND;
	sigemptyset(&action.sa_mask);
	for (unsigned i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
		sigaction(signals[i], &action, NULL);
	}
	if (strcmp(name, "Sileo") == 0) {
		dispatch_async(dispatch_get_main_queue(), ^{ keep_to_the_repositories(); });
	}
	if (strcmp(name, "Sileo") != 0) {
		watch("NSError", "initWithDomain:code:userInfo:", (IMP)watched_error_init, &error_init);
		goto started;           /* (an App Store app: the signals and the keychain only; its network calls are many) */
	}
	watch("UIApplication", "beginBackgroundTaskWithExpirationHandler:", (IMP)watched_begin_task, &begin_task);
	/*
	 * NSURLSessionTask alone: its subclasses inherit the very same method, and watching it twice
	 * would leave ours as the "original" of itself -- a call that never returns.
	 */
	watch("NSURLSessionTask", "resume", (IMP)watched_resume_task, &resume_task);
	watch("NSFileManager", "moveItemAtURL:toURL:error:", (IMP)watched_move_url, &move_url);
	watch("NSFileManager", "copyItemAtURL:toURL:error:", (IMP)watched_copy_url, &copy_url);
	watch("NSFileManager", "moveItemAtPath:toPath:error:", (IMP)watched_move_path, &move_path);
	watch("NSFileManager", "copyItemAtPath:toPath:error:", (IMP)watched_copy_path, &copy_path);
started:
	signature_said();
	n = snprintf(line, sizeof(line), "hakios: %s: started as uid %d (effective %d), watching for what kills it\n",
	    name, (int)getuid(), (int)geteuid());
	say(line, (size_t)(n > 0 ? n : 0));
}
