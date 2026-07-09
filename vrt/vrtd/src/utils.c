/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#include "utils.h"

#include <stdarg.h>
#include <stdio.h>
#include <pwd.h>
#include <string.h>
#include <syslog.h>

static bool log_to_stderr = false;

static const char *log_priority_name(int priority)
{
    switch (priority) {
        case LOG_EMERG:   return "EMERG";
        case LOG_ALERT:   return "ALERT";
        case LOG_CRIT:    return "CRIT";
        case LOG_ERR:     return "ERR";
        case LOG_WARNING: return "WARNING";
        case LOG_NOTICE:  return "NOTICE";
        case LOG_INFO:    return "INFO";
        case LOG_DEBUG:   return "DEBUG";
        default:          return "LOG";
    }
}

void vrtd_log_init(void)
{
    const char *log_stderr = getenv("VRTD_LOG_STDERR");
    log_to_stderr = log_stderr != NULL && strcmp(log_stderr, "1") == 0;
}

void vrtd_log(int priority, const char *fmt, ...)
{
    va_list args;

    va_start(args, fmt);
    if (log_to_stderr) {
        (void) fprintf(stderr, "vrtd[%s]: ", log_priority_name(priority));
        (void) vfprintf(stderr, fmt, args);
        (void) fputc('\n', stderr);
    } else {
        (void) sd_journal_printv(priority, fmt, args);
    }
    va_end(args);
}

const char *uid_to_username(uid_t uid, char *buf, size_t bufsz)
{
    struct passwd pwent, *pw = NULL;
    if (getpwuid_r(uid, &pwent, buf, bufsz, &pw) != 0 || pw == NULL) {
        buf[0] = '\0';
        return buf;
    }
    return pw->pw_name;
}
