// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Basic QDMA queue-pair lifecycle test.
 *
 * Uses a kselftest FIXTURE to manage the control device and queue pair,
 * with FIXTURE_TEARDOWN guaranteeing cleanup even if assertions fail.
 *
 * The DMA target address defaults to 0x0 and can be overridden via the
 * SLASH_TEST_DMA_ADDR environment variable.
 */

#include "kselftest_harness.h"

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ioctl.h>

#include <slash/uapi/slash_interface.h>

#define QDMA_CTL_DEV   "/dev/slash_qdma_ctl0"
#define TRANSFER_SIZE  4096
#define DDR_BASE_ADDRESS 0x60000000000ULL

/* ---------- helpers ---------- */

static uint64_t get_dma_addr(void)
{
	const char *val = getenv("SLASH_TEST_DMA_ADDR");

	if (val)
		return strtoull(val, NULL, DDR_BASE_ADDRESS);
	return 0;
}

static int qpair_op(int fd, uint32_t qid, uint32_t op)
{
	struct slash_qdma_qpair_op req;

	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	req.qid  = qid;
	req.op   = op;

	return ioctl(fd, SLASH_QDMA_IOCTL_Q_OP, &req);
}

static void fill_pattern(uint8_t *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = (uint8_t)(i & 0xff);
}

/* ---------- fixture ---------- */

FIXTURE(qdma)
{
	int ctl_fd;
	uint32_t qid;
	int io_fd;
	int qpair_added;
	int qpair_started;
};

FIXTURE_SETUP(qdma)
{
	self->ctl_fd = -1;
	self->io_fd  = -1;
	self->qpair_added   = 0;
	self->qpair_started = 0;

	if (access(QDMA_CTL_DEV, F_OK) != 0)
		SKIP(return, "QDMA device not found (%s)", QDMA_CTL_DEV);

	self->ctl_fd = open(QDMA_CTL_DEV, O_RDWR);
	ASSERT_GE(self->ctl_fd, 0);
}

FIXTURE_TEARDOWN(qdma)
{
	if (self->io_fd >= 0)
		close(self->io_fd);

	if (self->qpair_started)
		qpair_op(self->ctl_fd, self->qid, SLASH_QDMA_QUEUE_OP_STOP);

	if (self->qpair_added)
		qpair_op(self->ctl_fd, self->qid, SLASH_QDMA_QUEUE_OP_DEL);

	if (self->ctl_fd >= 0)
		close(self->ctl_fd);
}

/* ---------- tests ---------- */

TEST_F(qdma, query_info)
{
	struct slash_qdma_info info;

	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);

	EXPECT_GE(ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_INFO, &info), 0);
}

TEST_F(qdma, qpair_lifecycle)
{
	struct slash_qdma_qpair_add add;
	struct slash_qdma_qpair_fd_request fd_req;

	/* Add queue pair */
	memset(&add, 0, sizeof(add));
	add.size     = sizeof(add);
	add.mode     = 0;   /* MM mode */
	add.dir_mask = 0x3; /* H2C | C2H */

	ASSERT_GE(ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_QPAIR_ADD, &add), 0);
	self->qid = add.qid;
	self->qpair_added = 1;

	/* Start queue pair */
	ASSERT_GE(qpair_op(self->ctl_fd, self->qid, SLASH_QDMA_QUEUE_OP_START), 0);
	self->qpair_started = 1;

	/* Get I/O fd */
	memset(&fd_req, 0, sizeof(fd_req));
	fd_req.size  = sizeof(fd_req);
	fd_req.qid   = self->qid;
	fd_req.flags = O_CLOEXEC;

	self->io_fd = ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_QPAIR_GET_FD, &fd_req);
	ASSERT_GE(self->io_fd, 0);

	/* Stop queue pair */
	ASSERT_GE(qpair_op(self->ctl_fd, self->qid, SLASH_QDMA_QUEUE_OP_STOP), 0);
	self->qpair_started = 0;

	/* Delete queue pair */
	ASSERT_GE(qpair_op(self->ctl_fd, self->qid, SLASH_QDMA_QUEUE_OP_DEL), 0);
	self->qpair_added = 0;
}

TEST_F(qdma, write_read_verify)
{
	struct slash_qdma_qpair_add add;
	struct slash_qdma_qpair_fd_request fd_req;
	uint8_t *write_buf, *read_buf;
	uint64_t dma_addr = get_dma_addr();
	ssize_t ret;

	/* Add + start queue pair */
	memset(&add, 0, sizeof(add));
	add.size     = sizeof(add);
	add.mode     = 0;
	add.dir_mask = 0x3;

	ASSERT_GE(ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_QPAIR_ADD, &add), 0);
	self->qid = add.qid;
	self->qpair_added = 1;

	ASSERT_GE(qpair_op(self->ctl_fd, self->qid, SLASH_QDMA_QUEUE_OP_START), 0);
	self->qpair_started = 1;

	/* Get I/O fd */
	memset(&fd_req, 0, sizeof(fd_req));
	fd_req.size  = sizeof(fd_req);
	fd_req.qid   = self->qid;
	fd_req.flags = O_CLOEXEC;

	self->io_fd = ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_QPAIR_GET_FD, &fd_req);
	ASSERT_GE(self->io_fd, 0);

	/* Allocate page-aligned DMA buffers */
	write_buf = aligned_alloc(4096, TRANSFER_SIZE);
	ASSERT_NE(NULL, write_buf);
	read_buf = aligned_alloc(4096, TRANSFER_SIZE);
	ASSERT_NE(NULL, read_buf);

	fill_pattern(write_buf, TRANSFER_SIZE);
	memset(read_buf, 0, TRANSFER_SIZE);

	/* Write (H2C) */
	ret = pwrite(self->io_fd, write_buf, TRANSFER_SIZE, (off_t)dma_addr);
	ASSERT_EQ(TRANSFER_SIZE, ret);

	/* Read (C2H) */
	ret = pread(self->io_fd, read_buf, TRANSFER_SIZE, (off_t)dma_addr);
	ASSERT_EQ(TRANSFER_SIZE, ret);

	/* Verify */
	EXPECT_EQ(0, memcmp(write_buf, read_buf, TRANSFER_SIZE));

	free(write_buf);
	free(read_buf);
}

TEST_HARNESS_MAIN
