// CompileQueue.h: where the Metal frontend's pipeline compiles run (Starbite, iOS 27 port). Plain C++17, no Objective-C, so the
// rules below are tested on the host: tools/ios27/metal2/tests/compile-queue-test.cpp (run tools/ios27/metal2/tests/run.sh).
//
// The rules (scion-kernel #396 broke the first one twice on phone 2, 19 Nov 2026):
//  1. A synchronous creation call (newRenderPipelineStateWithDescriptor:error: and friends, and the draw-time remake of a pipeline)
//     compiles on the calling thread, now, every time, and returns what the compile returned: the pipeline, or the compile's own
//     error. No timeout, no refusal from a negative cache, no wait for a worker slot. QuartzCore treats any refused render pipeline
//     as fatal (backboardd aborts "Metal failed to build render pipeline", launchd reboots after three), so "ask again later" is not
//     an answer a synchronous caller can be given. A compile that never ends is PanVK's business: its compile budget
//     (mesa26-panvk-compile-budget.patch, CPU time of the compiling thread) turns it into a real compile error.
//  2. A completion-handler creation call returns at once; the compile runs on a worker task and the handler is called exactly once,
//     on that task, with the pipeline or the error. At most `slots` of these compile at the same time; the rest wait in a FIFO list
//     (no thread is parked on a semaphore for them). A synchronous call never waits for these slots (rule 1).
//  3. An asynchronous compile that FAILED after more than `slowFailureUs` is not started again for the same key for `negativeTtlUs`:
//     the handler gets the earlier failure at once. Only the asynchronous path uses this; a synchronous call always compiles.
//  4. Nothing is called with the lock held: a make() or a done() may call sync() or async() again (a completion handler that asks for
//     the next pipeline, a worker compile that goes through the synchronous entry point).
#pragma once
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <unordered_map>

namespace sbcompile {

using Make = std::function<void *(std::string &error)>;                 // compile: the handle, or null and `error`
using Done = std::function<void(void *handle, const std::string &error)>; // owns the handle it is given
using Spawn = std::function<void(std::function<void()>)>;                // run a task on another thread
using Clock = std::function<uint64_t()>;                                 // microseconds, monotonic

struct Stats {
	std::atomic<uint64_t> syncCalls{0}, asyncCalls{0}, asyncStarted{0}, asyncDone{0}, asyncRefusedNegative{0}, makeExceptions{0};
	std::atomic<int> asyncRunning{0}, asyncRunningMax{0};
};

class Compiler {
public:
	Compiler(unsigned slots, Spawn spawn, Clock now, uint64_t slowFailureUs = 10000000ull, uint64_t negativeTtlUs = 600000000ull)
		: slots_(slots ? slots : 1), spawn_(std::move(spawn)), now_(std::move(now)), slowFailureUs_(slowFailureUs), negativeTtlUs_(negativeTtlUs) {}

	// Rule 1.
	void *sync(const Make &make, std::string &error) {
		stats.syncCalls++;
		return guarded(make, error);
	}

	// Rules 2-4.
	void async(uint64_t key, Make make, Done done) {
		stats.asyncCalls++;
		std::string refused;
		{
			std::lock_guard<std::mutex> g(lock_);
			auto failed = slowFailures_.find(key);
			if (failed != slowFailures_.end()) {
				if (now_() - failed->second.at < negativeTtlUs_) refused = failed->second.error;
				else slowFailures_.erase(failed);
			}
		}
		if (!refused.empty()) {
			stats.asyncRefusedNegative++;
			done(nullptr, "an earlier compile of this pipeline failed after a long time (not tried again for 10 minutes): " + refused);
			return;
		}
		bool start = false;
		{
			std::lock_guard<std::mutex> g(lock_);
			pending_.push_back(Task{key, std::move(make), std::move(done)});
			if (running_ < slots_) { running_++; start = true; }
		}
		if (start) spawn_([this] { drain(); });
	}

	size_t pendingCount() { std::lock_guard<std::mutex> g(lock_); return pending_.size(); }
	bool idle() { std::lock_guard<std::mutex> g(lock_); return pending_.empty() && running_ == 0; }
	Stats stats;

private:
	struct Task { uint64_t key; Make make; Done done; };
	struct Failure { uint64_t at; std::string error; };

	void *guarded(const Make &make, std::string &error) {
		try {
			return make(error);
		} catch (...) {   // an Objective-C exception too: in Objective-C++ catch (...) takes it
			stats.makeExceptions++;
			if (error.empty()) error = "the compile threw an exception";
			return nullptr;
		}
	}

	// One worker: take the oldest pending task, compile, report, and go on while there is work; give the slot back when there is none.
	void drain() {
		for (;;) {
			Task task;
			{
				std::lock_guard<std::mutex> g(lock_);
				if (pending_.empty()) { running_--; return; }
				task = std::move(pending_.front());
				pending_.pop_front();
			}
			int now = ++stats.asyncRunning;
			for (int seen = stats.asyncRunningMax.load(); now > seen && !stats.asyncRunningMax.compare_exchange_weak(seen, now);) {}
			stats.asyncStarted++;
			const uint64_t began = now_();
			std::string error;
			void *handle = guarded(task.make, error);
			if (!handle && now_() - began > slowFailureUs_) {
				std::lock_guard<std::mutex> g(lock_);
				slowFailures_[task.key] = Failure{now_(), error};
			}
			stats.asyncRunning--;
			try {
				if (task.done) task.done(handle, error);
			} catch (...) {   // a handler that throws loses its own call, not the worker's slot
			}
			stats.asyncDone++;
		}
	}

	const unsigned slots_;
	Spawn spawn_;
	Clock now_;
	const uint64_t slowFailureUs_, negativeTtlUs_;
	std::mutex lock_;
	std::deque<Task> pending_;
	unsigned running_ = 0;
	std::unordered_map<uint64_t, Failure> slowFailures_;
};

} // namespace sbcompile
