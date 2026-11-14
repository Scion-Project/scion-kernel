# iOS 27 on the S9: a shell line over the USB console (test roots only)

The USB console (`drivers/usb/hakios.dwc3.c`) answers `sysctl`, `touch`, `shot` and the rest, but until now it could not ask
user space anything: every question about a program on the phone meant a launchd job on the root, a reboot, and reading a
log back from the root in TWRP. `devsh` is a line of shell over the same cable, for a test image.

## Pieces

| | |
|---|---|
| Kernel, boot-arg `devsh=1` (off by default) | The console's `sh WORDS` queues `WORDS` (8 lines at most). A console line is at most 127 bytes: a line that ends in `\` is continued by the next `sh` line, up to 1023 bytes |
| `kern.hakios.devsh.cmd` | Root only. Reading it takes the next queued line off the queue (empty when none) |
| `kern.hakios.devsh.out` | Root only. A string written here is printed on the console as `sh| ...` |
| `tools/ios27/devsh/devshd.c` | The helper: a root job that polls `devsh.cmd` (every 0.3 s while idle), runs the line with the prefix's `sh`, sends its output back line by line, then `rc=N (T s)` |
| `tools/ios27/devsh/devsh-step.sh ROOT [undo]` | Puts the helper on an attached root as `/usr/local/bin/scion-devshd` with the launchd job `com.scion.devsh` |

Without `devsh=1` both sysctls answer `ENOENT`, the console says `sh: off`, and the helper exits at once and stays off for
the boot (launchd does not restart a job that exited 0).

Prefixes of a line: `@mobile` runs it as `mobile` (501) with `HOME=/var/mobile`; `@bg` starts it and does not wait (output
to `/private/var/tmp/devsh-bg.log`); `@t=N` sets the time limit to N seconds (default 60; the process group is killed
after it). They combine: `@mobile @t=120 LINE`.

## Use

    printf 'sh id; uname -m\n' > /dev/cu.usbmodemNN      # or the run scripts' con.sh 'sh id'
    # in the console log:
    usb> sh id; uname -m
    usb: sh: #1 queued
    sh| uid=0(root) gid=0(wheel) ...
    sh| iPhone10,1
    sh| rc=0 (0.1 s)

## What it is not

Nothing listens on a network: the only way in is the console on the USB cable, which already reboots the phone and taps its
screen. Still, it is a root shell for anyone holding the cable, so it is for test roots: the boot-arg is off by default, no
release root carries the helper, and a test session ends with `devsh-step.sh ROOT undo`.
