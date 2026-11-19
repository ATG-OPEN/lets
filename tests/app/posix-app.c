/* A LETS program as the owner wants them: standard headers and lets-app.h only (make app-check
   compiles it for XC16, Cortex-M/newlib and the PC). */
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <sys/time.h>
#include <sys/timex.h>
#include <time.h>
#include <unistd.h>

#include <lets-app.h>

static int fd = -1;

static int step(void *ctx)
{
	struct pollfd p = {fd, POLLIN, 0};
	struct timespec t;
	char b[16];

	(void)ctx;
	clock_gettime(CLOCK_MONOTONIC, &t);
	if (read(fd, b, sizeof(b)) < 0 && errno != EAGAIN)
		return -1;
	return poll(&p, 1, 1000) ? 0 : 1000;
}

int echo_main(int argc, char *argv[])
{
	struct stat st;
	struct timex x = {0};
	struct timeval tv;
	DIR *d = opendir("/");

	if (d) {
		struct dirent *e = readdir(d);
		(void)e;
		closedir(d);
	}
	adjtimex(&x);
	gettimeofday(&tv, NULL);
	sync();
	if (argc < 2 || stat(argv[1], &st) < 0 || (fd = open(argv[1], O_RDWR, 0)) < 0)
		return -1;
	(void)ioctl(fd, 0);
	return lets_spawn("echo", step, NULL);
}
