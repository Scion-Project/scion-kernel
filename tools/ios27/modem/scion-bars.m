// SPDX-License-Identifier: MIT-0
/*
 * scion-bars (c.dylib, injected into SpringBoard through the boot-arg sweetinject): the status bar's cellular entry - signal
 * bars and the network's name - from the S9's own modem (scion-kernel drivers/modem/hakios.shannon.c, kern.hakios.cp.info),
 * because iOS's CommCenter does not know that modem and shows no service. Cosmetic only: Settings still says "No SIM", and
 * nothing of CoreTelephony changes. Only our own code; no Apple file is changed.
 *
 * On only when /private/var/local/scion-bars exists. That file may hold lines KEY=VALUE:
 *   name=TEXT     the network name shown (default: from the operator code, a small table; else the code itself)
 *   status=N      the entry's status value for "registered" (default 3: active), type=N the technology (default: found by
 *                 its description, "LTE"), mode=hook|publish|both (default hook: SpringBoard's own status bar data;
 *                 publish: StatusKit's status bar overrides domain), discover=1 (the classes' methods into the log),
 *                 poke=SELECTOR (a method of SpringBoard's status bar aggregator called after a change, to rebuild its data)
 * Safety: a boot counter (/private/var/local/scion-bars.boots) turns it off after 3 SpringBoard starts in a row that did not
 * live 90 s. Everything it does is logged to the console as "SCIONBAR[pid]: ...".
 */
#import <Foundation/Foundation.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <objc/message.h>
#include <objc/runtime.h>
#include <string.h>
#include <sys/sysctl.h>
#include <unistd.h>

#define ON_FILE "/private/var/local/scion-bars"
#define BOOTS   "/private/var/local/scion-bars.boots"

static void
say(NSString *fmt, ...)
{
	va_list ap;
	va_start(ap, fmt);
	NSString *s = [[NSString alloc] initWithFormat:fmt arguments:ap];
	va_end(ap);
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK);
	if (fd >= 0) {
		dprintf(fd, "SCIONBAR[%d]: %s\n", getpid(), s.UTF8String);
		close(fd);
	}
}

static NSDictionary *cfg;
static id current;              /* our cellular entry, nil while the modem is not registered */
static IMP orig_getter, orig_entry_for_key, orig_setter;
static long last_gen = -2;

static long
field(const char *info, const char *key, long def)
{
	char pat[24];
	snprintf(pat, sizeof(pat), "%s=", key);
	for (const char *p = info; (p = strstr(p, pat)) != NULL; p++) {
		if (p == info || p[-1] == ' ') {
			return strtol(p + strlen(pat), NULL, 10);
		}
	}
	return def;
}

static NSString *
oper_name(const char *info)
{
	static const struct { const char *code, *name; } t[] = {
		{ "27201", "Vodafone IE" }, { "27202", "3" }, { "27203", "eir" }, { "27205", "3" }, { "27211", "Tesco Mobile" },
		{ "27213", "Lycamobile" }, { "23410", "O2 - UK" }, { "23415", "Vodafone UK" }, { "23420", "3" }, { "23430", "EE" },
	};
	const char *p = strstr(info, "oper=");
	char code[8] = "";
	if (p != NULL) {
		sscanf(p + 5, "%7[0-9]", code);
	}
	for (size_t i = 0; i < sizeof(t) / sizeof(t[0]); i++) {
		if (strcmp(code, t[i].code) == 0) {
			return @(t[i].name);
		}
	}
	return code[0] ? @(code) : @"";
}

static void
dump_class(const char *name)
{
	Class c = objc_getClass(name);
	if (c == Nil) {
		say(@"class %s: none", name);
		return;
	}
	for (int meta = 0; meta < 2; meta++) {
		unsigned n = 0;
		Method *m = class_copyMethodList(meta ? object_getClass((id)c) : c, &n);
		NSMutableString *s = [NSMutableString string];
		for (unsigned i = 0; i < n; i++) {
			[s appendFormat:@" %s%s", meta ? "+" : "", sel_getName(method_getName(m[i]))];
		}
		free(m);
		say(@"class %s (super %s) %s methods %u:%@", name, class_getName(class_getSuperclass(c)), meta ? "class" : "instance",
		    n, s.length > 3000 ? [s substringToIndex:3000] : s);
	}
}

/* the technology's value: the one whose description says LTE (or 4G), unless type= is given */
static long
lte_type(void)
{
	if (cfg[@"type"] != nil) {
		return [cfg[@"type"] integerValue];
	}
	NSString *(*desc)(long) = (NSString * (*)(long))dlsym(RTLD_DEFAULT, "STStatusBarDataCellularTypeDescription");
	if (desc == NULL) {
		desc = (NSString * (*)(long))dlsym(RTLD_DEFAULT, "_STStatusBarDataCellularTypeDescription");
	}
	long best = -1;
	for (long v = 0; desc != NULL && v < 24; v++) {
		NSString *d = desc(v);
		if (d && best < 0 && ([d rangeOfString:@"LTE" options:NSCaseInsensitiveSearch].location != NSNotFound ||
		    [d isEqualToString:@"4G"])) {
			best = v;
		}
	}
	return best >= 0 ? best : 5;
}

