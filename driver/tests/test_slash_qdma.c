// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * QDMA control device (/dev/slash_qdma_ctl<N>) ABI tests.
 *
 * Covers QPAIR_ADD / Q_OP / QPAIR_GET_FD / INFO, the kernel-owned buffer fd
 * (BUF_CREATE + mmap), and the per-qpair anon-inode transfer fd
 * (TRANSFER ioctl, multi-fd, wrong-direction, read/write/lseek/mmap
 * unsupported, HBM/DDR region round trips).  See
 * docs/reference/kernel-abi/index.rst for the spec.
 */

#include "kselftest_harness.h"
#include "slash_test_helpers.h"

#include <stdio.h>
#include <sys/mman.h>

#define TRANSFER_SIZE 4096

/* ---------- helpers ---------- */

static uint64_t get_dma_addr(void)
{
	const char *val = getenv("SLASH_TEST_DMA_ADDR");

	if (val)
		return strtoull(val, NULL, 0);
	return 0;
}

static void fill_pattern(uint8_t *buf, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++)
		buf[i] = (uint8_t)(i & 0xff);
}

/*
 * Create a kernel-owned DMA buffer via BUF_CREATE on @ioctl_fd (control fd or
 * queue-pair fd).  Returns the new buffer fd (>= 0), or -errno on failure.
 */
static int qdma_buf_create(int ioctl_fd, uint64_t length, uint32_t *granule,
			   uint32_t *transfer_hint)
{
	struct slash_qdma_buf_create req;
	int fd;

	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	req.flags = O_CLOEXEC;
	req.length = length;

	fd = ioctl(ioctl_fd, SLASH_QDMA_IOCTL_BUF_CREATE, &req);
	if (fd < 0)
		return -errno;

	if (granule)
		*granule = req.granule;
	if (transfer_hint)
		*transfer_hint = req.transfer_hint;
	return fd;
}

/* mmap a buffer fd for CPU access; returns the mapping or MAP_FAILED. */
static void *qdma_buf_map(int buf_fd, uint64_t length)
{
	return mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, buf_fd, 0);
}

/*
 * Issue a single-sub-transfer buffer transfer on a qpair fd (qpair_index 0);
 * returns the ioctl result (bytes transferred or -1 with errno set).
 */
static long qdma_buf_transfer(int io_fd, int buf_fd, uint64_t buf_offset,
			      uint64_t dev_addr, uint64_t length,
			      uint32_t direction)
{
	struct slash_qdma_transfer req;

	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	req.count = 1;
	req.xfers[0].qpair_index = 0;
	req.xfers[0].direction = direction;
	req.xfers[0].buf_fd = buf_fd;
	req.xfers[0].buf_offset = buf_offset;
	req.xfers[0].dev_addr = dev_addr;
	req.xfers[0].length = length;

	return ioctl(io_fd, SLASH_QDMA_QPAIR_IOCTL_TRANSFER, &req);
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
	self->io_fd = -1;
	self->qpair_added = 0;
	self->qpair_started = 0;

	if (access(SLASH_TEST_QDMA_DEV, F_OK) != 0)
		SKIP(return, "QDMA device not found (%s)", SLASH_TEST_QDMA_DEV);

	self->ctl_fd = open(SLASH_TEST_QDMA_DEV, O_RDWR);
	ASSERT_GE(self->ctl_fd, 0);
}

FIXTURE_TEARDOWN(qdma)
{
	if (self->io_fd >= 0)
		close(self->io_fd);

	if (self->qpair_started)
		slash_qpair_op(self->ctl_fd, self->qid, SLASH_QDMA_QUEUE_OP_STOP);

	if (self->qpair_added)
		slash_qpair_op(self->ctl_fd, self->qid, SLASH_QDMA_QUEUE_OP_DEL);

	if (self->ctl_fd >= 0)
		close(self->ctl_fd);
}

/* Bring up a default MM qpair (H2C | C2H) and an I/O fd on the fixture. */
static void bring_up_qpair(struct __test_metadata *_metadata,
						   FIXTURE_DATA(qdma) * self, uint32_t dir_mask)
{
	ASSERT_EQ(0, slash_qpair_add(self->ctl_fd, 0 /* MM */, dir_mask,
								 &self->qid));
	self->qpair_added = 1;

	ASSERT_EQ(0, slash_qpair_op(self->ctl_fd, self->qid,
								SLASH_QDMA_QUEUE_OP_START));
	self->qpair_started = 1;

	self->io_fd = slash_qpair_get_fd(self->ctl_fd, self->qid, O_CLOEXEC);
	ASSERT_GE(self->io_fd, 0);
}

