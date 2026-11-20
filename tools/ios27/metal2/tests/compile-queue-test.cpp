// compile-queue-test.cpp: host test of CompileQueue.h, the rules the Metal frontend's pipeline compiles follow.
//   tools/ios27/metal2/tests/run.sh   (c++ -std=c++17 -pthread; no Apple SDK needed: also runs on Linux)
// Each test names the phone failure it would have caught. Exit status 0 = all pass.
#include "../CompileQueue.h"

#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

using namespace std::chrono;
using sbcompile::Compiler;

static int failures = 0;
#define CHECK(cond, ...) do { if (!(cond)) { failures++; std::printf("  FAIL %s:%d: %s: ", __FILE__, __LINE__, #cond); std::printf(__VA_ARGS__); std::printf("\n"); } } while (0)

static uint64_t realNow() { return (uint64_t)duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count(); }
static sbcompile::Spawn threads() { return [](std::function<void()> f) { std::thread(std::move(f)).detach(); }; }
static void *H(uintptr_t v) { return (void *)v; }

// A gate the test opens by hand: compiles that should still be "running" wait on it.
struct Gate {
	std::mutex m; std::condition_variable cv; bool open = false;
	void wait() { std::unique_lock<std::mutex> l(m); cv.wait(l, [&] { return open; }); }
	void release() { { std::lock_guard<std::mutex> g(m); open = true; } cv.notify_all(); }
};
// Wait (bounded) until pred() holds.
template <class P> static bool eventually(P pred, int ms = 3000) {
	for (int i = 0; i < ms; i += 5) { if (pred()) return true; std::this_thread::sleep_for(milliseconds(5)); }
	return pred();
}

// Phone 2, 19 Nov 00:30 (L6 + root-b3): with STARBITE_M2V_COMPILE_TIMEOUT_MS=5000 a cold-cache compile of backboardd's first
// render pipelines took longer than the wait, the frontend refused them, QuartzCore aborted backboardd 3 times, launchd rebooted.
// A synchronous call must return the compile's result however long it takes, whatever the old timeout variable says.
static void syncNeverTimesOut() {
	std::printf("sync: a slow compile is waited for (no timeout refusal; #396 failure 1)\n");
	setenv("STARBITE_M2V_COMPILE_TIMEOUT_MS", "50", 1);   // ignored now: a leftover mtl2-env line must change nothing
	Compiler c(3, threads(), realNow);
	std::string err;
	const uint64_t t0 = realNow();
	void *h = c.sync([](std::string &) { std::this_thread::sleep_for(milliseconds(400)); return H(0x51); }, err);
	CHECK(h == H(0x51), "got %p err '%s'", h, err.c_str());
	CHECK(err.empty(), "err '%s'", err.c_str());
	CHECK(realNow() - t0 >= 400000, "returned before the compile finished");
	unsetenv("STARBITE_M2V_COMPILE_TIMEOUT_MS");
}

// A real compile error is passed through as it is (the caller decides: QuartzCore aborts, an app shows nothing).
static void syncPassesRealError() {
	std::printf("sync: a compile error is the caller's error, verbatim\n");
	Compiler c(3, threads(), realNow);
	std::string err;
	void *h = c.sync([](std::string &e) { e = "PANVK: fragment shader abandoned after 90000 ms (compile budget)"; return (void *)nullptr; }, err);
	CHECK(h == nullptr, "handle %p", h);
	CHECK(err == "PANVK: fragment shader abandoned after 90000 ms (compile budget)", "err '%s'", err.c_str());
	std::string err2;
	void *h2 = c.sync([](std::string &) -> void * { throw 42; }, err2);
	CHECK(h2 == nullptr && !err2.empty(), "an exception in the compile must come back as an error, got %p '%s'", h2, err2.c_str());
}

// The night of 19 Nov blamed the frontend's 3-slot semaphore for b3t's missing frames (timeout 0): a synchronous compile on the
// render thread queued behind completion-handler compiles that never ended. Whatever the cause there was, it must not be possible:
// with every asynchronous slot taken by a compile that does not end, a synchronous call still compiles at once.
static void syncNotBehindAsyncSlots() {
	std::printf("sync: never waits for the asynchronous slots (all slots busy, sync still returns)\n");
	Gate gate;
	Compiler c(3, threads(), realNow);
	std::atomic<int> started{0}, done{0};
	for (int i = 0; i < 6; i++)
		c.async(100 + i, [&](std::string &) { started++; gate.wait(); return H(1); }, [&](void *, const std::string &) { done++; });
	CHECK(eventually([&] { return started.load() == 3; }), "3 slots should be busy, started=%d", started.load());
	std::string err;
	const uint64_t t0 = realNow();
	void *h = c.sync([](std::string &) { return H(0x77); }, err);
	CHECK(h == H(0x77), "sync got %p '%s'", h, err.c_str());
	CHECK(realNow() - t0 < 200000, "sync took %llu us with the slots busy", (unsigned long long)(realNow() - t0));
	CHECK(started.load() == 3 && c.pendingCount() == 3, "the other 3 wait: started=%d pending=%zu", started.load(), c.pendingCount());
	gate.release();
	CHECK(eventually([&] { return done.load() == 6; }), "all 6 handlers, got %d", done.load());
	CHECK(eventually([&] { return c.idle(); }), "workers still running at the end");
}

