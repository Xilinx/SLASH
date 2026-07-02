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
 * Probe for the io_uring uring_cmd *infrastructure* in the exact shape
 * slash_qdma.c uses, excluding the SQE payload accessor (that axis changed
 * independently and is probed separately by uring_sqe_cmd.c):
 *   - struct file_operations has a .uring_cmd member,
 *   - struct io_uring_cmd exposes ->pdu, ->file, and ->cmd_op,
 *   - io_uring_cmd_complete_in_task() takes a (cmd, issue_flags) callback,
 *   - io_uring_cmd_done() takes (cmd, ret, res2, issue_flags).
 *
 * This requires CONFIG_IO_URING and a kernel >= 5.19 with the settled
 * (>= 6.1) signatures; anywhere it fails to build, SLASH_HAVE_URING_CMD=n and
 * the optional async transfer path is compiled out.  The payload pointer is
 * read via the SLASH_HAVE_URING_SQE_CMD-selected accessor (see slash_qdma.c):
 * io_uring_sqe_cmd(cmd->sqe) on newer kernels, cmd->cmd on older ones.
 */

#include <linux/fs.h>
#include <linux/init.h>
#include <linux/module.h>
#include <linux/io_uring.h>
#if __has_include(<linux/io_uring/cmd.h>)
#include <linux/io_uring/cmd.h>
#endif

static void conftest_tw(struct io_uring_cmd *cmd, unsigned int issue_flags)
{
    io_uring_cmd_done(cmd, 0, 0, issue_flags);
}

static int conftest_uring_cmd(struct io_uring_cmd *cmd, unsigned int issue_flags)
{
    void *p = cmd->pdu;
    struct file *f = cmd->file;
    u32 op = cmd->cmd_op;

    (void)p;
    (void)f;
    (void)op;

    if (issue_flags & IO_URING_F_NONBLOCK)
        return -EAGAIN;

    io_uring_cmd_complete_in_task(cmd, conftest_tw);
    return -EIOCBQUEUED;
}

static const struct file_operations conftest_fops = {
    .owner = THIS_MODULE,
    .uring_cmd = conftest_uring_cmd,
};

static int __init conftest_init(void)
{
    (void)conftest_fops;
    return 0;
}

static void __exit conftest_exit(void)
{
}

MODULE_LICENSE("GPL");
module_init(conftest_init);
module_exit(conftest_exit);
