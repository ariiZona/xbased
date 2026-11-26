/*
 * xbased on macOS x86-64: what the core takes from Linux and macOS has
 * otherwise or not at all, included before every source of the core's own
 * targets (nc_linux_compat, cmake/macos.cmake). Third-party code doesn't
 * see it.
 *
 * Timed waits: macOS's condition variables have no clock attribute (their
 * deadlines are CLOCK_REALTIME's), and the core's are CLOCK_MONOTONIC's.
 * The wait takes the deadline's clock by its size, as compat/win does: a
 * CLOCK_REALTIME one is decades of seconds since 1970, a CLOCK_MONOTONIC
 * one the time since boot. It then waits that long, relative.
 */
#ifndef NC_MACOS_POSIX_H
#define NC_MACOS_POSIX_H

#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#ifdef __cplusplus
extern "C" {
#endif

extern char **environ;

/* the stat times' names */
#define st_atim st_atimespec
#define st_mtim st_mtimespec
#define st_ctim st_ctimespec

/* sockets: the flags socket() takes on Linux; the core sets non-blocking
 * on its own after (nc_net_nonblock) */
#ifndef SOCK_NONBLOCK
#define SOCK_NONBLOCK 0
#endif
#ifndef SOCK_CLOEXEC
#define SOCK_CLOEXEC 0
#endif

#define CLOCK_MONOTONIC_COARSE CLOCK_MONOTONIC

static inline int nc_gettid(void)
{
    uint64_t tid = 0;
    pthread_threadid_np(NULL, &tid);
    return (int)tid;
}
#define gettid nc_gettid

static inline int64_t nc_ts_ns(const struct timespec *t)
{
    return (int64_t)t->tv_sec * 1000000000LL + t->tv_nsec;
}

static inline int nc_condattr_setclock(pthread_condattr_t *a, clockid_t c)
{
    (void)a;
    (void)c;
    return 0; /* the deadline's clock is the wait's to tell (above) */
}
#define pthread_condattr_setclock nc_condattr_setclock

static inline int nc_cond_timedwait(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *abstime)
{
    struct timespec now, rel;
    clock_gettime(abstime->tv_sec > 1000000000 ? CLOCK_REALTIME : CLOCK_MONOTONIC, &now);
    int64_t left = nc_ts_ns(abstime) - nc_ts_ns(&now);
    if (left < 0) {
        left = 0;
    }
    rel.tv_sec = (time_t)(left / 1000000000LL);
    rel.tv_nsec = (long)(left % 1000000000LL);
    return pthread_cond_timedwait_relative_np(c, m, &rel);
}
#define pthread_cond_timedwait nc_cond_timedwait

#ifndef TIMER_ABSTIME
#define TIMER_ABSTIME 1
#endif
static inline int nc_clock_nanosleep(clockid_t clk, int flags, const struct timespec *req, struct timespec *rem)
{
    int64_t ns = nc_ts_ns(req);
    if (flags & TIMER_ABSTIME) {
        struct timespec now;
        clock_gettime(clk, &now);
        ns -= nc_ts_ns(&now);
    }
    if (ns <= 0) {
        return 0;
    }
    const struct timespec d = {(time_t)(ns / 1000000000LL), (long)(ns % 1000000000LL)};
    return nanosleep(&d, rem) == 0 ? 0 : errno;
}
#define clock_nanosleep nc_clock_nanosleep

/* another thread's CPU clock: none (the callers do without) */
static inline int nc_getcpuclockid(pthread_t t, clockid_t *c)
{
    (void)t;
    (void)c;
    return ENOTSUP;
}
#define pthread_getcpuclockid nc_getcpuclockid

/* macOS names the calling thread only */
static inline int nc_setname_np(pthread_t t, const char *name)
{
    return pthread_equal(t, pthread_self()) ? pthread_setname_np(name) : 0;
}
#define pthread_setname_np nc_setname_np

/* eventfd's wakeups as a pipe's bytes (compat/linux/linux.c): the fd to poll
 * and read is the read end, the write end is kept beside it */
int nc_eventfd(void);
void nc_eventfd_write(int fd);
void nc_eventfd_read(int fd);

#ifdef __cplusplus
}
#endif

#endif
