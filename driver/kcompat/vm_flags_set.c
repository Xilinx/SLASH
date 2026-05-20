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

#include <linux/init.h>
#include <linux/mm.h>
#include <linux/module.h>

static int __init conftest_init(void)
{
    struct vm_area_struct *vma = NULL;

    vm_flags_set(vma, (vm_flags_t)0);
    return 0;
}

static void __exit conftest_exit(void)
{
}

MODULE_LICENSE("GPL");
module_init(conftest_init);
module_exit(conftest_exit);