/* ---------- happy-path tests ---------- */

TEST_F(qdma, query_info)
{
	struct slash_qdma_info info;

	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	EXPECT_GE(ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_INFO, &info), 0);
}

TEST_F(qdma, qpair_lifecycle)
{
	// Direction 0b11 -> host-to-card and card-to-host
	ASSERT_EQ(0, slash_qpair_add(self->ctl_fd, 0, 0b11, &self->qid));
	self->qpair_added = 1;

	ASSERT_EQ(0, slash_qpair_op(self->ctl_fd, self->qid,
								SLASH_QDMA_QUEUE_OP_START));
	self->qpair_started = 1;

	self->io_fd = slash_qpair_get_fd(self->ctl_fd, self->qid, O_CLOEXEC);
	ASSERT_GE(self->io_fd, 0);

	ASSERT_EQ(0, slash_qpair_op(self->ctl_fd, self->qid,
								SLASH_QDMA_QUEUE_OP_STOP));
	self->qpair_started = 0;

	ASSERT_EQ(0, slash_qpair_op(self->ctl_fd, self->qid,
								SLASH_QDMA_QUEUE_OP_DEL));
	self->qpair_added = 0;
}

TEST_F(qdma, write_read_verify)
{
	uint64_t dma_addr = get_dma_addr();
	int write_fd, read_fd;
	uint8_t *write_buf, *read_buf;
	long ret;

	bring_up_qpair(_metadata, self, 0x3);

	write_fd = qdma_buf_create(self->ctl_fd, TRANSFER_SIZE, NULL, NULL);
	ASSERT_GE(write_fd, 0);
	read_fd = qdma_buf_create(self->ctl_fd, TRANSFER_SIZE, NULL, NULL);
	ASSERT_GE(read_fd, 0);

	write_buf = qdma_buf_map(write_fd, TRANSFER_SIZE);
	ASSERT_NE(MAP_FAILED, write_buf);
	read_buf = qdma_buf_map(read_fd, TRANSFER_SIZE);
	ASSERT_NE(MAP_FAILED, read_buf);

	fill_pattern(write_buf, TRANSFER_SIZE);
	memset(read_buf, 0, TRANSFER_SIZE);

	ret = qdma_buf_transfer(self->io_fd, write_fd, 0, dma_addr,
				TRANSFER_SIZE, SLASH_QDMA_XFER_H2C);
	ASSERT_EQ(TRANSFER_SIZE, ret);

	ret = qdma_buf_transfer(self->io_fd, read_fd, 0, dma_addr,
				TRANSFER_SIZE, SLASH_QDMA_XFER_C2H);
	ASSERT_EQ(TRANSFER_SIZE, ret);

	EXPECT_EQ(0, memcmp(write_buf, read_buf, TRANSFER_SIZE));

	munmap(write_buf, TRANSFER_SIZE);
	munmap(read_buf, TRANSFER_SIZE);
	close(write_fd);
	close(read_fd);
}

/* ---------- buffer fd behaviour ---------- */

TEST_F(qdma, buf_create_zero_length_returns_einval)
{
	EXPECT_EQ(-EINVAL, qdma_buf_create(self->ctl_fd, 0, NULL, NULL));
}

TEST_F(qdma, buf_create_unaligned_length_returns_einval)
{
	/* Length must be a multiple of the page size. */
	EXPECT_EQ(-EINVAL,
		  qdma_buf_create(self->ctl_fd, TRANSFER_SIZE + 1, NULL, NULL));
}

TEST_F(qdma, buf_create_reports_granule_and_hint)
{
	uint32_t granule = 0;
	uint32_t hint = 0;
	int buf_fd;

	buf_fd = qdma_buf_create(self->ctl_fd, TRANSFER_SIZE, &granule, &hint);
	ASSERT_GE(buf_fd, 0);
	EXPECT_EQ(4096u, granule);
	EXPECT_EQ(SLASH_QDMA_TRANSFER_HINT_V80, hint);
	close(buf_fd);
}

