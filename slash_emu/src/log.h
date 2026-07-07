// ################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//  and associated documentation files (the "Software"), to deal in the Software without
//  restriction, including without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all copies or
//  substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ################################################################################################

#pragma once

// Thin ergonomic wrapper over sd_journal_print(3).  The daemon runs only under
// systemd, so all diagnostics go to the journal with proper syslog severities
// (LOG_INFO / LOG_WARNING / LOG_ERR).  The journal already records the unit
// identity, timestamp, and PID, so messages carry neither a "[slash_emu]" prefix
// nor a trailing newline.

#include <cstdarg>

#include <systemd/sd-journal.h>

namespace slash_emu {

// C-style variadic forwarders to sd_journal_printv.  The format(printf,1,2)
// attribute lets -Wformat check the arguments against the format string exactly
// as for printf (variadic templates cannot carry this attribute).

__attribute__((format(printf, 1, 2)))
inline void log_info(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sd_journal_printv(LOG_INFO, fmt, ap);
    va_end(ap);
}

__attribute__((format(printf, 1, 2)))
inline void log_warn(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sd_journal_printv(LOG_WARNING, fmt, ap);
    va_end(ap);
}

__attribute__((format(printf, 1, 2)))
inline void log_err(const char* fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    sd_journal_printv(LOG_ERR, fmt, ap);
    va_end(ap);
}

} // namespace slash_emu