/* a cellular entry: a copy of SpringBoard's own one (or a new one) with our values set by key-value coding */
static id
make_entry(id base, NSString *name, long bars, long dbm, long type)
{
	Class c = objc_getClass("STStatusBarDataCellularEntry");
	id e = nil;
	@try {
		e = base != nil ? [base copy] : [[c alloc] init];
		long status = cfg[@"status"] ? [cfg[@"status"] integerValue] : 3;
		NSDictionary *v = @{ @"stringValue": name, @"type": @(type), @"status": @(status), @"displayValue": @(bars),
				     @"rawValue": @(dbm), @"displayRawValue": @NO };
		for (NSString *k in v) {
			@try {
				[e setValue:v[k] forKey:k];
			} @catch (NSException *x) {
				say(@"entry: %@ not set: %@", k, x.reason);
			}
		}
	} @catch (NSException *x) {
		say(@"entry: not made: %@", x.reason);
		e = nil;
	}
	return e;
}

static id
my_getter(id self, SEL _cmd)
{
	id e = current;
	return e != nil ? e : ((id (*)(id, SEL))orig_getter)(self, _cmd);
}

static id
my_entry_for_key(id self, SEL _cmd, id key)
{
	id e = current;
	if (e != nil && [key isKindOfClass:[NSString class]] && [key isEqualToString:@"cellularEntry"]) {
		return e;
	}
	return ((id (*)(id, SEL, id))orig_entry_for_key)(self, _cmd, key);
}

static void
my_setter(id self, SEL _cmd, id entry)
{
	id e = current;
	((void (*)(id, SEL, id))orig_setter)(self, _cmd, e != nil ? e : entry);
}

/* poke=SELECTOR: a no-argument method of SpringBoard's status bar aggregator called after each change, so that it builds
 * its data again (found with discover=1) */
static void
poke(void)
{
	NSString *sel = cfg[@"poke"];
	Class c = objc_getClass("SBStatusBarStateAggregator");
	if (sel.length == 0 || c == Nil) {
		return;
	}
	@try {
		id agg = [c respondsToSelector:sel_registerName("sharedInstance")] ?
		    ((id (*)(id, SEL))objc_msgSend)(c, sel_registerName("sharedInstance")) : nil;
		SEL s = NSSelectorFromString(sel);
		if (agg != nil && [agg respondsToSelector:s]) {
			((void (*)(id, SEL))objc_msgSend)(agg, s);
			say(@"poke: -[SBStatusBarStateAggregator %@] called", sel);
		} else {
			say(@"poke: no aggregator (%@) or no %@", agg, sel);
		}
	} @catch (NSException *x) {
		say(@"poke: %@", x.reason);
	}
}

static void
hook(void)
{
	Class c = objc_getClass("STStatusBarData");
	Method m = c ? class_getInstanceMethod(c, sel_registerName("cellularEntry")) : NULL;
	if (m != NULL) {
		orig_getter = method_setImplementation(m, (IMP)my_getter);
	}
	Method k = c ? class_getInstanceMethod(c, sel_registerName("entryForKey:")) : NULL;
	if (k != NULL) {
		orig_entry_for_key = method_setImplementation(k, (IMP)my_entry_for_key);
	}
	Class mc = objc_getClass("STMutableStatusBarData");
	Method st = mc ? class_getInstanceMethod(mc, sel_registerName("setCellularEntry:")) : NULL;
	if (st != NULL) {
		orig_setter = method_setImplementation(st, (IMP)my_setter);
	}
	say(@"hook: STStatusBarData -cellularEntry %s, -entryForKey: %s, STMutableStatusBarData -setCellularEntry: %s",
	    m ? "hooked" : "NOT FOUND", k ? "hooked" : "not found", st ? "hooked" : "not found");
}

static void
publish(id entry)
{
	static id pub;
	Class pc = objc_getClass("STStatusBarOverridesStatusDomainPublisher");
	if (pc == Nil) {
		say(@"publish: no publisher class");
		return;
	}
	@try {
		if (pub == nil) {
			pub = [[pc alloc] init];
		}
		void (^fill)(id) = ^(id data) {
			id ov = [data valueForKey:@"customOverrides"];
			if (ov == nil) {
				ov = [[objc_getClass("STMutableStatusBarData") alloc] init];
				[data setValue:ov forKey:@"customOverrides"];
			}
			[ov setValue:entry forKey:@"cellularEntry"];
		};
		if ([pub respondsToSelector:sel_registerName("updateDataWithBlock:")]) {
			((void (*)(id, SEL, id))objc_msgSend)(pub, sel_registerName("updateDataWithBlock:"), fill);
			say(@"publish: updateDataWithBlock: done");
		} else {
			say(@"publish: the publisher has no updateDataWithBlock:");
		}
	} @catch (NSException *x) {
		say(@"publish: %@", x.reason);
	}
}