TEST_F(qdma, buf_create_via_qpair_fd)
{
	int buf_fd;
	uint8_t *map;
	long ret;
	uint64_t dma_addr = get_dma_addr();

	bring_up_qpair(_metadata, self, 0x3);

	/* Buffers can be created through the queue-pair fd too (SCM_RIGHTS use). */
	buf_fd = qdma_buf_create(self->io_fd, TRANSFER_SIZE, NULL, NULL);
	ASSERT_GE(buf_fd, 0);

	map = qdma_buf_map(buf_fd, TRANSFER_SIZE);
	ASSERT_NE(MAP_FAILED, map);
	fill_pattern(map, TRANSFER_SIZE);

	ret = qdma_buf_transfer(self->io_fd, buf_fd, 0, dma_addr, TRANSFER_SIZE,
				SLASH_QDMA_XFER_H2C);
	ASSERT_EQ(TRANSFER_SIZE, ret);

	munmap(map, TRANSFER_SIZE);
	close(buf_fd);
}

TEST_F(qdma, buf_fd_mapping_outlives_fd_close)
{
	int buf_fd;
	uint8_t *map;
	uint64_t dma_addr = get_dma_addr();
	long ret;

	bring_up_qpair(_metadata, self, 0x3);

	buf_fd = qdma_buf_create(self->io_fd, TRANSFER_SIZE, NULL, NULL);
	ASSERT_GE(buf_fd, 0);
	map = qdma_buf_map(buf_fd, TRANSFER_SIZE);
	ASSERT_NE(MAP_FAILED, map);

	/* Closing the fd must not invalidate an existing mapping. */
	close(buf_fd);

	fill_pattern(map, TRANSFER_SIZE);
	/* The mapping is still valid; the bytes are readable. */
	EXPECT_EQ(0u, map[0]);
	(void)dma_addr;
	(void)ret;

	munmap(map, TRANSFER_SIZE);
}

/* ---------- error paths ---------- */

TEST_F(qdma, qpair_add_invalid_dir_mask_zero)
{
	uint32_t qid;

	EXPECT_EQ(-EINVAL, slash_qpair_add(self->ctl_fd, 0, 0x0, &qid));
}

TEST_F(qdma, qpair_add_invalid_dir_mask_cmpt)
{
	uint32_t qid;

	EXPECT_EQ(-EOPNOTSUPP, slash_qpair_add(self->ctl_fd, 0, 0x4, &qid));
}

TEST_F(qdma, qpair_add_invalid_dir_mask_high_bits)
{
	uint32_t qid;

	EXPECT_EQ(-EINVAL, slash_qpair_add(self->ctl_fd, 0, 0x8, &qid));
}

TEST_F(qdma, qpair_add_invalid_mode_st)
{
	uint32_t qid;

	EXPECT_EQ(-EOPNOTSUPP, slash_qpair_add(self->ctl_fd, 1 /* ST */,
										   0x3, &qid));
}

TEST_F(qdma, qpair_add_invalid_mode_other)
{
	uint32_t qid;

	EXPECT_EQ(-EINVAL, slash_qpair_add(self->ctl_fd, 99, 0x3, &qid));
}

TEST_F(qdma, qpair_add_h2c_ring_size_out_of_range)
{
	struct slash_qdma_qpair_add add;

	memset(&add, 0, sizeof(add));
	add.size = sizeof(add);
	add.mode = 0;
	add.dir_mask = 0x3;
	add.h2c_ring_sz = 16; /* valid range: 0..15 */
	EXPECT_EQ(-1, ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_QPAIR_ADD, &add));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(qdma, qpair_add_c2h_ring_size_out_of_range)
{
	struct slash_qdma_qpair_add add;

	memset(&add, 0, sizeof(add));
	add.size = sizeof(add);
	add.mode = 0;
	add.dir_mask = 0x3;
	add.c2h_ring_sz = 16;
	EXPECT_EQ(-1, ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_QPAIR_ADD, &add));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(qdma, qpair_add_cmpt_ring_size_out_of_range)
{
	struct slash_qdma_qpair_add add;

	memset(&add, 0, sizeof(add));
	add.size = sizeof(add);
	add.mode = 0;
	add.dir_mask = 0x3;
	add.cmpt_ring_sz = 16;
	EXPECT_EQ(-1, ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_QPAIR_ADD, &add));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(qdma, q_op_invalid_op)
{
	ASSERT_EQ(0, slash_qpair_add(self->ctl_fd, 0, 0x3, &self->qid));
	self->qpair_added = 1;

	EXPECT_EQ(-EINVAL, slash_qpair_op(self->ctl_fd, self->qid, 99));
}

