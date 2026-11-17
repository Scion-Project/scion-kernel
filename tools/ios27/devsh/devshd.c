/*
 * SPDX-License-Identifier: MIT-0
 * devshd: the user-space half of the USB console's "sh" (kernel boot-arg devsh=1, drivers/usb/hakios.dwc3.c). A TEST ROOT ONLY
 * (tools/ios27/devsh/devsh-step.sh adds it; check-root.sh is told it is not for a release). Runs as root under launchd: takes a
 * queued line with sysctl kern.hakios.devsh.cmd, runs it with the prefix's sh, and hands what it prints back to the console
 * line by line (kern.hakios.devsh.out), then "rc=N". Nothing listens: the only way in is the console on the USB cable.
 *   "@mobile LINE"  runs LINE as mobile (501:501, HOME /var/mobile) instead of root
 *   "@bg LINE"      starts LINE and does not wait (its output goes to /private/var/tmp/devsh-bg.log)
 *   "@t=N LINE"     a time limit of N seconds instead of 60 (the whole process group is killed after it)
 * The kernel answers ENOENT without devsh=1: then this exits at once and stays off for the boot.
 */
#include <errno.h>
#include <stdarg.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <poll.h>
#include <time.h>
#include <unistd.h>

#define MAXLINE 1024

static void
out(const char *s, size_t n)
{
	char b[250];

	while (n > 0) {
		size_t k = n < sizeof(b) - 1 ? n : sizeof(b) - 1;

		memcpy(b, s, k);
		b[k] = 0;
		(void)sysctlbyname("kern.hakios.devsh.out", NULL, NULL, b, k + 1);
		s += k;
		n -= k;
	}
}

static void
outf(const char *fmt, ...)
{
	char b[250];
	va_list ap;

	va_start(ap, fmt);
	vsnprintf(b, sizeof(b), fmt, ap);
	va_end(ap);
	out(b, strlen(b));
}

static double
now(void)
{
	struct timeval tv;

	gettimeofday(&tv, NULL);
	return tv.tv_sec + tv.tv_usec / 1e6;
}

static void
run(char *cmd)
{
	int mobile = 0, bg = 0, limit = 60, p[2], status = 0;
	pid_t pid;
	double t0 = now();
	char line[256];
	size_t ln = 0;

	for (;;) {
		if (strncmp(cmd, "@mobile ", 8) == 0) {
			mobile = 1;
			cmd += 8;
		} else if (strncmp(cmd, "@bg ", 4) == 0) {
			bg = 1;
			cmd += 4;
		} else if (strncmp(cmd, "@t=", 3) == 0) {
			limit = atoi(cmd + 3);
			cmd = strchr(cmd, ' ');
			if (cmd == NULL) {
				return;
			}
			cmd++;
			if (limit < 1 || limit > 3600) {
				limit = 60;
			}
		} else {
			break;
		}
	}
	if (pipe(p) != 0) {
		outf("devshd: pipe: %s", strerror(errno));
		return;
	}
	pid = fork();
	if (pid < 0) {
		outf("devshd: fork: %s", strerror(errno));
		close(p[0]);
		close(p[1]);
		return;
	}
	if (pid == 0) {
		int fd;

		setsid();
		close(p[0]);
		fd = bg ? open("/private/var/tmp/devsh-bg.log", O_WRONLY | O_CREAT | O_APPEND, 0600) : p[1];
		if (fd < 0) {
			fd = p[1];
		}
		dup2(fd, 1);
		dup2(fd, 2);
		fd = open("/dev/null", O_RDONLY);
		dup2(fd, 0);
		for (int i = 3; i < 256; i++) {
			close(i);
		}
		setenv("PATH", "/var/jb/usr/bin:/var/jb/bin:/var/jb/usr/sbin:/var/jb/sbin:/var/jb/usr/local/bin:/usr/bin:/bin:/usr/sbin:/sbin", 1);
		setenv("TERM", "dumb", 1);
		if (mobile) {
			setenv("HOME", "/var/mobile", 1);
			setenv("USER", "mobile", 1);
			if (setgid(501) != 0 || setuid(501) != 0) {
				_exit(126);
			}
		} else {
			setenv("HOME", "/var/root", 1);
			setenv("USER", "root", 1);
		}
		execl("/var/jb/bin/sh", "sh", "-c", cmd, (char *)NULL);
		execl("/var/jb/usr/bin/bash", "bash", "-c", cmd, (char *)NULL);
		_exit(127);
	}
	close(p[1]);
	if (bg) {
		close(p[0]);
		outf("rc=bg pid %d", pid);
		return;
	}
	fcntl(p[0], F_SETFL, O_NONBLOCK);
	for (;;) {
		struct pollfd pf = { .fd = p[0], .events = POLLIN };
		int r = poll(&pf, 1, 500);
		char buf[512];
		ssize_t n;

		if (now() - t0 > limit) {
			kill(-pid, SIGKILL);
			kill(pid, SIGKILL);
			outf("devshd: time limit %d s: killed", limit);
			break;
		}
		if (r <= 0) {
			continue;
		}
		n = read(p[0], buf, sizeof(buf));
		if (n == 0) {
			break;
		}
		if (n < 0) {
			if (errno == EAGAIN || errno == EINTR) {
				continue;
			}
			break;
		}
		for (ssize_t i = 0; i < n; i++) {
			if (buf[i] == '\n' || ln == sizeof(line) - 1) {
				out(line, ln);
				if (ln == 0) {
					out(" ", 1);
				}
				ln = 0;
				if (buf[i] == '\n') {
					continue;
				}
			}
			line[ln++] = buf[i] == '\t' ? ' ' : (buf[i] < 32 || buf[i] == 127) ? '.' : buf[i];
		}
	}
	if (ln > 0) {
		out(line, ln);
	}
	close(p[0]);
	waitpid(pid, &status, 0);
	if (WIFSIGNALED(status)) {
		outf("rc=sig%d (%.1f s)", WTERMSIG(status), now() - t0);
	} else {
		outf("rc=%d (%.1f s)", WEXITSTATUS(status), now() - t0);
	}
}

int
main(void)
{
	char cmd[MAXLINE];

	signal(SIGPIPE, SIG_IGN);
	/* a root edited without owners leaves the builder's uid on this file: root's from the first run on */
	(void)chown("/usr/local/bin/scion-devshd", 0, 0);
	(void)chmod("/usr/local/bin/scion-devshd", 0755);
	for (;;) {
		size_t len = sizeof(cmd);

		memset(cmd, 0, sizeof(cmd));
		if (sysctlbyname("kern.hakios.devsh.cmd", cmd, &len, NULL, 0) != 0) {
			if (errno == ENOENT) {
				return 0;       /* devsh=0: off for this boot */
			}
			sleep(2);
			continue;
		}
		cmd[sizeof(cmd) - 1] = 0;
		if (cmd[0] == 0) {
			usleep(300000);
			continue;
		}
		run(cmd);
	}
}
