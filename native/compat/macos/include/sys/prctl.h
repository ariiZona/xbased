/* xbased on macOS: prctl, with nothing to set (the timer slack is Linux's). */
#ifndef NC_MACOS_PRCTL_H
#define NC_MACOS_PRCTL_H
#define PR_SET_TIMERSLACK 29
static inline int prctl(int option, ...)
{
    (void)option;
    return 0;
}
#endif
