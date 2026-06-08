/**
 * Copyright (C) 2026 Advanced Micro Devices, Inc. All rights reserved.
 * This program is free software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program; if
 * not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

#ifndef SLASH_COMPAT_H
#define SLASH_COMPAT_H

#include <linux/mm.h>
#include <linux/module.h>
#include <linux/timer.h>

/*
 * Compat shims selected by the kcompat probes in driver/kcompat/.
 * If the modern form is detected (SLASH_HAVE_*), use it; otherwise
 * fall back to the legacy form. The probes are exhaustive, so no
 * #error path is needed.
 */

static inline void slash_vm_flags_set(struct vm_area_struct *vma, vm_flags_t flags)
{
#if defined(SLASH_HAVE_VM_FLAGS_SET)
    vm_flags_set(vma, flags);
#else
    vma->vm_flags |= flags;
#endif
}

/*
 * MODULE_IMPORT_NS argument form.
 *
 * Pre-6.13: bare token, e.g. MODULE_IMPORT_NS(DMA_BUF). The kernel
 *           macro internally __stringify()s the argument, so passing a
 *           string literal would produce a runtime namespace mismatch.
 * 6.13+:    string literal, e.g. MODULE_IMPORT_NS("DMA_BUF"). The
 *           kernel macro stopped stringifying, so passing a bare token
 *           fails to compile.
 *
 * We probe the token form (precise cutover at 6.13) and stringify here
 * when it's no longer accepted.
 */
#if defined(SLASH_HAVE_MODULE_IMPORT_NS_TOKEN)
#define SLASH_MODULE_IMPORT_NS(ns) MODULE_IMPORT_NS(ns)
#else
#define SLASH_MODULE_IMPORT_NS(ns) MODULE_IMPORT_NS(#ns)
#endif

/*
 * from_timer() was renamed to timer_container_of() upstream in v6.16
 * (commit 41cb08555c41) and backported by RHEL/CentOS 9.8 (kernel
 * 5.14.0-687; 9.7 / 5.14.0-611 and earlier still ship from_timer) into the
 * 5.14 baseline, so a LINUX_VERSION_CODE / RHEL_RELEASE_CODE check is
 * unreliable across the 9.x rebuilds. Both names are typeof()-based macros,
 * so detect them directly and prefer the kernel's own API:
 *   1. kernel still defines from_timer()          -> use it as-is (no redefine)
 *   2. kernel renamed it to timer_container_of()  -> delegate to that
 *   3. neither exists                             -> hand-roll the historical body
 * <linux/timer.h> is included above so the guard sees whichever name the
 * kernel defines, regardless of -include ordering.
 */
#ifndef from_timer
#  ifdef timer_container_of
#    define from_timer(var, callback_timer, timer_fieldname) \
	timer_container_of(var, callback_timer, timer_fieldname)
#  else
#    define from_timer(var, callback_timer, timer_fieldname) \
	container_of(callback_timer, typeof(*var), timer_fieldname)
#  endif
#endif

#endif /* SLASH_COMPAT_H */
