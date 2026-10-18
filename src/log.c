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

static char last_error[512];

void qx_err(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(last_error, sizeof(last_error), fmt, ap);
    va_end(ap);
    fprintf(stderr, "error: %s\n", last_error);
    fflush(stderr);
}

const char *qx_last_error(void)
{
    return last_error;
}
