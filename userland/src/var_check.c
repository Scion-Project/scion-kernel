/*
 * Why /private/var is not the kept volume: what init found is printed before the console's ring buffer catches it, so this
 * says it again from a job, late enough to be in the log. It reports whether the data volume's node exists, what
 * /private/var is (statfs), and tries the same HFS Plus mount init makes -- READ ONLY, on a scratch directory, and
 * unmounts it again -- so that its errno is seen.
 *
 *   usage: var_check [DEV]     DEV a node of /dev (default: var_hfs= from the boot-args, else ufs_USERDATA_data)
 */
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mount.h>
#include <sys/param.h>
#include <sys/stat.h>
#include <sys/sysctl.h>
#include <unistd.h>

int
main(int argc, char **argv)
{
	char args[1024] = "", name[64] = "ufs_USERDATA_data", dev[80];
	size_t size = sizeof(args) - 1;
	struct statfs sf;
	struct stat st;
	struct {
		const char *fspec;
		uint32_t uid, gid;
		uint16_t mask, pad;
		uint32_t encoding;
		int tz[2];
		int flags, journal_tbuffer_size, journal_flags, journal_disable;
	} hargs;
	const char *scratch = "/private/var/tmp/var_check";

	if (argc > 1) {
		snprintf(name, sizeof(name), "%s", argv[1]);
	} else if (sysctlbyname("kern.bootargs", args, &size, NULL, 0) == 0) {
		char *p = strstr(args, "var_hfs=");

		if (p != NULL) {
			p += 8;
			size_t n = strcspn(p, " ");
			if (n > 0 && n < sizeof(name)) {
				memcpy(name, p, n);
				name[n] = 0;
			}
		}
	}
	snprintf(dev, sizeof(dev), "/dev/%s", name);
	if (stat(dev, &st) != 0) {
		printf("var_check: %s: %s\n", dev, strerror(errno));
	} else {
		printf("var_check: %s exists (%s, %lld bytes)\n", dev, S_ISBLK(st.st_mode) ? "block" : S_ISCHR(st.st_mode) ? "char" :
		    "other", (long long)st.st_size);
	}
	if (statfs("/private/var", &sf) == 0) {
		printf("var_check: /private/var is %s from %s, %llu of %llu blocks of %u free\n", sf.f_fstypename, sf.f_mntfromname,
		    (unsigned long long)sf.f_bfree, (unsigned long long)sf.f_blocks, sf.f_bsize);
	}
	mkdir(scratch, 0755);
	memset(&hargs, 0, sizeof(hargs));
	hargs.fspec = dev;
	if (mount("hfs", scratch, MNT_RDONLY, &hargs) != 0) {
		printf("var_check: mount hfs %s read only: %s (errno %d)\n", dev, strerror(errno), errno);
	} else {
		if (statfs(scratch, &sf) == 0) {
			printf("var_check: mount hfs %s read only: OK, %s, %llu of %llu blocks free\n", dev, sf.f_fstypename,
			    (unsigned long long)sf.f_bfree, (unsigned long long)sf.f_blocks);
		}
		unmount(scratch, 0);
	}
	rmdir(scratch);
	return 0;
}
