// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Control device (/dev/slash_ctl<N>) ABI tests.
 *
 * Covers GET_DEVICE_INFO, GET_BAR_INFO, GET_BAR_FD, and the BAR dma-buf
 * mmap path.  See docs/reference/kernel-abi/index.rst for the spec.
 */

#include "kselftest_harness.h"
#include "slash_test_helpers.h"

#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>

/* ---------- fixture ---------- */

FIXTURE(ctldev)
{
	int ctl_fd;
};

FIXTURE_SETUP(ctldev)
{
	self->ctl_fd = -1;

	if (access(SLASH_TEST_CTL_DEV, F_OK) != 0)
		SKIP(return, "control device not found (%s)", SLASH_TEST_CTL_DEV);

	self->ctl_fd = open(SLASH_TEST_CTL_DEV, O_RDWR);
	ASSERT_GE(self->ctl_fd, 0)
	TH_LOG("open(%s) failed: %s",
		   SLASH_TEST_CTL_DEV, strerror(errno));
}

FIXTURE_TEARDOWN(ctldev)
{
	if (self->ctl_fd >= 0)
		close(self->ctl_fd);
}

/* ---------- tests ---------- */

TEST_F(ctldev, get_device_info_happy)
{
	struct slash_ioctl_device_info info;

	ASSERT_EQ(0, slash_get_device_info(self->ctl_fd, &info));

	EXPECT_NE('\0', info.bdf[0]);
	EXPECT_TRUE(slash_looks_like_bdf(info.bdf))
	TH_LOG("bad BDF '%s'", info.bdf);
	EXPECT_EQ(SLASH_TEST_VENDOR_ID, info.vendor_id);
	EXPECT_TRUE(info.device_id == SLASH_TEST_PF2_DEV_ID ||
		    info.device_id == SLASH_TEST_PF2_DEV_ID_LEGACY)
	TH_LOG("unexpected PF2 device ID 0x%04x", info.device_id);
	/* The function digit of the BDF must be '2' (PF2). */
	EXPECT_EQ('2', info.bdf[11]);
}

TEST_F(ctldev, device_node_identity_matches_sysfs_and_fd)
{
	struct slash_ioctl_device_info info;
	struct slash_test_node_identity identity;
	char sysfs_name[64];
	struct stat fd_stat;

	ASSERT_EQ(0, slash_get_device_info(self->ctl_fd, &info));

	snprintf(sysfs_name, sizeof(sysfs_name),
			 SLASH_TEST_CTL_SYSFS_PREFIX "%s", info.bdf);
	ASSERT_EQ(0, slash_test_read_node_identity(sysfs_name, &identity))
	TH_LOG("sysfs dev/uevent or /dev node disagree for %s/%s",
		   SLASH_TEST_SYSFS_CLASS_DIR, sysfs_name);
	EXPECT_TRUE(slash_test_validate_ctl_node(&identity))
	TH_LOG("control node %s has invalid minor/name (%u:%u)",
		   identity.devpath, identity.major, identity.minor);

	ASSERT_EQ(0, fstat(self->ctl_fd, &fd_stat));
	EXPECT_EQ(identity.dev, fd_stat.st_rdev)
	TH_LOG("opened fd %s does not match %s/%s",
		   SLASH_TEST_CTL_DEV, SLASH_TEST_SYSFS_CLASS_DIR, sysfs_name);
}

TEST_F(ctldev, bar_info_all_indices_succeed)
{
	int i;

	for (i = 0; i < 6; i++)
	{
		struct slash_ioctl_bar_info info;

		memset(&info, 0, sizeof(info));
		info.size = sizeof(info);
		info.bar_number = i;
		EXPECT_EQ(0, ioctl(self->ctl_fd,
						   SLASH_CTLDEV_IOCTL_GET_BAR_INFO, &info))
		TH_LOG("BAR%d info ioctl failed: %s", i, strerror(errno));
	}
}

TEST_F(ctldev, bar_info_at_least_one_usable)
{
	uint8_t bar = 0;
	uint64_t len = 0;

	ASSERT_EQ(0, slash_find_first_mmio_bar(self->ctl_fd, &bar, &len));
	EXPECT_GT(len, 0);
}