TEST_F(qdma, q_op_unknown_qid)
{
	EXPECT_EQ(-ENOENT, slash_qpair_op(self->ctl_fd, 0xDEADBEEF,
									  SLASH_QDMA_QUEUE_OP_START));
}

TEST_F(qdma, qpair_get_fd_invalid_flags)
{
	struct slash_qdma_qpair_fd_request req;

	ASSERT_EQ(0, slash_qpair_add(self->ctl_fd, 0, 0x3, &self->qid));
	self->qpair_added = 1;
	ASSERT_EQ(0, slash_qpair_op(self->ctl_fd, self->qid,
								SLASH_QDMA_QUEUE_OP_START));
	self->qpair_started = 1;

	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	req.qid = self->qid;
	req.flags = O_NONBLOCK; /* only O_CLOEXEC is honoured */
	EXPECT_EQ(-1, ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_QPAIR_GET_FD, &req));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(qdma, qpair_get_fd_unknown_qid)
{
	EXPECT_EQ(-ENOENT, slash_qpair_get_fd(self->ctl_fd, 0xDEADBEEF,
										  O_CLOEXEC));
}

/* ---------- I/O fd behaviour ---------- */

TEST_F(qdma, io_read_on_h2c_only_returns_enodev)
{
	int buf_fd;
	long ret;

	bring_up_qpair(_metadata, self, 0x1); /* H2C only */

	buf_fd = qdma_buf_create(self->io_fd, TRANSFER_SIZE, NULL, NULL);
	ASSERT_GE(buf_fd, 0);

	ret = qdma_buf_transfer(self->io_fd, buf_fd, 0, SLASH_TEST_HBM_BASE,
				TRANSFER_SIZE, SLASH_QDMA_XFER_C2H);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(ENODEV, errno);

	close(buf_fd);
}

TEST_F(qdma, io_write_on_c2h_only_returns_enodev)
{
	int buf_fd;
	long ret;

	bring_up_qpair(_metadata, self, 0x2); /* C2H only */

	buf_fd = qdma_buf_create(self->io_fd, TRANSFER_SIZE, NULL, NULL);
	ASSERT_GE(buf_fd, 0);

	ret = qdma_buf_transfer(self->io_fd, buf_fd, 0, SLASH_TEST_HBM_BASE,
				TRANSFER_SIZE, SLASH_QDMA_XFER_H2C);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(ENODEV, errno);

	close(buf_fd);
}

TEST_F(qdma, io_zero_length_returns_einval)
{
	int buf_fd;
	long ret;

	bring_up_qpair(_metadata, self, 0x3);

	buf_fd = qdma_buf_create(self->io_fd, TRANSFER_SIZE, NULL, NULL);
	ASSERT_GE(buf_fd, 0);

	ret = qdma_buf_transfer(self->io_fd, buf_fd, 0, SLASH_TEST_HBM_BASE,
				0, SLASH_QDMA_XFER_H2C);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	close(buf_fd);
}

TEST_F(qdma, io_mmap_unsupported)
{
	void *p;

	bring_up_qpair(_metadata, self, 0x3);

	/* The transfer (queue-pair) fd is not mappable — only buffer fds are. */
	p = mmap(NULL, 4096, PROT_READ, MAP_SHARED, self->io_fd, 0);
	EXPECT_EQ(MAP_FAILED, p);
	if (p != MAP_FAILED)
		munmap(p, 4096);
}

TEST_F(qdma, io_junk_ioctl_returns_enotty)
{
	/* The per-qpair fd defines only BUF_CREATE / TRANSFER; any other cmd
	 * returns -ENOTTY. */
	unsigned int junk = _IO('v', 0xFE);

	bring_up_qpair(_metadata, self, 0x3);

	EXPECT_EQ(-1, ioctl(self->io_fd, junk, 0));
	EXPECT_EQ(ENOTTY, errno);
}

TEST_F(qdma, io_lseek_unsupported)
{
	off_t pos;

	bring_up_qpair(_metadata, self, 0x3);

	pos = lseek(self->io_fd, (off_t)SLASH_TEST_HBM_BASE, SEEK_SET);
	EXPECT_EQ((off_t)-1, pos);
	EXPECT_EQ(ESPIPE, errno);
}