static void
tick(void)
{
	char info[256] = "";
	size_t len = sizeof(info);
	if (sysctlbyname("kern.hakios.cp.info", info, &len, NULL, 0) != 0) {
		info[0] = 0;
	}
	info[sizeof(info) - 1] = 0;
	long gen = field(info, "gen", -1);
	if (gen == last_gen) {
		return;
	}
	last_gen = gen;
	long reg = field(info, "reg", -1), bars = field(info, "bars", -1), dbm = field(info, "dbm", 0);
	bool registered = reg == 2 || reg == 6;
	id e = nil;
	if (registered) {
		NSString *name = cfg[@"name"] ?: oper_name(info);
		if (bars < 0) {
			bars = dbm == 0 ? 1 : dbm > -90 ? 4 : dbm > -100 ? 3 : dbm > -110 ? 2 : 1;
		}
		Class sb = objc_getClass("STStatusBarData");
		id base = nil;
		if (orig_getter != NULL && sb != Nil) {
			/* (a fresh data object's own entry, as a template; no current one is reachable from here) */
			id d = [[sb alloc] init];
			base = ((id (*)(id, SEL))orig_getter)(d, sel_registerName("cellularEntry"));
		}
		e = make_entry(base, name, bars > 4 ? 4 : bars, dbm, lte_type());
	}
	current = e;
	say(@"modem: %s -> %@", info, e != nil ? [e description] : @"(no entry: not registered)");
	NSString *mode = cfg[@"mode"] ?: @"hook";
	if (e != nil && ([mode isEqualToString:@"publish"] || [mode isEqualToString:@"both"])) {
		publish(e);
	}
	poke();
}

__attribute__((constructor)) static void
scion_bars_init(void)
{
	if (strcmp(getprogname(), "SpringBoard") != 0 || access(ON_FILE, F_OK) != 0) {
		return;
	}
	int boots = 0;
	FILE *f = fopen(BOOTS, "r");
	if (f) {
		fscanf(f, "%d", &boots);
		fclose(f);
	}
	if (boots >= 3) {
		say(@"off: SpringBoard started %d times without living 90 s with it (remove %s to try again)", boots, BOOTS);
		return;
	}
	if ((f = fopen(BOOTS, "w"))) {
		fprintf(f, "%d\n", boots + 1);
		fclose(f);
	}
	NSMutableDictionary *d = [NSMutableDictionary dictionary];
	for (NSString *l in [[NSString stringWithContentsOfFile:@ON_FILE encoding:NSUTF8StringEncoding error:nil] ?: @""
		componentsSeparatedByString:@"\n"]) {
		NSRange r = [l rangeOfString:@"="];
		if (r.location != NSNotFound) {
			d[[l substringToIndex:r.location]] = [l substringFromIndex:r.location + 1];
		}
	}
	cfg = d;
	say(@"on (config %@)", cfg);
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 15 * NSEC_PER_SEC), dispatch_get_main_queue(), ^{
		if ([cfg[@"discover"] isEqualToString:@"1"]) {
			const char *names[] = { "STStatusBarDataCellularEntry", "STStatusBarData", "STMutableStatusBarData",
				"STStatusBarOverridesStatusDomainPublisher", "STStatusBarOverridesStatusDomainData",
				"STMutableStatusBarOverridesStatusDomainData", "SBStatusBarStateAggregator" };
			for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
				dump_class(names[i]);
			}
			NSString *(*desc)(long) = (NSString * (*)(long))dlsym(RTLD_DEFAULT, "STStatusBarDataCellularTypeDescription");
			for (long v = 0; desc != NULL && v < 24; v++) {
				say(@"cellular type %ld = %@", v, desc(v));
			}
			if (desc == NULL) {
				say(@"no STStatusBarDataCellularTypeDescription");
			}
		}
		NSString *mode = cfg[@"mode"] ?: @"hook";
		if ([mode isEqualToString:@"hook"] || [mode isEqualToString:@"both"]) {
			hook();
		}
		dispatch_source_t t = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
		dispatch_source_set_timer(t, dispatch_time(DISPATCH_TIME_NOW, 0), 10 * NSEC_PER_SEC, NSEC_PER_SEC);
		dispatch_source_set_event_handler(t, ^{
			tick();
		});
		dispatch_resume(t);
		static dispatch_source_t keep;
		keep = t;
	});
	dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 90 * NSEC_PER_SEC), dispatch_get_global_queue(0, 0), ^{
		unlink(BOOTS);
		say(@"alive 90 s: the boot counter is cleared");
	});
}
