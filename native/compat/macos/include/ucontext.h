/* xbased on macOS: the signal context's types (ucontext_t, mcontext_t);
 * the system's <ucontext.h> is the deprecated makecontext API, which wants
 * _XOPEN_SOURCE. */
#ifndef NC_MACOS_UCONTEXT_H
#define NC_MACOS_UCONTEXT_H
#include <signal.h>
#include <sys/ucontext.h>
#endif