// Completion handlers: every one called exactly once, never more than `slots` compiles at once, all of them finish.
static void asyncAllCompleteOnceWithinSlots() {
	std::printf("async: 200 requests, 3 slots: each handler once, at most 3 at once, all done\n");
	Compiler c(3, threads(), realNow);
	const int N = 200;
	std::vector<std::atomic<int>> calls(N);
	std::atomic<int> concurrent{0}, worst{0};
	for (int i = 0; i < N; i++) {
		c.async((uint64_t)i, [&, i](std::string &e) -> void * {
			int now = ++concurrent;
			for (int w = worst.load(); now > w && !worst.compare_exchange_weak(w, now);) {}
			std::this_thread::sleep_for(microseconds(200 + (i % 7) * 100));
			concurrent--;
			if (i % 5 == 0) { e = "no"; return nullptr; }   // failures release their slot like successes
			return H(1000 + i);
		}, [&, i](void *h, const std::string &e) {
			calls[i]++;
			if (i % 5 == 0) CHECK(!h && e == "no", "job %d: %p '%s'", i, h, e.c_str());
			else CHECK(h == H(1000 + i), "job %d got %p", i, h);
		});
	}
	CHECK(eventually([&] { return c.stats.asyncDone.load() == (uint64_t)N; }, 10000), "done %llu of %d", (unsigned long long)c.stats.asyncDone.load(), N);
	for (int i = 0; i < N; i++) CHECK(calls[i].load() == 1, "handler %d called %d times", i, calls[i].load());
	CHECK(worst.load() <= 3, "%d compiles at once (slots 3)", worst.load());
	CHECK(c.pendingCount() == 0, "pending %zu", c.pendingCount());
	CHECK(eventually([&] { return c.idle(); }), "workers still running at the end");
}

// The completion-handler entry points of the frontend run the synchronous method on the worker (that is how they compile), and a
// handler often asks for the next pipeline. Neither may deadlock, including when all slots are in use.
static void reentrancy() {
	std::printf("async: make() calling sync(), done() calling async(): no deadlock\n");
	Compiler c(1, threads(), realNow);
	std::atomic<int> second{0};
	c.async(1, [&](std::string &e) { return c.sync([](std::string &) { return H(2); }, e); },
	        [&](void *h, const std::string &) {
		        CHECK(h == H(2), "nested sync result %p", h);
		        c.async(2, [](std::string &) { return H(3); }, [&](void *h2, const std::string &) { CHECK(h2 == H(3), "%p", h2); second++; });
	        });
	CHECK(eventually([&] { return second.load() == 1; }), "the handler's own request did not complete");
	CHECK(eventually([&] { return c.idle(); }), "workers still running at the end");
}

// A handler that throws must not keep the slot (with one slot, the next request would never start).
static void throwingHandlerKeepsSlot() {
	std::printf("async: a throwing handler does not lose the slot\n");
	Compiler c(1, threads(), realNow);
	std::atomic<int> ok{0};
	c.async(1, [](std::string &) { return H(1); }, [](void *, const std::string &) { throw 1; });
	c.async(2, [](std::string &) { return H(2); }, [&](void *, const std::string &) { ok++; });
	CHECK(eventually([&] { return ok.load() == 1; }), "second request never ran");
	CHECK(eventually([&] { return c.idle(); }), "workers still running at the end");
}

// Rule 3 with a fake clock: an asynchronous compile that failed slowly is answered from the negative cache for 10 minutes, then
// tried again; a synchronous call of the same pipeline always compiles.
static void negativeCacheAsyncOnly() {
	std::printf("async: a slow failure is not retried for 10 min; sync ignores that\n");
	std::atomic<uint64_t> clock{1000000};
	Compiler c(2, threads(), [&] { return clock.load(); });
	std::atomic<int> makes{0}, done{0};
	std::string lastErr;
	std::mutex m;
	auto slowFail = [&](std::string &e) -> void * { makes++; clock += 11000000; e = "budget"; return nullptr; };
	auto rec = [&](void *h, const std::string &e) { std::lock_guard<std::mutex> g(m); CHECK(!h, "%p", h); lastErr = e; done++; };
	c.async(9, slowFail, rec);
	CHECK(eventually([&] { return done.load() == 1; }), "first");
	c.async(9, slowFail, rec);
	CHECK(eventually([&] { return done.load() == 2; }), "second");
	CHECK(makes.load() == 1, "the second request compiled again (makes=%d)", makes.load());
	{ std::lock_guard<std::mutex> g(m); CHECK(lastErr.find("budget") != std::string::npos, "refusal should carry the real error: '%s'", lastErr.c_str()); }
	std::string err;
	void *h = c.sync([&](std::string &) { makes++; return H(5); }, err);
	CHECK(h == H(5) && makes.load() == 2, "sync must compile regardless of the negative cache (%p, makes %d)", h, makes.load());
	clock += 600000001;
	c.async(9, [&](std::string &) { makes++; return H(6); }, [&](void *h2, const std::string &) { CHECK(h2 == H(6), "%p", h2); done++; });
	CHECK(eventually([&] { return done.load() == 3; }), "after 10 min it is tried again");
	CHECK(makes.load() == 3, "makes %d", makes.load());
	CHECK(eventually([&] { return c.idle(); }), "workers still running at the end");
}

int main() {
	syncNeverTimesOut();
	syncPassesRealError();
	syncNotBehindAsyncSlots();
	asyncAllCompleteOnceWithinSlots();
	reentrancy();
	throwingHandlerKeepsSlot();
	negativeCacheAsyncOnly();
	std::printf(failures ? "compile-queue-test: %d FAILED\n" : "compile-queue-test: all passed\n", failures);
	return failures ? 1 : 0;
}