TEST_F(qdma, io_read_write_unsupported)
{
	uint8_t buf[TRANSFER_SIZE];
	long ret;

	bring_up_qpair(_metadata, self, 0x3);

	ret = write(self->io_fd, buf, TRANSFER_SIZE);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	ret = read(self->io_fd, buf, TRANSFER_SIZE);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(qdma, io_multiple_fds_same_qpair)
{
	int write_fd, read_fd, io_fd_b;
	uint8_t *write_buf, *read_buf;
	long ret;

	bring_up_qpair(_metadata, self, 0x3);

	io_fd_b = slash_qpair_get_fd(self->ctl_fd, self->qid, O_CLOEXEC);
	ASSERT_GE(io_fd_b, 0);

	write_fd = qdma_buf_create(self->ctl_fd, TRANSFER_SIZE, NULL, NULL);
	ASSERT_GE(write_fd, 0);
	read_fd = qdma_buf_create(self->ctl_fd, TRANSFER_SIZE, NULL, NULL);
	ASSERT_GE(read_fd, 0);

	write_buf = qdma_buf_map(write_fd, TRANSFER_SIZE);
	ASSERT_NE(MAP_FAILED, write_buf);
	read_buf = qdma_buf_map(read_fd, TRANSFER_SIZE);
	ASSERT_NE(MAP_FAILED, read_buf);

	fill_pattern(write_buf, TRANSFER_SIZE);
	memset(read_buf, 0, TRANSFER_SIZE);

	ret = qdma_buf_transfer(self->io_fd, write_fd, 0, SLASH_TEST_HBM_BASE,
				TRANSFER_SIZE, SLASH_QDMA_XFER_H2C);
	ASSERT_EQ(TRANSFER_SIZE, ret);

	ret = qdma_buf_transfer(io_fd_b, read_fd, 0, SLASH_TEST_HBM_BASE,
				TRANSFER_SIZE, SLASH_QDMA_XFER_C2H);
	ASSERT_EQ(TRANSFER_SIZE, ret);

	EXPECT_EQ(0, memcmp(write_buf, read_buf, TRANSFER_SIZE));

	munmap(write_buf, TRANSFER_SIZE);
	munmap(read_buf, TRANSFER_SIZE);
	close(write_fd);
	close(read_fd);
	close(io_fd_b);
}

TEST_F(qdma, io_fd_outlives_qpair_del)
{
	int buf_fd;
	long ret;

	bring_up_qpair(_metadata, self, 0x3);

	buf_fd = qdma_buf_create(self->io_fd, TRANSFER_SIZE, NULL, NULL);
	ASSERT_GE(buf_fd, 0);

	/* DEL the qpair while io_fd is still open. */
	ASSERT_EQ(0, slash_qpair_op(self->ctl_fd, self->qid,
								SLASH_QDMA_QUEUE_OP_DEL));
	self->qpair_added = 0;
	self->qpair_started = 0;

	/*
	 * fd is still valid but the qpair's HW queues are gone.  The spec does
	 * not name a specific errno, so we only assert the call fails.
	 */
	ret = qdma_buf_transfer(self->io_fd, buf_fd, 0, SLASH_TEST_HBM_BASE,
				TRANSFER_SIZE, SLASH_QDMA_XFER_H2C);
	EXPECT_EQ(-1, ret);

	close(buf_fd);
	/* close(io_fd) happens in fixture teardown — must not crash. */
}

/* ---------- region round trips ---------- */

static void region_round_trip(struct __test_metadata *_metadata,
							  FIXTURE_DATA(qdma) * self, uint64_t base)
{
	int write_fd, read_fd;
	uint8_t *write_buf, *read_buf;
	long ret;

	bring_up_qpair(_metadata, self, 0x3);

	write_fd = qdma_buf_create(self->ctl_fd, TRANSFER_SIZE, NULL, NULL);
	ASSERT_GE(write_fd, 0);
	read_fd = qdma_buf_create(self->ctl_fd, TRANSFER_SIZE, NULL, NULL);
	ASSERT_GE(read_fd, 0);

	write_buf = qdma_buf_map(write_fd, TRANSFER_SIZE);
	ASSERT_NE(MAP_FAILED, write_buf);
	read_buf = qdma_buf_map(read_fd, TRANSFER_SIZE);
	ASSERT_NE(MAP_FAILED, read_buf);

	fill_pattern(write_buf, TRANSFER_SIZE);
	memset(read_buf, 0, TRANSFER_SIZE);

	ret = qdma_buf_transfer(self->io_fd, write_fd, 0, base,
				TRANSFER_SIZE, SLASH_QDMA_XFER_H2C);
	ASSERT_EQ(TRANSFER_SIZE, ret)
	TH_LOG("H2C transfer to 0x%llx failed: %s",
		   (unsigned long long)base, strerror(errno));

	ret = qdma_buf_transfer(self->io_fd, read_fd, 0, base,
				TRANSFER_SIZE, SLASH_QDMA_XFER_C2H);
	ASSERT_EQ(TRANSFER_SIZE, ret);

	EXPECT_EQ(0, memcmp(write_buf, read_buf, TRANSFER_SIZE));

	munmap(write_buf, TRANSFER_SIZE);
	munmap(read_buf, TRANSFER_SIZE);
	close(write_fd);
	close(read_fd);
}

TEST_F(qdma, transfer_hbm)
{
	region_round_trip(_metadata, self, SLASH_TEST_HBM_BASE);
}

TEST_F(qdma, transfer_ddr)
{
	region_round_trip(_metadata, self, SLASH_TEST_DDR_BASE);
}

/* ---------- ABI size-versioning tests ----------
 *
 * QDMA_INFO is a pure-output ioctl: any user_size is accepted (including
 * 0); output is truncated to min(user_size, sizeof(struct)).
 *
 * QPAIR_ADD, Q_OP, and QPAIR_GET_FD are input-bearing ioctls and reject
 * under-sized user structs with -EINVAL before acting on zero-filled
 * inputs, matching the ABI versioning contract.
 *
 * All four handlers honour the oversized-tail-zero-fill contract.
 */

TEST_F(qdma, info_size_zero_returns_einval)
{
	struct slash_qdma_info info;

	memset(&info, 0, sizeof(info));
	info.size = 0;
	EXPECT_EQ(-1, ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_INFO, &info));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(qdma, info_undersized_truncates_output)
{
	unsigned char buf[sizeof(struct slash_qdma_info)];
	__u32 size_field = sizeof(__u32); /* covers only the size field */
	size_t i;

	memset(buf, SLASH_TEST_CANARY, sizeof(buf));
	memcpy(buf, &size_field, sizeof(size_field));

	EXPECT_EQ(0, ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_INFO, buf));

	/* Kernel may overwrite the size field with sizeof(kernel struct),
	 * but must not touch anything beyond the user-claimed size. */
	for (i = sizeof(__u32); i < sizeof(buf); i++)
		EXPECT_EQ(SLASH_TEST_CANARY, buf[i])
		TH_LOG("byte %zu modified despite user_size=%u",
			   i, (unsigned int)sizeof(__u32));
}

