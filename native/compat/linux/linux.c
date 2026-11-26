/*
 * xbased on Linux: the thread helpers the Windows build's compat layer has
 * (nc_win_thread_name, _priority), and what an older glibc lacks
 * (arc4random: 2.36, strlcpy: 2.38) for the release built on one.
 */
#if !defined(_GNU_SOURCE) && !defined(__APPLE__)
#define _GNU_SOURCE
#endif
#include <ctype.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <stdint.h>
#ifdef __APPLE__
#include <fcntl.h>
#else
#include <sys/random.h>
#endif

#if defined(__GLIBC__)
#if !__GLIBC_PREREQ(2, 36)
void arc4random_buf(void *buf, size_t n)
{
    uint8_t *p = buf;
    while (n) {
        const ssize_t got = getrandom(p, n, 0);
        if (got > 0) {
            p += got;
            n -= (size_t)got;
        }
    }
}

uint32_t arc4random(void)
{
    uint32_t v;
    arc4random_buf(&v, sizeof(v));
    return v;
}

uint32_t arc4random_uniform(uint32_t bound)
{
    if (bound < 2) {
        return 0;
    }
    /* the values below 2^32 mod bound would come up once more often */
    const uint32_t min = -bound % bound;
    for (;;) {
        const uint32_t r = arc4random();
        if (r >= min) {
            return r % bound;
        }
    }
}
#endif
#endif

#if defined(__GLIBC__)
#if !__GLIBC_PREREQ(2, 38)
size_t strlcpy(char *dst, const char *src, size_t n)
{
    const size_t len = strlen(src);
    if (n) {
        const size_t k = len < n - 1 ? len : n - 1;
        memcpy(dst, src, k);
        dst[k] = 0;
    }
    return len;
}
#endif
#endif

void nc_win_thread_name(const char *name)
{
    char n[16];
    snprintf(n, sizeof(n), "%s", name);
    pthread_setname_np(pthread_self(), n);
}

void nc_win_thread_priority(int level)
{
    (void)level;    /* raising it needs privileges on Linux; the scheduler's default */
}

#ifdef __APPLE__
/* eventfd as a pipe (nc_macos_posix.h): the read end is the fd, its write
 * end kept here; both non-blocking, a full pipe has a wakeup pending anyway */
static int g_efd_w[256];

int nc_eventfd(void)
{
    int p[2];
    if (pipe(p) != 0) {
        return -1;
    }
    if (p[0] >= 256) {
        close(p[0]);
        close(p[1]);
        errno = EMFILE;
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        fcntl(p[i], F_SETFL, fcntl(p[i], F_GETFL) | O_NONBLOCK);
        fcntl(p[i], F_SETFD, FD_CLOEXEC);
    }
    g_efd_w[p[0]] = p[1];
    return p[0];
}

void nc_eventfd_write(int fd)
{
    const char one = 1;
    (void)!write(g_efd_w[fd], &one, 1);
}

void nc_eventfd_read(int fd)
{
    char buf[64];
    while (read(fd, buf, sizeof(buf)) > 0) {
    }
}
#endif
