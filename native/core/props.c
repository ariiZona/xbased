/*
 * xbased native core — the debug switches (props.h).
 */
#include "native/core/props.h"

#include <ctype.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#define PROPS_MAX_SET 16
#define PROPS_FILE_MAX (64 * 1024)

static pthread_mutex_t g_lock = PTHREAD_MUTEX_INITIALIZER;
static char *g_text;                /* props.ini's text, lines NUL-separated */
static size_t g_len;
static uint64_t g_stamp;            /* its modification time */
static uint64_t g_checked_ms;       /* when it was last looked at */

/* values set while running (fast forward's debug.xbased.uncap), over the file's */
static struct {
    char name[NC_PROP_NAME_MAX];
    char value[NC_PROP_VALUE_MAX];
} g_set[PROPS_MAX_SET];

static void props_path(char *out, size_t n)
{
#ifdef _WIN32
    char exe[MAX_PATH];
    DWORD len = GetModuleFileNameA(NULL, exe, sizeof(exe));
    while (len && exe[len - 1] != '\\' && exe[len - 1] != '/') {
        len--;
    }
    exe[len] = 0;
#else
    char exe[PATH_MAX];
#ifdef __APPLE__
    uint32_t sz = sizeof(exe);
    char raw[PATH_MAX];
    if (_NSGetExecutablePath(raw, &sz) != 0 || !realpath(raw, exe)) {
        snprintf(out, n, "props.ini");
        return;
    }
#else
    const ssize_t k = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (k <= 0) {
        snprintf(out, n, "props.ini");
        return;
    }
    exe[k] = 0;
#endif
    char *slash = strrchr(exe, '/');
    if (slash) {
        slash[1] = 0;
    }
#endif
    snprintf(out, n, "%sprops.ini", exe);
}

/* The file's modification time, 0 when there is none. */
static uint64_t props_stamp(const char *path)
{
#ifdef _WIN32
    WIN32_FILE_ATTRIBUTE_DATA fa;
    if (!GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) {
        return 0;
    }
    return (uint64_t)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime;
#else
    struct stat st;
    if (stat(path, &st) != 0) {
        return 0;
    }
    return (uint64_t)st.st_mtim.tv_sec * 1000000000u + (uint64_t)st.st_mtim.tv_nsec;
#endif
}

/* The file again when it changed, looked at no more than every 100 ms. */
static void props_refresh(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    const uint64_t now = (uint64_t)t.tv_sec * 1000u + (uint64_t)t.tv_nsec / 1000000u;
    if (g_checked_ms && now - g_checked_ms < 100) {
        return;
    }
    g_checked_ms = now;
    char path[4096 + 16];
    props_path(path, sizeof(path));
    const uint64_t stamp = props_stamp(path);
    if (!stamp) {
        free(g_text);
        g_text = NULL;
        g_len = 0;
        return;
    }
    if (g_text && stamp == g_stamp) {
        return;
    }
    FILE *f = fopen(path, "rb");
    if (!f) {
        return;
    }
    char *buf = malloc(PROPS_FILE_MAX);
    const size_t n = buf ? fread(buf, 1, PROPS_FILE_MAX - 1, f) : 0;
    fclose(f);
    if (!buf) {
        return;
    }
    buf[n] = 0;
    for (size_t i = 0; i < n; i++) {
        if (buf[i] == '\r' || buf[i] == '\n') {
            buf[i] = 0;
        }
    }
    free(g_text);
    g_text = buf;
    g_len = n;
    g_stamp = stamp;
}

void nc_prop_set(const char *name, const char *value)
{
    pthread_mutex_lock(&g_lock);
    int slot = -1;
    for (int i = 0; i < PROPS_MAX_SET; i++) {
        if (!strcmp(g_set[i].name, name)) {
            slot = i;
            break;
        }
        if (!g_set[i].name[0] && slot < 0) {
            slot = i;
        }
    }
    if (slot >= 0) {
        snprintf(g_set[slot].name, NC_PROP_NAME_MAX, "%s", value ? name : "");
        snprintf(g_set[slot].value, NC_PROP_VALUE_MAX, "%s", value ? value : "");
    }
    pthread_mutex_unlock(&g_lock);
}

/* name's line in the file (the lock held): its value in value, its length; -1 if none. */
static int props_file_get(const char *name, char *value)
{
    const size_t nl = strlen(name);
    for (size_t i = 0; g_text && i < g_len;) {
        const char *line = g_text + i;
        const size_t ll = strlen(line);
        i += ll + 1;
        while (*line == ' ' || *line == '\t') {
            line++;
        }
        if (line[0] == '#' || strncmp(line, name, nl)) {
            continue;
        }
        const char *p = line + nl;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        if (*p != '=') {
            continue;
        }
        p++;
        while (*p == ' ' || *p == '\t') {
            p++;
        }
        snprintf(value, NC_PROP_VALUE_MAX, "%s", p);
        size_t vl = strlen(value);
        while (vl && (value[vl - 1] == ' ' || value[vl - 1] == '\t')) {
            value[--vl] = 0;
        }
        return (int)vl;
    }
    return -1;
}

int nc_prop_get(const char *name, char *value)
{
    value[0] = 0;
    pthread_mutex_lock(&g_lock);
    for (int i = 0; i < PROPS_MAX_SET; i++) {
        if (g_set[i].name[0] && !strcmp(g_set[i].name, name)) {
            snprintf(value, NC_PROP_VALUE_MAX, "%s", g_set[i].value);
            pthread_mutex_unlock(&g_lock);
            return (int)strlen(value);
        }
    }
    props_refresh();
    const int got = props_file_get(name, value);
    pthread_mutex_unlock(&g_lock);
    if (got >= 0) {
        return got;
    }
    /* the environment: DEBUG_XBASED_PERF for debug.xbased.perf */
    char env[NC_PROP_NAME_MAX * 2 + 8];
    size_t k = 0;
    for (; name[k] && k < sizeof(env) - 1; k++) {
        env[k] = name[k] == '.' ? '_' : (char)toupper((unsigned char)name[k]);
    }
    env[k] = 0;
    const char *e = getenv(env);
    if (!e) {
        return 0;
    }
    snprintf(value, NC_PROP_VALUE_MAX, "%s", e);
    return (int)strlen(value);
}
