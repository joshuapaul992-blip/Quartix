/*
 * Quartix - console logging.
 * Copyright (C) 2026 Quartix contributors. Licensed under GPLv3 (see LICENSE).
 */
#include "quartix.h"
#include <stdarg.h>
#include <stdio.h>

void qx_log(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vfprintf(stdout, fmt, ap);
    va_end(ap);
    fputc('\n', stdout);
    fflush(stdout);
}

void qx_err(const char *fmt, ...)
{
    va_list ap;
    fputs("error: ", stderr);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    fflush(stderr);
}
