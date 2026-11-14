// SPDX-License-Identifier: MIT-0
/*
 * scion-fetch URL [IFNAME]: a test tool for an iOS 27 test root: one GET of URL through iOS's own stack (the name through
 * the system resolver, TLS, the route IPMonitor chose), then one line: the resolved addresses, the HTTP status, the bytes,
 * the time, and which interface the default route uses. With IFNAME the request is bound to that interface
 * (NSURLSession has no such knob: the name is resolved and the route checked only). Exit 0 on a 2xx/3xx answer.
 */
#import <Foundation/Foundation.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netdb.h>
#include <stdio.h>

int
main(int argc, char **argv)
{
	@autoreleasepool {
		if (argc < 2) {
			fprintf(stderr, "usage: scion-fetch URL\n");
			return 2;
		}
		NSURL *url = [NSURL URLWithString:[NSString stringWithUTF8String:argv[1]]];
		struct addrinfo hints = { .ai_family = AF_UNSPEC, .ai_socktype = SOCK_STREAM }, *res = NULL, *r;
		NSDate *t0 = [NSDate date];
		int e = getaddrinfo(url.host.UTF8String, "443", &hints, &res);
		NSMutableString *names = [NSMutableString string];

		for (r = res; e == 0 && r != NULL; r = r->ai_next) {
			char a[64];

			if (getnameinfo(r->ai_addr, r->ai_addrlen, a, sizeof(a), NULL, 0, NI_NUMERICHOST) == 0) {
				[names appendFormat:@"%s%s", names.length ? "," : "", a];
			}
		}
		if (res != NULL) {
			freeaddrinfo(res);
		}
		printf("resolve %s: %s (%.0f ms)\n", url.host.UTF8String, e == 0 ? names.UTF8String : gai_strerror(e),
		    -[t0 timeIntervalSinceNow] * 1000);

		NSURLSessionConfiguration *c = [NSURLSessionConfiguration ephemeralSessionConfiguration];
		c.timeoutIntervalForRequest = 30;
		c.allowsCellularAccess = YES;
		NSURLSession *s = [NSURLSession sessionWithConfiguration:c];
		dispatch_semaphore_t done = dispatch_semaphore_create(0);
		__block long status = -1;
		__block NSUInteger bytes = 0;
		__block NSString *err = nil;
		t0 = [NSDate date];
		[[s dataTaskWithURL:url completionHandler:^(NSData *d, NSURLResponse *resp, NSError *error) {
			if (error != nil) {
				err = [NSString stringWithFormat:@"%@ %ld", error.domain, (long)error.code];
			}
			if ([resp isKindOfClass:[NSHTTPURLResponse class]]) {
				status = ((NSHTTPURLResponse *)resp).statusCode;
			}
			bytes = d.length;
			dispatch_semaphore_signal(done);
		}] resume];
		if (dispatch_semaphore_wait(done, dispatch_time(DISPATCH_TIME_NOW, 45 * NSEC_PER_SEC)) != 0) {
			err = @"timed out (45 s)";
		}
		printf("GET %s: status %ld, %lu bytes, %.0f ms%s%s\n", argv[1], status, (unsigned long)bytes,
		    -[t0 timeIntervalSinceNow] * 1000, err ? ", error " : "", err ? err.UTF8String : "");
		return status >= 200 && status < 400 ? 0 : 1;
	}
}
