/*
 * SPDX-License-Identifier: MIT-0
 *
 * HakiosAudio.driver's level meter (audio_plugin.c includes this file once): how loud the speaker's samples are, said
 * on the console, so a test can see that the volume buttons and Control Centre's slider change what is played without
 * anybody listening.
 *
 * On the phones, the virtual audio device keeps the iPhone's speaker in software volume mode. It sets the plug-in's
 * volume control once, to its top, and from then on it scales the samples itself (audio_plugin.c, its first comment).
 * The plug-in's control and the kernel's level therefore never move when the volume does. Only the samples show it.
 *
 * The switch is the file /private/var/local/hakios-audio-level, read each time the speaker starts playing:
 *   "log"  one console line a second while the speaker plays (at most LEVEL_LINES a run): the loudest sample and the
 *          average power of that second, in dB under full scale. The samples still go to the kernel and are heard.
 *   "dry"  the same lines, and the samples do NOT go to the kernel: nothing is heard, the amplifiers stay off (the kernel
 *          starts them only when it is given sound). For tests at night or when the owner must not be disturbed.
 * Without the file (the default) nothing changes: the samples go to the kernel as before and nothing is said.
 * A step of the volume moves both numbers by the step's size in dB; silence reads "silent".
 *
 * Written 17 November 2026. NOT RUN ON A PHONE.
 */
#ifndef HAKIOS_AUDIO_LEVEL_H
#define HAKIOS_AUDIO_LEVEL_H

#include <fcntl.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define LEVEL_SWITCH    "/private/var/local/hakios-audio-level"
#define LEVEL_LINES     120

static int level_mode;                  /* 0 off, 1 log, 2 dry; set when the speaker starts, read by the I/O thread */
static int level_console = -1;
static unsigned level_lines;            /* lines said this run */
static uint64_t level_frames;           /* frames of the second being measured */
static uint64_t level_seconds;
static int level_peak;
static double level_power;              /* sum of squares, all channels */

/* When the speaker starts: read the switch; the text goes after "playing" in the plug-in's own line. */
static const char *
level_start(void)
{
	char word[8] = { 0 };
	int fd = open(LEVEL_SWITCH, O_RDONLY);

	level_mode = 0;
	if (fd >= 0) {
		if (read(fd, word, sizeof(word) - 1) > 0) {
			level_mode = strncmp(word, "dry", 3) == 0 ? 2 : strncmp(word, "log", 3) == 0 ? 1 : 0;
		}
		close(fd);
	}
	if (level_mode != 0 && level_console < 0) {
		level_console = open("/dev/console", O_WRONLY | O_NOCTTY | O_NONBLOCK);
	}
	level_lines = 0;
	level_frames = level_seconds = 0;
	level_peak = 0;
	level_power = 0;
	if (level_mode != 0 && level_console >= 0) {
		dprintf(level_console, "hakios-audio-level: the speaker starts; %s\n", level_mode == 2 ?
		    "dry run: its samples are measured and NOT played" : "its samples are measured and played");
	}
	return level_mode == 2 ? " (level meter, dry run: nothing is played)" : level_mode == 1 ? " (level meter on)" : "";
}

static double
level_db(double value)
{
	return value > 0 ? 20 * log10(value) : -200;
}

/*
 * The I/O thread, with each buffer the speaker is given (16-bit, HAKIOS_AUDIO_CHANNELS a frame). True: dry run, the
 * caller must not pass the samples on. No allocation; a console line once a second at most.
 */
static int
level_take(const void *buffer, uint32_t frames)
{
	const int16_t *s = buffer;

	if (level_mode == 0) {
		return 0;
	}
	for (uint32_t i = 0; i < frames * HAKIOS_AUDIO_CHANNELS; i++) {
		int v = s[i] < 0 ? -(int)s[i] : s[i];

		level_peak = v > level_peak ? v : level_peak;
		level_power += (double)s[i] * s[i];
	}
	level_frames += frames;
	if (level_frames >= HAKIOS_AUDIO_RATE) {
		double rms = sqrt(level_power / ((double)level_frames * HAKIOS_AUDIO_CHANNELS)) / 32768;

		level_seconds++;
		if (level_console >= 0 && level_lines < LEVEL_LINES) {
			level_lines++;
			if (level_peak == 0) {
				dprintf(level_console, "hakios-audio-level: second %llu: silent\n", (unsigned long long)level_seconds);
			} else {
				dprintf(level_console, "hakios-audio-level: second %llu: peak %.1f dBFS, average %.1f dBFS\n",
				    (unsigned long long)level_seconds, level_db(level_peak / 32768.0), level_db(rms));
			}
		}
		level_frames = 0;
		level_peak = 0;
		level_power = 0;
	}
	return level_mode == 2;
}

#endif
