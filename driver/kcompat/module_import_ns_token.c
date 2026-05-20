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

/*
 * Token-form MODULE_IMPORT_NS probe.
 *
 * Pre-6.13:  MODULE_IMPORT_NS(ns) = MODULE_INFO(import_ns, __stringify(ns))
 *            -> token form is the documented usage.
 * 6.13+:     MODULE_IMPORT_NS(ns) = MODULE_INFO(import_ns, ns)
 *            -> token form fails to compile (DMA_BUF undefined).
 *
 * So this probe succeeds iff the token form is the right one to use.
 * Compile success on a pre-6.13 kernel that still accepts the string
 * form silently produces the wrong namespace string at runtime, which
 * is why we probe the token form (precise) instead of the string form
 * (ambiguous on older kernels).
 */
#include <linux/init.h>
#include <linux/module.h>

static int __init conftest_init(void)
{
    return 0;
}

static void __exit conftest_exit(void)
{
}

MODULE_LICENSE("GPL");
MODULE_IMPORT_NS(DMA_BUF);
module_init(conftest_init);
module_exit(conftest_exit);
