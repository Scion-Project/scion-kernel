/*
 * Which processes take the processors: every few seconds, the processor time each process was given since the
 * last look (user and system together), the most first, as one line for the run's log:
 *
 *   cputop: 25 s: 412% in all: backboardd[13] 96% SpringBoard[41] 88% WeatherWidget[131] 40% ...
 *
 * (100% is one core's whole time.) tools/frames.py says when the cores were all busy ("cores busy") while the
 * window server's frames came late; this says with whom it shared them. A process born since the last look counts
 * with all the time it has had.
 *
 *   usage: cputop [SECONDS [LOOKS]]     every SECONDS (default 5), LOOKS times (default: until the run ends)
 */
#include <mach/mach_time.h>
#include <stdint.h>
#include <stdio.h>
#include <sys/sysctl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

/* libproc's, which the iPhoneOS SDK does not declare (XNU's bsd/sys/proc_info.h, libsyscall/wrappers/libproc) */
#define PROC_ALL_PIDS           1
#define PROC_PIDTASKINFO        4
struct proc_taskinfo {
	uint64_t pti_virtual_size;
	uint64_t pti_resident_size;
	uint64_t pti_total_user;        /* in mach_absolute_time's units */
	uint64_t pti_total_system;
	uint64_t pti_threads_user;
	uint64_t pti_threads_system;
	int32_t pti_policy;
	int32_t pti_faults;
	int32_t pti_pageins;
	int32_t pti_cow_faults;
	int32_t pti_messages_sent;
	int32_t pti_messages_received;
	int32_t pti_syscalls_mach;
	int32_t pti_syscalls_unix;
	int32_t pti_csw;
	int32_t pti_threadnum;
	int32_t pti_numrunning;
	int32_t pti_priority;
};
int proc_listpids(uint32_t type, uint32_t typeinfo, void *buffer, int buffersize);
int proc_pidinfo(int pid, int flavor, uint64_t arg, void *buffer, int buffersize);
int proc_name(int pid, void *buffer, uint32_t buffersize);

#define MAX_PROCS       2048
#define SHOWN           10

struct sample {
	int pid;
	uint64_t time;                  /* user + system, mach_absolute_time's units */
};

static struct sample last[MAX_PROCS], now[MAX_PROCS];
static int last_count;

struct share {
	int pid;
	uint64_t time;
};

static int
by_time(const void *a, const void *b)
{
	const struct share *x = a, *y = b;

	return x->time < y->time ? 1 : x->time > y->time ? -1 : 0;
}

static uint64_t
time_before(int pid, int *found)
{
	for (int i = 0; i < last_count; i++) {
		if (last[i].pid == pid) {
			*found = 1;
			return last[i].time;
		}
	}
	*found = 0;
	return 0;
}

int
main(int argc, char **argv)
{
	unsigned seconds = argc > 1 ? (unsigned)atoi(argv[1]) : 5, looks = argc > 2 ? (unsigned)atoi(argv[2]) : 0;
	static int pids[MAX_PROCS];
	static struct share shares[MAX_PROCS];
	mach_timebase_info_data_t timebase;
	uint64_t start = mach_absolute_time(), before = start;

	if (seconds == 0) {
		seconds = 5;
	}
	mach_timebase_info(&timebase);
	setvbuf(stdout, NULL, _IOLBF, 0);
	for (unsigned look = 0; looks == 0 || look <= looks; look++) {
		int bytes = proc_listpids(PROC_ALL_PIDS, 0, pids, sizeof(pids)), count = 0, shown = 0;
		uint64_t at = mach_absolute_time(), all = 0;

		for (int i = 0; i < bytes / (int)sizeof(int) && count < MAX_PROCS; i++) {
			struct proc_taskinfo info;

			if (pids[i] <= 0 || proc_pidinfo(pids[i], PROC_PIDTASKINFO, 0, &info, sizeof(info)) != sizeof(info)) {
				continue;
			}
			now[count].pid = pids[i];
			now[count].time = info.pti_total_user + info.pti_total_system;
			count++;
		}
		if (look > 0) {
			int n = 0;

			for (int i = 0; i < count; i++) {
				int found;
				uint64_t then = time_before(now[i].pid, &found);

				if (now[i].time > then) {
					shares[n].pid = now[i].pid;
					shares[n].time = now[i].time - then;
					all += shares[n].time;
					n++;
				}
			}
			qsort(shares, (size_t)n, sizeof(shares[0]), by_time);
			/* the shares as percent of one core over the time between the looks (both in mach_absolute_time's units) */
			double span = (double)(at - before);
			printf("cputop: %llu s: %.0f%% in all:", (unsigned long long)((at - start) * timebase.numer / timebase.denom / 1000000000ull),
			    span > 0 ? 100.0 * (double)all / span : 0);
			for (int i = 0; i < n && shown < SHOWN; i++) {
				char name[64] = "?";
				double percent = span > 0 ? 100.0 * (double)shares[i].time / span : 0;

				if (percent < 1) {
					break;
				}
				proc_name(shares[i].pid, name, sizeof(name));
				printf(" %s[%d] %.0f%%", name, shares[i].pid, percent);
				/*
				 * A process that keeps a core or more busy for three looks in a row (mediaserverd at 84-140 % during
				 * YouTube with the MFC decoding, 19 November 2026): its threads' user stacks into the kernel's log
				 * (kern.hakios.ustacks), three times a run at the most, a minute apart.
				 */
				if (i == 0) {
					static char hot[64];
					static unsigned hot_looks, dumps;
					static uint64_t dumped_at;

					hot_looks = percent >= 80 && strcmp(name, hot) == 0 ? hot_looks + 1 : percent >= 80 ? 1 : 0;
					strlcpy(hot, name, sizeof(hot));
					if (hot_looks >= 3 && dumps < 3 && (dumped_at == 0 || (at - dumped_at) * timebase.numer / timebase.denom > 60000000000ull)) {
						if (sysctlbyname("kern.hakios.ustacks", NULL, NULL, name, strlen(name)) == 0) {
							dumps++;
							dumped_at = at;
							printf(" (its stacks to the log)");
						}
						hot_looks = 0;
					}
				}
				shown++;
			}
			printf("\n");
		}
		memcpy(last, now, sizeof(now[0]) * (size_t)count);
		last_count = count;
		before = at;
		if (looks != 0 && look == looks) {
			break;
		}
		sleep(seconds);
	}
	return 0;
}
