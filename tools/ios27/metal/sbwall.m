/*
 * sbwall (part of sbmtlc.dylib): a stop-gap static wallpaper while PosterBoard has no poster.
 * In SpringBoard only, when /private/var/local/sbwallpaper.png exists: every 4 s (first after 6 s, for 10 min) every
 * SpringBoard window whose class name contains one of the words in /private/var/local/sbwallpaper-windows gets the
 * image as a sublayer (aspect fill): WORD:top (above the window's own content) or WORD:bottom (under it). Default
 * "Wallpaper:top HomeScreen:bottom": the lock screen's _SBWallpaperSecureWindow, and behind the home screen icons. Window class names are logged once to the console
 * ("SBWALL ..."), so the right window can be named if the default misses. Remove the PNG to turn it off.
 */
#import <Foundation/Foundation.h>
#import <UIKit/UIKit.h>
#import <QuartzCore/QuartzCore.h>
#include <fcntl.h>
#include <unistd.h>

#define WALL_PNG "/private/var/local/sbwallpaper.png"
#define WALL_WINDOWS "/private/var/local/sbwallpaper-windows"

static void wall_say(NSString *text)
{
	int fd = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK);
	if (fd >= 0) {
		dprintf(fd, "SBWALL[%d]: %s\n", getpid(), text.UTF8String);
		close(fd);
	}
}

static NSArray<UIWindow *> *all_windows(void)
{
	NSMutableArray *out = [NSMutableArray array];
	UIApplication *app = [UIApplication sharedApplication];
	if (!app)
		return out;
	for (UIScene *scene in app.connectedScenes)
		if ([scene isKindOfClass:[UIWindowScene class]])
			[out addObjectsFromArray:((UIWindowScene *)scene).windows];
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wdeprecated-declarations"
	for (UIWindow *w in app.windows)
		if (![out containsObject:w])
			[out addObject:w];
#pragma clang diagnostic pop
	return out;
}

static void wall_pass(CGImageRef image, NSArray<NSString *> *words, int pass)
{
	NSArray<UIWindow *> *windows = all_windows();
	static NSMutableSet *named;
	if (!named)
		named = [NSMutableSet set];
	int applied = 0;
	for (UIWindow *w in windows) {
		NSString *cls = NSStringFromClass([w class]);
		if (![named containsObject:cls]) {
			[named addObject:cls];
			wall_say([NSString stringWithFormat:@"window %@ level %.0f hidden %d frame %.0fx%.0f", cls, w.windowLevel, w.hidden,
							    w.bounds.size.width, w.bounds.size.height]);
		}
		int want = 0; /* 1 top, 2 bottom */
		for (NSString *entry in words) {
			NSArray *parts = [entry componentsSeparatedByString:@":"];
			NSString *word = parts.firstObject;
			if (word.length && [cls rangeOfString:word options:NSCaseInsensitiveSearch].location != NSNotFound)
				want = (parts.count > 1 && [parts[1] isEqualToString:@"bottom"]) ? 2 : 1;
		}
		if (!want)
			continue;
		CALayer *mine = nil;
		for (CALayer *l in w.layer.sublayers)
			if ([l.name isEqualToString:@"sbwall"])
				mine = l;
		if (!mine) {
			mine = [CALayer layer];
			mine.name = @"sbwall";
			mine.contents = (__bridge id)image;
			mine.contentsGravity = kCAGravityResizeAspectFill;
			mine.masksToBounds = YES;
			mine.zPosition = 0;
			if (want == 2)
				[w.layer insertSublayer:mine atIndex:0];
			else
				[w.layer addSublayer:mine];
			wall_say([NSString stringWithFormat:@"image on %@ (%.0fx%.0f, %s)", cls, w.bounds.size.width, w.bounds.size.height,
							    want == 2 ? "bottom" : "top"]);
		} else if (want == 1 && mine != w.layer.sublayers.lastObject) {
			[mine removeFromSuperlayer];
			[w.layer addSublayer:mine];
		} else if (want == 2 && mine != w.layer.sublayers.firstObject) {
			[mine removeFromSuperlayer];
			[w.layer insertSublayer:mine atIndex:0];
		}
		mine.frame = w.layer.bounds;
		applied++;
	}
	if (pass == 0 || (pass % 15 == 0))
		wall_say([NSString stringWithFormat:@"pass %d: %lu windows, image on %d", pass, (unsigned long)windows.count, applied]);
}

void sbwall_start(void)
{
	if (strcmp(getprogname(), "SpringBoard") != 0 || access(WALL_PNG, R_OK) != 0)
		return;
	NSArray<NSString *> *words = @[ @"Wallpaper:top", @"HomeScreen:bottom" ];
	NSString *list = [NSString stringWithContentsOfFile:@WALL_WINDOWS encoding:NSUTF8StringEncoding error:nil];
	if (list.length)
		words = [list componentsSeparatedByCharactersInSet:[NSCharacterSet whitespaceAndNewlineCharacterSet]];
	__block CGImageRef image = NULL;
	__block int pass = 0;
	dispatch_source_t timer = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
	dispatch_source_set_timer(timer, dispatch_time(DISPATCH_TIME_NOW, 6 * NSEC_PER_SEC), 4 * NSEC_PER_SEC, NSEC_PER_SEC);
	dispatch_source_set_event_handler(timer, ^{
		@autoreleasepool {
			if (!image) {
				UIImage *ui = [UIImage imageWithContentsOfFile:@WALL_PNG];
				image = ui.CGImage ? CGImageRetain(ui.CGImage) : NULL;
				wall_say([NSString stringWithFormat:@"%s: %s, windows matching %@", WALL_PNG, image ? "loaded" : "NOT loaded",
								    [words componentsJoinedByString:@","]]);
				if (!image) {
					dispatch_source_cancel(timer);
					return;
				}
			}
			@try {
				wall_pass(image, words, pass);
			} @catch (NSException *e) {
				wall_say([NSString stringWithFormat:@"exception %@", e.reason]);
			}
			if (++pass > 150)
				dispatch_source_cancel(timer);
		}
	});
	dispatch_resume(timer);
	static dispatch_source_t keep;
	keep = timer;
	(void)keep;
	wall_say(@"armed");
}