TEST_F(ctldev, bar_info_invalid_bar_number)
{
	struct slash_ioctl_bar_info info;

	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	info.bar_number = 6;
	EXPECT_EQ(-1, ioctl(self->ctl_fd,
						SLASH_CTLDEV_IOCTL_GET_BAR_INFO, &info));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(ctldev, bar_fd_invalid_bar_number)
{
	struct slash_ioctl_bar_fd_request req;

	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	req.bar_number = 6;
	req.flags = O_CLOEXEC;
	EXPECT_EQ(-1, ioctl(self->ctl_fd,
						SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(ctldev, bar_fd_invalid_flags)
{
	struct slash_ioctl_bar_fd_request req;
	uint8_t bar = 0;
	uint64_t len = 0;

	ASSERT_EQ(0, slash_find_first_mmio_bar(self->ctl_fd, &bar, &len));

	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	req.bar_number = bar;
	req.flags = O_NONBLOCK; /* only O_CLOEXEC is honoured */
	EXPECT_EQ(-1, ioctl(self->ctl_fd,
						SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(ctldev, bar_fd_unusable_bar_returns_enodev)
{
	struct slash_ioctl_bar_fd_request req;
	uint8_t bar = 0;
	int rc;

	rc = slash_find_unusable_bar(self->ctl_fd, &bar);
	if (rc == -ENOENT)
		SKIP(return, "all six BARs are usable on this card");
	ASSERT_EQ(0, rc);

	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	req.bar_number = bar;
	req.flags = O_CLOEXEC;
	EXPECT_EQ(-1, ioctl(self->ctl_fd,
						SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req));
	EXPECT_EQ(ENODEV, errno);
}

TEST_F(ctldev, bar_fd_mmap_read_round_trip)
{
	struct slash_ioctl_bar_fd_request req;
	uint8_t bar = 0;
	uint64_t len = 0;
	int bar_fd;
	volatile uint32_t *mmio;
	uint32_t v0, v1;
	size_t map_len;

	ASSERT_EQ(0, slash_find_first_mmio_bar(self->ctl_fd, &bar, &len));

	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	req.bar_number = bar;
	req.flags = O_CLOEXEC;
	bar_fd = ioctl(self->ctl_fd, SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req);
	ASSERT_GE(bar_fd, 0);
	ASSERT_EQ(len, req.length);

	/*
	 * Only map the first page — we don't need more for a read-only
	 * probe and shorter maps reduce the chance of mapping registers
	 * with side-effects on read.
	 */
	map_len = 4096;
	if (map_len > len)
		map_len = len;

	mmio = mmap(NULL, map_len, PROT_READ, MAP_SHARED, bar_fd, 0);
	ASSERT_NE(MAP_FAILED, mmio)
	TH_LOG("mmap failed: %s", strerror(errno));

	ASSERT_EQ(0, slash_dma_buf_sync(bar_fd,
									DMA_BUF_SYNC_START | DMA_BUF_SYNC_READ));
	v0 = mmio[0];
	v1 = mmio[0]; /* idempotency: same offset twice */
	ASSERT_EQ(0, slash_dma_buf_sync(bar_fd,
									DMA_BUF_SYNC_END | DMA_BUF_SYNC_READ));

	EXPECT_EQ(v0, v1);
	/* If the device has been removed from under us, MMIO returns ~0. */
	EXPECT_NE(0xFFFFFFFFu, v0)
	TH_LOG("BAR%d first word reads as 0xFFFFFFFF — device gone?", bar);

	EXPECT_EQ(0, munmap((void *)mmio, map_len));
	EXPECT_EQ(0, close(bar_fd));
}

TEST_F(ctldev, bar_fd_close_releases_dmabuf)
{
	struct slash_ioctl_bar_fd_request req;
	uint8_t bar = 0;
	uint64_t len = 0;
	int fd_a, fd_b;

	ASSERT_EQ(0, slash_find_first_mmio_bar(self->ctl_fd, &bar, &len));

	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	req.bar_number = bar;
	req.flags = O_CLOEXEC;
	fd_a = ioctl(self->ctl_fd, SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req);
	ASSERT_GE(fd_a, 0);
	EXPECT_EQ(0, close(fd_a));

	fd_b = ioctl(self->ctl_fd, SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req);
	ASSERT_GE(fd_b, 0);
	EXPECT_EQ(0, close(fd_b));
}

TEST_F(ctldev, unknown_ioctl_returns_enotty)
{
	/* Pick a sequence number outside the 0x30..0x32 range used by ctldev. */
	unsigned int junk = _IO('v', 0xFE);

	EXPECT_EQ(-1, ioctl(self->ctl_fd, junk, 0));
	EXPECT_EQ(ENOTTY, errno);
}

/* ---------- ABI size-versioning tests ----------
 *
 * GET_BAR_INFO and GET_BAR_FD enforce two size gates: an input minimum
 * (must cover the trailing input field) and a response minimum (must
 * cover the trailing output field). Both report -EINVAL.
 *
 * GET_DEVICE_INFO is pure output: any size is accepted, including 0;
 * output is truncated to min(size, sizeof(struct)).
 *
 * All three handlers zero-fill the trailing bytes when user_size >
 * sizeof(struct) via clear_user().
 */

TEST_F(ctldev, get_bar_info_size_zero_returns_einval)
{
	struct slash_ioctl_bar_info info;

	memset(&info, 0, sizeof(info));
	info.size = 0;
	info.bar_number = 0;
	EXPECT_EQ(-1, ioctl(self->ctl_fd,
						SLASH_CTLDEV_IOCTL_GET_BAR_INFO, &info));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(ctldev, get_bar_info_size_below_input_min_returns_einval)
{
	struct slash_ioctl_bar_info info;

	memset(&info, 0, sizeof(info));
	/* IN_MIN covers bar_number; sizeof(__u32) (== just the size field) is
	 * smaller than offsetof(bar_number)+sizeof(bar_number). */
	info.size = sizeof(__u32);
	EXPECT_EQ(-1, ioctl(self->ctl_fd,
						SLASH_CTLDEV_IOCTL_GET_BAR_INFO, &info));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(ctldev, get_bar_info_size_between_input_min_and_response_min_returns_einval)
{
	struct slash_ioctl_bar_info info;

	memset(&info, 0, sizeof(info));
	/* Pick a size that satisfies IN_MIN (covers bar_number) but is below
	 * RESP_MIN (must cover length). offsetof(length) is strictly between
	 * the two gates. */
	info.size = offsetof(struct slash_ioctl_bar_info, length);
	info.bar_number = 0;
	EXPECT_EQ(-1, ioctl(self->ctl_fd,
						SLASH_CTLDEV_IOCTL_GET_BAR_INFO, &info));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(ctldev, get_bar_info_oversized_struct_zeros_tail)
{
	struct slash_ioctl_bar_info info;
	uint8_t bar = 0;
	uint64_t len = 0;
	void *buf;
	const size_t tail = 64;

	ASSERT_EQ(0, slash_find_first_mmio_bar(self->ctl_fd, &bar, &len));

	memset(&info, 0, sizeof(info));
	info.bar_number = bar;
	buf = slash_alloc_oversized(&info, sizeof(info), tail);
	ASSERT_NE(NULL, buf);

	EXPECT_EQ(0, ioctl(self->ctl_fd,
					   SLASH_CTLDEV_IOCTL_GET_BAR_INFO, buf));
	EXPECT_EQ(1, slash_tail_is_zero(buf, sizeof(info), tail))
	TH_LOG("kernel did not zero-fill the oversized tail");

	free(buf);
}

TEST_F(ctldev, get_bar_fd_size_zero_returns_einval)
{
	struct slash_ioctl_bar_fd_request req;

	memset(&req, 0, sizeof(req));
	req.size = 0;
	EXPECT_EQ(-1, ioctl(self->ctl_fd,
						SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(ctldev, get_bar_fd_size_below_input_min_returns_einval)
{
	struct slash_ioctl_bar_fd_request req;

	memset(&req, 0, sizeof(req));
	req.size = sizeof(__u32);
	EXPECT_EQ(-1, ioctl(self->ctl_fd,
						SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(ctldev, get_bar_fd_size_between_input_min_and_response_min_returns_einval)
{
	struct slash_ioctl_bar_fd_request req;
	uint8_t bar = 0;
	uint64_t len = 0;

	ASSERT_EQ(0, slash_find_first_mmio_bar(self->ctl_fd, &bar, &len));

	memset(&req, 0, sizeof(req));
	/* Below the offset of `length` — covers `flags` but not the output. */
	req.size = offsetof(struct slash_ioctl_bar_fd_request, length);
	req.bar_number = bar;
	req.flags = O_CLOEXEC;
	EXPECT_EQ(-1, ioctl(self->ctl_fd,
						SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(ctldev, get_bar_fd_oversized_struct_zeros_tail)
{
	struct slash_ioctl_bar_fd_request req;
	uint8_t bar = 0;
	uint64_t len = 0;
	void *buf;
	int fd;
	const size_t tail = 64;

	ASSERT_EQ(0, slash_find_first_mmio_bar(self->ctl_fd, &bar, &len));

	memset(&req, 0, sizeof(req));
	req.bar_number = bar;
	req.flags = O_CLOEXEC;
	buf = slash_alloc_oversized(&req, sizeof(req), tail);
	ASSERT_NE(NULL, buf);

	fd = ioctl(self->ctl_fd, SLASH_CTLDEV_IOCTL_GET_BAR_FD, buf);
	EXPECT_GE(fd, 0);
	EXPECT_EQ(1, slash_tail_is_zero(buf, sizeof(req), tail))
	TH_LOG("kernel did not zero-fill the oversized tail");

	if (fd >= 0)
		close(fd);
	free(buf);
}

TEST_F(ctldev, get_device_info_size_zero_returns_einval)
{
	struct slash_ioctl_device_info info;

	memset(&info, 0, sizeof(info));
	info.size = 0;
	EXPECT_EQ(-1, ioctl(self->ctl_fd,
						SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO, &info));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(ctldev, get_device_info_undersized_truncates_output)
{
	struct slash_ioctl_device_info info;
	uint16_t canary_u16 = ((uint16_t)SLASH_TEST_CANARY << 8) | SLASH_TEST_CANARY;

	memset(&info, SLASH_TEST_CANARY, sizeof(info));
	/* Size that covers the size + bdf fields but stops before vendor_id. */
	info.size = offsetof(struct slash_ioctl_device_info, vendor_id);
	EXPECT_EQ(0, ioctl(self->ctl_fd,
					   SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO, &info));

	/* bdf is inside the user-claimed window — the kernel must populate it. */
	EXPECT_TRUE(slash_looks_like_bdf(info.bdf))
	TH_LOG("bdf was not populated within the user-claimed window");

	/* vendor_id and the trailing IDs are beyond user_size — the kernel
	 * must NOT touch them (no copy_to_user, no clear_user). */
	EXPECT_EQ(canary_u16, info.vendor_id);
	EXPECT_EQ(canary_u16, info.device_id);
	EXPECT_EQ(canary_u16, info.subsystem_vendor_id);
	EXPECT_EQ(canary_u16, info.subsystem_device_id);
}

TEST_F(ctldev, get_device_info_oversized_struct_zeros_tail)
{
	struct slash_ioctl_device_info info;
	void *buf;
	const size_t tail = 64;

	memset(&info, 0, sizeof(info));
	buf = slash_alloc_oversized(&info, sizeof(info), tail);
	ASSERT_NE(NULL, buf);

	EXPECT_EQ(0, ioctl(self->ctl_fd,
					   SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO, buf));
	EXPECT_EQ(1, slash_tail_is_zero(buf, sizeof(info), tail))
	TH_LOG("kernel did not zero-fill the oversized tail");

	free(buf);
}

TEST_HARNESS_MAIN
