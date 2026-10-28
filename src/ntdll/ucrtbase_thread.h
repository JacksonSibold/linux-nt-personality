/* ucrtbase_thread.h - the per-thread record of ucrtbase.dll (included by ucrtbase.c and its host tests). */
#ifndef UCRTBASE_THREAD_H
#define UCRTBASE_THREAD_H

#include "ucrt_time.h"

typedef struct uct_thread {
    int err;
    unsigned long dos;
    ucrt_tm tm;
    char timebuf[32];
    ucrt_wchar wtimebuf[32];
    char *tok;           /* strtok and wcstok continue here */
    ucrt_wchar *wtok;
    char cvt[400];       /* _ecvt and _fcvt results */
    char errbuf[160];    /* _strerror */
    ucrt_wchar werrbuf[160];
} uct_thread;

#endif /* UCRTBASE_THREAD_H */