TEST_F(qdma, info_oversized_struct_zeros_tail)
{
	struct slash_qdma_info info;
	void *buf;
	const size_t tail = 64;

	memset(&info, 0, sizeof(info));
	buf = slash_alloc_oversized(&info, sizeof(info), tail);
	ASSERT_NE(NULL, buf);

	EXPECT_EQ(0, ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_INFO, buf));
	EXPECT_EQ(1, slash_tail_is_zero(buf, sizeof(info), tail))
	TH_LOG("kernel did not zero-fill the oversized tail");

	free(buf);
}

TEST_F(qdma, qpair_add_size_below_input_min_returns_einval)
{
	/* The input-size gate (size must cover cmpt_ring_sz, the trailing
	 * input field) rejects under-sized structs with -EINVAL before any
	 * semantic validation runs — ensuring no qpair is ever created from
	 * zero-filled garbage. */
	struct slash_qdma_qpair_add add;

	memset(&add, 0, sizeof(add));
	add.size = sizeof(__u32);
	EXPECT_EQ(-1, ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_QPAIR_ADD, &add));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(qdma, qpair_add_oversized_struct_zeros_tail)
{
	struct slash_qdma_qpair_add add;
	struct slash_qdma_qpair_add *result;
	void *buf;
	const size_t tail = 64;
	int ret;

	memset(&add, 0, sizeof(add));
	add.mode = 0;
	add.dir_mask = 0x3;
	buf = slash_alloc_oversized(&add, sizeof(add), tail);
	ASSERT_NE(NULL, buf);

	ret = ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_QPAIR_ADD, buf);
	EXPECT_EQ(0, ret);
	EXPECT_EQ(1, slash_tail_is_zero(buf, sizeof(add), tail))
	TH_LOG("kernel did not zero-fill the oversized tail");

	if (ret == 0) {
		result = (struct slash_qdma_qpair_add *)buf;
		self->qid = result->qid;
		self->qpair_added = 1;
	}

	free(buf);
}

