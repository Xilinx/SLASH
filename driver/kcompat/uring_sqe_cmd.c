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
 * Probe for the *newer* io_uring uring_cmd SQE payload accessor.
 *
 * Upstream removed `struct io_uring_cmd::cmd` (a const void * pointing at the
 * inline SQE command payload) and replaced it with `->sqe` plus the
 * io_uring_sqe_cmd() accessor.  This change was backported into distro kernels
 * (e.g. Ubuntu 6.8), so a LINUX_VERSION_CODE check is unreliable — probe the
 * accessor directly instead.
 *
 *   - SLASH_HAVE_URING_SQE_CMD=y  -> use io_uring_sqe_cmd(cmd->sqe)
 *   - SLASH_HAVE_URING_SQE_CMD=n  -> fall back to cmd->cmd (older kernels)
 *
 * This probe only governs the payload accessor; the rest of the uring_cmd
 * infrastructure is probed by uring_cmd.c (SLASH_HAVE_URING_CMD).
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/io_uring.h>
#if __has_include(<linux/io_uring/cmd.h>)
#include <linux/io_uring/cmd.h>
#endif

static int conftest_uring_sqe_cmd(struct io_uring_cmd *cmd)
{
    const void *payload = io_uring_sqe_cmd(cmd->sqe);

    (void)payload;
    return 0;
}

static int __init conftest_init(void)
{
    (void)conftest_uring_sqe_cmd;
    return 0;
}

static void __exit conftest_exit(void)
{
}

MODULE_LICENSE("GPL");
module_init(conftest_init);
module_exit(conftest_exit);