TEST_F(qdma, q_op_size_below_input_min_returns_einval)
{
	/* The input-size gate (size must cover op, the trailing input field)
	 * rejects under-sized structs with -EINVAL before the qid lookup. */
	struct slash_qdma_qpair_op op;

	memset(&op, 0, sizeof(op));
	op.size = sizeof(__u32);
	EXPECT_EQ(-1, ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_Q_OP, &op));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(qdma, q_op_oversized_struct_zeros_tail)
{
	struct slash_qdma_qpair_op op;
	void *buf;
	const size_t tail = 64;

	ASSERT_EQ(0, slash_qpair_add(self->ctl_fd, 0, 0x3, &self->qid));
	self->qpair_added = 1;

	memset(&op, 0, sizeof(op));
	op.qid = self->qid;
	op.op = SLASH_QDMA_QUEUE_OP_START;
	buf = slash_alloc_oversized(&op, sizeof(op), tail);
	ASSERT_NE(NULL, buf);

	EXPECT_EQ(0, ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_Q_OP, buf));
	EXPECT_EQ(1, slash_tail_is_zero(buf, sizeof(op), tail))
	TH_LOG("kernel did not zero-fill the oversized tail");

	self->qpair_started = 1;
	free(buf);
}

TEST_F(qdma, qpair_get_fd_size_below_input_min_returns_einval)
{
	/* The input-size gate (size must cover flags, the trailing input field)
	 * rejects under-sized structs with -EINVAL before the qid lookup. */
	struct slash_qdma_qpair_fd_request req;

	memset(&req, 0, sizeof(req));
	req.size = sizeof(__u32);
	EXPECT_EQ(-1, ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_QPAIR_GET_FD, &req));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(qdma, qpair_get_fd_oversized_struct_zeros_tail)
{
	struct slash_qdma_qpair_fd_request req;
	void *buf;
	const size_t tail = 64;
	int fd;

	ASSERT_EQ(0, slash_qpair_add(self->ctl_fd, 0, 0x3, &self->qid));
	self->qpair_added = 1;
	ASSERT_EQ(0, slash_qpair_op(self->ctl_fd, self->qid,
								SLASH_QDMA_QUEUE_OP_START));
	self->qpair_started = 1;

	memset(&req, 0, sizeof(req));
	req.qid = self->qid;
	req.flags = O_CLOEXEC;
	buf = slash_alloc_oversized(&req, sizeof(req), tail);
	ASSERT_NE(NULL, buf);

	fd = ioctl(self->ctl_fd, SLASH_QDMA_IOCTL_QPAIR_GET_FD, buf);
	EXPECT_GE(fd, 0);
	EXPECT_EQ(1, slash_tail_is_zero(buf, sizeof(req), tail))
	TH_LOG("kernel did not zero-fill the oversized tail");

	if (fd >= 0)
		close(fd);
	free(buf);
}

TEST_F(qdma, reject_partial_4k_transfer)
{
	int buf_fd;
	uint64_t dma_addr = get_dma_addr();
	long ret;

	bring_up_qpair(_metadata, self, 0x3);

	buf_fd = qdma_buf_create(self->io_fd, TRANSFER_SIZE, NULL, NULL);
	ASSERT_GE(buf_fd, 0);

	/* A sub-page length is not a multiple of the buffer granule. */
	ret = qdma_buf_transfer(self->io_fd, buf_fd, 0, dma_addr,
				TRANSFER_SIZE / 2, SLASH_QDMA_XFER_H2C);
	ASSERT_EQ(-1, ret);
	ASSERT_EQ(EINVAL, errno);

	close(buf_fd);
}

TEST_F(qdma, multipage_4k_write_read_verify)
{
	const size_t xfer_size = TRANSFER_SIZE * 8; /* 8 base pages, one request */
	int write_fd, read_fd;
	uint8_t *write_buf, *read_buf;
	uint64_t dma_addr = get_dma_addr();
	long ret;

	bring_up_qpair(_metadata, self, 0x3);

	write_fd = qdma_buf_create(self->ctl_fd, xfer_size, NULL, NULL);
	ASSERT_GE(write_fd, 0);
	read_fd = qdma_buf_create(self->ctl_fd, xfer_size, NULL, NULL);
	ASSERT_GE(read_fd, 0);

	write_buf = qdma_buf_map(write_fd, xfer_size);
	ASSERT_NE(MAP_FAILED, write_buf);
	read_buf = qdma_buf_map(read_fd, xfer_size);
	ASSERT_NE(MAP_FAILED, read_buf);

	fill_pattern(write_buf, xfer_size);
	memset(read_buf, 0, xfer_size);

	ret = qdma_buf_transfer(self->io_fd, write_fd, 0, dma_addr, xfer_size,
				SLASH_QDMA_XFER_H2C);
	ASSERT_EQ((ssize_t)xfer_size, ret);

	ret = qdma_buf_transfer(self->io_fd, read_fd, 0, dma_addr, xfer_size,
				SLASH_QDMA_XFER_C2H);
	ASSERT_EQ((ssize_t)xfer_size, ret);

	EXPECT_EQ(0, memcmp(write_buf, read_buf, xfer_size));

	munmap(write_buf, xfer_size);
	munmap(read_buf, xfer_size);
	close(write_fd);
	close(read_fd);
}

/* ---------- transfer error paths ---------- */

TEST_F(qdma, transfer_size_below_input_min_returns_einval)
{
	struct slash_qdma_transfer req;

	bring_up_qpair(_metadata, self, 0x3);

	memset(&req, 0, sizeof(req));
	req.size = sizeof(__u32); /* below the trailing input field */
	EXPECT_EQ(-1, ioctl(self->io_fd, SLASH_QDMA_QPAIR_IOCTL_TRANSFER, &req));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(qdma, transfer_invalid_buf_fd_returns_einval)
{
	long ret;

	bring_up_qpair(_metadata, self, 0x3);

	/* The control fd is a valid fd but not a buffer fd. */
	ret = qdma_buf_transfer(self->io_fd, self->ctl_fd, 0,
				get_dma_addr(), TRANSFER_SIZE,
				SLASH_QDMA_XFER_H2C);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(qdma, transfer_bad_fd_returns_ebadf)
{
	long ret;

	bring_up_qpair(_metadata, self, 0x3);

	ret = qdma_buf_transfer(self->io_fd, -1, 0,
				get_dma_addr(), TRANSFER_SIZE,
				SLASH_QDMA_XFER_H2C);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EBADF, errno);
}

TEST_F(qdma, transfer_wrong_direction_returns_enodev)
{
	int buf_fd;
	uint32_t transfer_hint = 0;
	long ret;

	bring_up_qpair(_metadata, self, 0x1); /* H2C only */

	buf_fd = qdma_buf_create(self->io_fd, TRANSFER_SIZE, NULL, &transfer_hint);
	ASSERT_GE(buf_fd, 0);
	EXPECT_EQ(SLASH_QDMA_TRANSFER_HINT_V80, transfer_hint);

	/* C2H is not enabled on this qpair. */
	ret = qdma_buf_transfer(self->io_fd, buf_fd, 0,
				get_dma_addr(), TRANSFER_SIZE,
				SLASH_QDMA_XFER_C2H);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(ENODEV, errno);

	close(buf_fd);
}

TEST_F(qdma, transfer_out_of_range_returns_einval)
{
	int buf_fd;
	long ret;

	bring_up_qpair(_metadata, self, 0x3);

	buf_fd = qdma_buf_create(self->io_fd, TRANSFER_SIZE, NULL, NULL);
	ASSERT_GE(buf_fd, 0);

	/* Slice extends past the buffer length. */
	ret = qdma_buf_transfer(self->io_fd, buf_fd, TRANSFER_SIZE,
				get_dma_addr(), TRANSFER_SIZE,
				SLASH_QDMA_XFER_H2C);
	EXPECT_EQ(-1, ret);
	EXPECT_EQ(EINVAL, errno);

	close(buf_fd);
}

TEST_HARNESS_MAIN
