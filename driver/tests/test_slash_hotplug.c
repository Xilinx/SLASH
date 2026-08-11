// SPDX-License-Identifier: GPL-2.0-only OR MIT
/*
 * Hotplug device (/dev/slash_hotplug) ABI tests.
 *
 * Covers RESCAN, REMOVE, HOTPLUG, and TOGGLE_SBR.  These tests are
 * destructive by definition: they remove and re-add cards from the
 * PCI hierarchy.  Run last in the suite (Makefile orders TESTS
 * accordingly) so the ctldev and qdma tests have already completed
 * against the live device(s).
 *
 * Discovery: the fixture enumerates every SLASH accelerator on the
 * system at setup time by scanning /sys/class/slash/ for slash_ctl_<BDF>
 * (PF2) and slash_qdma_ctl_<BDF> (PF1), then pairs them by the
 * "DDDD:BB:SS" bus prefix.  Operations target the first accelerator;
 * the teardown polls until *every* discovered accelerator has both its
 * ctl and qdma sysfs entries back with the same paths and device numbers
 * recorded before removal.  This catches both
 * cross-card damage (touching the wrong accelerator) and stale /dev
 * state (sysfs back but udev missed the node, or wrong minor).
 *
 * Tests that retain open resources across removal or perform a full board
 * reset are gated by SLASH_TEST_DESTRUCTIVE=1.
 *
 * The accelerator-discovery helpers live in this file rather than in
 * slash_test_helpers.h because no other test binary needs them.
 *
 * See docs/reference/kernel-abi/index.rst.
 */

#include "kselftest_harness.h"
#include "slash_test_helpers.h"

#include <slash/uapi/slash_hotplug.h>

#include <dirent.h>
#include <stdio.h>

#define NODE_RECOVERY_TIMEOUT_S 10
#define SBR_SETTLE_SECONDS 7

/* ====================================================================
 * Accelerator discovery + verification
 * ==================================================================== */

struct accelerator
{
	char pf0_bdf[SLASH_PCI_BDF_LEN]; /* derived: "<bus_prefix>.0" */
	char pf1_bdf[SLASH_PCI_BDF_LEN]; /* from slash_qdma_ctl_<BDF> */
	char pf2_bdf[SLASH_PCI_BDF_LEN]; /* from slash_ctl_<BDF> */
	struct slash_test_node_identity ctl_identity;
	struct slash_test_node_identity qdma_identity;
};

/* Both nodes must reappear with their recorded dev_t and path identity. */
static int verify_accelerator_present(const struct accelerator *a)
{
	struct slash_test_node_identity current;
	int err;

	err = slash_test_read_node_identity(a->ctl_identity.sysfs_name, &current);
	if (err)
		return err;
	if (!slash_test_node_identity_equal(&a->ctl_identity, &current))
		return -ESTALE;

	err = slash_test_read_node_identity(a->qdma_identity.sysfs_name, &current);
	if (err)
		return err;
	if (!slash_test_node_identity_equal(&a->qdma_identity, &current))
		return -ESTALE;
	return 0;
}

/*
 * Discover every SLASH accelerator by scanning /sys/class/slash/.
 *
 * Unpaired entries (PF2 without a matching PF1, or vice versa) indicate
 * a partial probe failure — log a warning and skip the orphan.
 */
static int discover_accelerators(struct accelerator *out, int max, int *n_out)
{
	char pf2_bdfs[SLASH_TEST_MAX_CARDS][SLASH_PCI_BDF_LEN] = {{0}};
	char pf1_bdfs[SLASH_TEST_MAX_CARDS][SLASH_PCI_BDF_LEN] = {{0}};
	int pf2_paired[SLASH_TEST_MAX_CARDS] = {0};
	int pf1_paired[SLASH_TEST_MAX_CARDS] = {0};
	int n_pf2 = 0, n_pf1 = 0;
	int n_accels = 0;
	int i, j;
	DIR *d;
	struct dirent *de;

	d = opendir(SLASH_TEST_SYSFS_CLASS_DIR);
	if (!d)
		return -errno;

	while ((de = readdir(d)) != NULL)
	{
		/* Match QDMA prefix first — neither prefix is a prefix of
		 * the other, but the ordering keeps intent obvious. */
		if (strncmp(de->d_name, SLASH_TEST_QDMA_SYSFS_PREFIX,
					strlen(SLASH_TEST_QDMA_SYSFS_PREFIX)) == 0)
		{
			if (n_pf1 < SLASH_TEST_MAX_CARDS)
			{
				strncpy(pf1_bdfs[n_pf1],
						de->d_name + strlen(SLASH_TEST_QDMA_SYSFS_PREFIX),
						SLASH_PCI_BDF_LEN - 1);
				n_pf1++;
			}
		}
		else if (strncmp(de->d_name, SLASH_TEST_CTL_SYSFS_PREFIX,
						 strlen(SLASH_TEST_CTL_SYSFS_PREFIX)) == 0)
		{
			if (n_pf2 < SLASH_TEST_MAX_CARDS)
			{
				strncpy(pf2_bdfs[n_pf2],
						de->d_name + strlen(SLASH_TEST_CTL_SYSFS_PREFIX),
						SLASH_PCI_BDF_LEN - 1);
				n_pf2++;
			}
		}
	}
	closedir(d);

	for (i = 0; i < n_pf2 && n_accels < max; i++)
	{
		for (j = 0; j < n_pf1; j++)
		{
			if (pf1_paired[j])
				continue;
			if (!slash_same_card(pf2_bdfs[i], pf1_bdfs[j]))
				continue;

			strncpy(out[n_accels].pf2_bdf, pf2_bdfs[i],
					SLASH_PCI_BDF_LEN - 1);
			out[n_accels].pf2_bdf[SLASH_PCI_BDF_LEN - 1] = '\0';
			strncpy(out[n_accels].pf1_bdf, pf1_bdfs[j],
					SLASH_PCI_BDF_LEN - 1);
			out[n_accels].pf1_bdf[SLASH_PCI_BDF_LEN - 1] = '\0';
			strncpy(out[n_accels].pf0_bdf, pf2_bdfs[i],
					SLASH_PCI_BDF_LEN - 1);
			out[n_accels].pf0_bdf[SLASH_PCI_BDF_LEN - 1] = '\0';
			out[n_accels].pf0_bdf[11] = '0';

			{
				char sysfs_name[64];
				int err;

				snprintf(sysfs_name, sizeof(sysfs_name),
						 SLASH_TEST_CTL_SYSFS_PREFIX "%s",
						 out[n_accels].pf2_bdf);
				err = slash_test_read_node_identity(
					sysfs_name, &out[n_accels].ctl_identity);
				if (err)
					return err;

				snprintf(sysfs_name, sizeof(sysfs_name),
						 SLASH_TEST_QDMA_SYSFS_PREFIX "%s",
						 out[n_accels].pf1_bdf);
				err = slash_test_read_node_identity(
					sysfs_name, &out[n_accels].qdma_identity);
				if (err)
					return err;
			}

			pf2_paired[i] = 1;
			pf1_paired[j] = 1;
			n_accels++;
			break;
		}
	}

	for (i = 0; i < n_pf2; i++)
		if (!pf2_paired[i])
			fprintf(stderr,
					"# WARNING: unpaired " SLASH_TEST_CTL_SYSFS_PREFIX
					"%s (no matching " SLASH_TEST_QDMA_SYSFS_PREFIX
					"<%.*s.x>)\n",
					pf2_bdfs[i],
					SLASH_TEST_BUS_PREFIX_LEN, pf2_bdfs[i]);
	for (j = 0; j < n_pf1; j++)
		if (!pf1_paired[j])
			fprintf(stderr,
					"# WARNING: unpaired " SLASH_TEST_QDMA_SYSFS_PREFIX
					"%s (no matching " SLASH_TEST_CTL_SYSFS_PREFIX
					"<%.*s.x>)\n",
					pf1_bdfs[j],
					SLASH_TEST_BUS_PREFIX_LEN, pf1_bdfs[j]);

	*n_out = n_accels;
	return 0;
}

/* Wait until every accelerator verifies cleanly, or timeout. */
static int poll_accelerators_present(const struct accelerator *accels,
									 int n, int timeout_s)
{
	int attempt;
	int last_err = 0;
	int last_failing = -1;

	for (attempt = 0; attempt < timeout_s * 10; attempt++)
	{
		int all_ok = 1;
		int i;

		last_err = 0;
		last_failing = -1;
		for (i = 0; i < n; i++)
		{
			int err = verify_accelerator_present(&accels[i]);

			if (err)
			{
				all_ok = 0;
				last_err = err;
				last_failing = i;
				break;
			}
		}
		if (all_ok)
			return 0;
		usleep(100000);
	}
	if (last_failing >= 0)
		fprintf(stderr,
				"# accelerator %d (PF2=%s, PF1=%s) verify failed: errno %d\n",
				last_failing,
				accels[last_failing].pf2_bdf,
				accels[last_failing].pf1_bdf,
				-last_err);
	return -ETIMEDOUT;
}

/* Wait until /sys/class/slash/<basename> is gone. */
static int poll_slash_absent(const char *sysfs_basename, int timeout_s)
{
	char path[256];
	int i;

	snprintf(path, sizeof(path), SLASH_TEST_SYSFS_CLASS_DIR "/%s",
			 sysfs_basename);
	for (i = 0; i < timeout_s * 10; i++)
	{
		if (access(path, F_OK) != 0)
			return 0;
		usleep(100000);
	}
	return -ETIMEDOUT;
}

/* ====================================================================
 * Fixture
 * ==================================================================== */

FIXTURE(hotplug)
{
	int hp_fd;
	struct slash_test_node_identity hotplug_identity;
	struct accelerator accels[SLASH_TEST_MAX_CARDS];
	int n_accels;
};

FIXTURE_SETUP(hotplug)
{
	self->hp_fd = -1;
	self->n_accels = 0;

	if (access(SLASH_TEST_HOTPLUG_DEV, F_OK) != 0)
		SKIP(return, "hotplug device not found (%s)",
				   SLASH_TEST_HOTPLUG_DEV);

	self->hp_fd = open(SLASH_TEST_HOTPLUG_DEV, O_RDWR);
	ASSERT_GE(self->hp_fd, 0)
	TH_LOG("open(%s) failed: %s",
		   SLASH_TEST_HOTPLUG_DEV, strerror(errno));

	ASSERT_EQ(0, slash_test_read_node_identity(
					 SLASH_TEST_HOTPLUG_SYSFS_NAME,
					 &self->hotplug_identity))
	TH_LOG("hotplug sysfs dev/uevent or /dev node disagree");

	ASSERT_EQ(0, discover_accelerators(self->accels,
									   SLASH_TEST_MAX_CARDS,
									   &self->n_accels));
	if (self->n_accels == 0)
		SKIP(return, "no SLASH accelerators discovered in "
				   SLASH_TEST_SYSFS_CLASS_DIR);

	/* Starting state must be sane: every discovered accelerator's nodes
	 * must already pass verification.  If not, a previous run left the
	 * system in a broken state — fail loud rather than chase symptoms. */
	ASSERT_EQ(0, poll_accelerators_present(self->accels, self->n_accels,
										   NODE_RECOVERY_TIMEOUT_S))
	TH_LOG("initial accelerator state is broken; recover the system "
		   "before re-running the hotplug tests");
}

FIXTURE_TEARDOWN(hotplug)
{
	if (self->hp_fd >= 0)
	{
		/* Best-effort RESCAN to recover the device nodes. */
		ioctl(self->hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN);
		if (poll_accelerators_present(self->accels, self->n_accels,
									  NODE_RECOVERY_TIMEOUT_S) < 0)
			fprintf(stderr,
					"# WARNING: not all accelerators recovered after teardown RESCAN\n");
		close(self->hp_fd);
	}
}

/* ====================================================================
 * Helpers for issuing hotplug ioctls
 * ==================================================================== */

static int hp_ioctl_bdf(int hp_fd, unsigned long cmd, const char *bdf)
{
	struct slash_hotplug_device_request req;

	memset(&req, 0, sizeof(req));
	req.size = sizeof(req);
	if (bdf)
	{
		strncpy(req.bdf, bdf, sizeof(req.bdf) - 1);
		req.bdf[sizeof(req.bdf) - 1] = '\0';
	}
	if (ioctl(hp_fd, cmd, &req) < 0)
		return -errno;
	return 0;
}

/* ====================================================================
 * Tests
 * ==================================================================== */

TEST_F(hotplug, stable_chrdev_layout)
{
	struct stat hp_stat;
	int i;

	ASSERT_TRUE(slash_test_validate_hotplug_node(&self->hotplug_identity))
	TH_LOG("hotplug node has invalid minor/name (%u:%u, %s)",
		   self->hotplug_identity.major, self->hotplug_identity.minor,
		   self->hotplug_identity.devname);
	ASSERT_EQ(0, fstat(self->hp_fd, &hp_stat));
	EXPECT_EQ(self->hotplug_identity.dev, hp_stat.st_rdev);
	EXPECT_GT(self->hotplug_identity.major, 0);

	for (i = 0; i < self->n_accels; i++)
	{
		const struct accelerator *a = &self->accels[i];

		EXPECT_TRUE(slash_test_validate_ctl_node(&a->ctl_identity))
		TH_LOG("invalid control node %s (%u:%u)",
			   a->ctl_identity.devpath,
			   a->ctl_identity.major, a->ctl_identity.minor);
		EXPECT_TRUE(slash_test_validate_qdma_node(&a->qdma_identity))
		TH_LOG("invalid QDMA node %s (%u:%u)",
			   a->qdma_identity.devpath,
			   a->qdma_identity.major, a->qdma_identity.minor);
		EXPECT_EQ(self->hotplug_identity.major, a->ctl_identity.major);
		EXPECT_EQ(self->hotplug_identity.major, a->qdma_identity.major);
		EXPECT_EQ(a->ctl_identity.minor + 1, a->qdma_identity.minor)
		TH_LOG("PF2 %s and PF1 %s do not have paired minors",
			   a->pf2_bdf, a->pf1_bdf);
		EXPECT_TRUE(slash_same_card(a->pf2_bdf, a->pf1_bdf));
	}
}

TEST_F(hotplug, rescan_smoke)
{
	EXPECT_EQ(0, ioctl(self->hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN));

	/* Presence includes unchanged class name, /dev path, and dev_t. */
	EXPECT_EQ(0, poll_accelerators_present(self->accels, self->n_accels,
										   NODE_RECOVERY_TIMEOUT_S));
}

TEST_F(hotplug, unknown_ioctl_returns_enotty)
{
	unsigned int junk = _IO('w', 0xFE);

	EXPECT_EQ(-1, ioctl(self->hp_fd, junk));
	EXPECT_EQ(ENOTTY, errno);
}

TEST_F(hotplug, remove_malformed_bdf)
{
	EXPECT_EQ(-EINVAL,
			  hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE,
						   "not-a-bdf"));
}

TEST_F(hotplug, remove_empty_bdf)
{
	EXPECT_EQ(-EINVAL,
			  hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE, ""));
}

TEST_F(hotplug, remove_unknown_bdf)
{
	EXPECT_EQ(-ENODEV,
			  hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE,
						   "ffff:ff:1f.7"));
}

TEST_F(hotplug, remove_then_rescan_preserves_pf2_identity)
{
	char sysfs_name[64];

	snprintf(sysfs_name, sizeof(sysfs_name),
			 SLASH_TEST_CTL_SYSFS_PREFIX "%s", self->accels[0].pf2_bdf);

	ASSERT_EQ(0, hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE,
							  self->accels[0].pf2_bdf));
	EXPECT_EQ(0, poll_slash_absent(sysfs_name, NODE_RECOVERY_TIMEOUT_S))
	TH_LOG("%s/%s did not disappear after REMOVE",
		   SLASH_TEST_SYSFS_CLASS_DIR, sysfs_name);

	ASSERT_EQ(0, ioctl(self->hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN));
	EXPECT_EQ(0, poll_accelerators_present(self->accels, self->n_accels,
										   NODE_RECOVERY_TIMEOUT_S))
	TH_LOG("node identity changed after PF2 REMOVE + RESCAN");
}

TEST_F(hotplug, remove_then_rescan_preserves_pf1_identity)
{
	char sysfs_name[64];

	snprintf(sysfs_name, sizeof(sysfs_name),
			 SLASH_TEST_QDMA_SYSFS_PREFIX "%s", self->accels[0].pf1_bdf);

	ASSERT_EQ(0, hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE,
							  self->accels[0].pf1_bdf));
	EXPECT_EQ(0, poll_slash_absent(sysfs_name, NODE_RECOVERY_TIMEOUT_S))
	TH_LOG("%s/%s did not disappear after REMOVE",
		   SLASH_TEST_SYSFS_CLASS_DIR, sysfs_name);

	ASSERT_EQ(0, ioctl(self->hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN));
	EXPECT_EQ(0, poll_accelerators_present(self->accels, self->n_accels,
										   NODE_RECOVERY_TIMEOUT_S))
	TH_LOG("node identity changed after PF1 REMOVE + RESCAN");
}

/* ---------- Open-resource lifetime (destructive, env-gated) ---------- */

TEST_F(hotplug, open_pf2_fd_survives_remove_but_stays_offline)
{
	struct slash_ioctl_device_info info;
	int ctl_fd;

	if (getenv("SLASH_TEST_DESTRUCTIVE") == NULL)
		SKIP(return, "remove PF2 while its fd is open; "
					 "set SLASH_TEST_DESTRUCTIVE=1 to run");

	ctl_fd = open(self->accels[0].ctl_identity.devpath, O_RDWR);
	ASSERT_GE(ctl_fd, 0);

	ASSERT_EQ(0, hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE,
							  self->accels[0].pf2_bdf));
	EXPECT_EQ(0, poll_slash_absent(
					 self->accels[0].ctl_identity.sysfs_name,
					 NODE_RECOVERY_TIMEOUT_S));

	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	errno = 0;
	EXPECT_EQ(-1, ioctl(ctl_fd, SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO, &info));
	EXPECT_EQ(ENODEV, errno);

	EXPECT_EQ(0, ioctl(self->hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN));
	EXPECT_EQ(0, poll_accelerators_present(self->accels, self->n_accels,
										   NODE_RECOVERY_TIMEOUT_S));

	errno = 0;
	EXPECT_EQ(-1, ioctl(ctl_fd, SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO, &info));
	EXPECT_EQ(ENODEV, errno)
	TH_LOG("old PF2 fd rebound to the rescanned device");
	EXPECT_EQ(0, close(ctl_fd));
}

TEST_F(hotplug, open_pf1_fd_survives_remove_but_stays_offline)
{
	struct slash_qdma_info info;
	int qdma_fd;

	if (getenv("SLASH_TEST_DESTRUCTIVE") == NULL)
		SKIP(return, "remove PF1 while its fd is open; "
					 "set SLASH_TEST_DESTRUCTIVE=1 to run");

	qdma_fd = open(self->accels[0].qdma_identity.devpath, O_RDWR);
	ASSERT_GE(qdma_fd, 0);

	ASSERT_EQ(0, hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE,
							  self->accels[0].pf1_bdf));
	EXPECT_EQ(0, poll_slash_absent(
					 self->accels[0].qdma_identity.sysfs_name,
					 NODE_RECOVERY_TIMEOUT_S));

	memset(&info, 0, sizeof(info));
	info.size = sizeof(info);
	errno = 0;
	EXPECT_EQ(-1, ioctl(qdma_fd, SLASH_QDMA_IOCTL_INFO, &info));
	EXPECT_EQ(ENODEV, errno);

	EXPECT_EQ(0, ioctl(self->hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN));
	EXPECT_EQ(0, poll_accelerators_present(self->accels, self->n_accels,
										   NODE_RECOVERY_TIMEOUT_S));

	errno = 0;
	EXPECT_EQ(-1, ioctl(qdma_fd, SLASH_QDMA_IOCTL_INFO, &info));
	EXPECT_EQ(ENODEV, errno)
	TH_LOG("old PF1 fd rebound to the rescanned device");
	EXPECT_EQ(0, close(qdma_fd));
}

TEST_F(hotplug, remove_pf1_with_live_qpair_cleans_up)
{
	/*
	 * Keep both the management fd and an anon transfer fd open while PF1
	 * is removed. The driver must reclaim the live HW queues, leave both
	 * old fds safely offline, and permit a fresh instance after rescan.
	 */
	struct slash_qdma_buf_create create;
	int qdma_fd;
	int io_fd = -1;
	uint32_t qid = 0;

	if (getenv("SLASH_TEST_DESTRUCTIVE") == NULL)
		SKIP(return, "remove PF1 with live queue pairs."
					 "set SLASH_TEST_DESTRUCTIVE=1 to run");

	qdma_fd = open(self->accels[0].qdma_identity.devpath, O_RDWR);
	ASSERT_GE(qdma_fd, 0)
	TH_LOG("open(%s) failed: %s",
		   self->accels[0].qdma_identity.devpath, strerror(errno));

	/* Add a qpair and intentionally skip DEL — the teardown loop must
	 * reclaim it when the device disappears. Bidirectional so both
	 * H2C and C2H queue handles need cleanup. */
	ASSERT_EQ(0, slash_qpair_add(qdma_fd, 0 /* MM */, 0x3, &qid));
	ASSERT_EQ(0, slash_qpair_op(qdma_fd, qid, SLASH_QDMA_QUEUE_OP_START));
	io_fd = slash_qpair_get_fd(qdma_fd, qid, O_CLOEXEC);
	ASSERT_GE(io_fd, 0);

	ASSERT_EQ(0, hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE,
							  self->accels[0].pf1_bdf));
	EXPECT_EQ(0, poll_slash_absent(
					 self->accels[0].qdma_identity.sysfs_name,
					 NODE_RECOVERY_TIMEOUT_S))
	TH_LOG("%s/%s did not disappear after REMOVE",
		   SLASH_TEST_SYSFS_CLASS_DIR,
		   self->accels[0].qdma_identity.sysfs_name);

	memset(&create, 0, sizeof(create));
	create.size = sizeof(create);
	create.flags = O_CLOEXEC;
	create.length = 4096;
	errno = 0;
	EXPECT_EQ(-1, ioctl(io_fd, SLASH_QDMA_IOCTL_BUF_CREATE, &create));
	EXPECT_EQ(ENODEV, errno)
	TH_LOG("old QDMA anon fd remained usable after PF1 removal");

	ASSERT_EQ(0, ioctl(self->hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN));
	EXPECT_EQ(0, poll_accelerators_present(self->accels, self->n_accels,
										   NODE_RECOVERY_TIMEOUT_S))
	TH_LOG("not all accelerators reappeared after RESCAN");

	errno = 0;
	EXPECT_EQ(-1, ioctl(io_fd, SLASH_QDMA_IOCTL_BUF_CREATE, &create));
	EXPECT_EQ(ENODEV, errno)
	TH_LOG("old QDMA anon fd rebound to the rescanned device");

	EXPECT_EQ(0, close(io_fd));
	EXPECT_EQ(0, close(qdma_fd));
}

TEST_F(hotplug, hotplug_atomic_pf2)
{
	/*
	 * HOTPLUG = REMOVE + RESCAN atomically.  By the time the ioctl
	 * returns the bus has been rescanned; allow a brief window for
	 * udev to recreate the /dev nodes.
	 */
	ASSERT_EQ(0, hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_HOTPLUG,
							  self->accels[0].pf2_bdf));
	EXPECT_EQ(0, poll_accelerators_present(self->accels, self->n_accels,
										   NODE_RECOVERY_TIMEOUT_S));
}

TEST_F(hotplug, hotplug_malformed_bdf)
{
	EXPECT_EQ(-EINVAL,
			  hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_HOTPLUG,
						   "not-a-bdf"));
}

TEST_F(hotplug, hotplug_unknown_bdf)
{
	EXPECT_EQ(-ENODEV,
			  hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_HOTPLUG,
						   "ffff:ff:1f.7"));
}

TEST_F(hotplug, toggle_sbr_malformed_bdf)
{
	EXPECT_EQ(-EINVAL,
			  hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_TOGGLE_SBR,
						   "not-a-bdf"));
}

TEST_F(hotplug, toggle_sbr_no_upstream_bridge)
{
	EXPECT_EQ(-ENODEV,
			  hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_TOGGLE_SBR,
						   "ffff:ff:00.0"));
}

/* ====================================================================
 * ABI size-versioning tests
 *
 * REMOVE, TOGGLE_SBR, and HOTPLUG share slash_hotplug_copy_request,
 * which accepts size zero for legacy callers and rejects nonzero sizes below
 * sizeof(struct) with -EINVAL. Non-destructive: no real device is touched.
 * ==================================================================== */

TEST_F(hotplug, zero_size_uses_legacy_request_layout)
{
	struct slash_hotplug_device_request req;

	memset(&req, 0, sizeof(req));
	strncpy(req.bdf, "ffff:ff:1f.7", sizeof(req.bdf) - 1);

	EXPECT_EQ(-1, ioctl(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE, &req));
	EXPECT_EQ(ENODEV, errno);
}

TEST_F(hotplug, remove_size_below_struct_returns_einval)
{
	struct slash_hotplug_device_request req;

	memset(&req, 0, sizeof(req));
	req.size = sizeof(__u32); /* size field only — below sizeof(struct) */
	strncpy(req.bdf, "ffff:ff:1f.7", sizeof(req.bdf) - 1);

	EXPECT_EQ(-1, ioctl(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE, &req));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(hotplug, toggle_sbr_size_below_struct_returns_einval)
{
	struct slash_hotplug_device_request req;

	memset(&req, 0, sizeof(req));
	req.size = sizeof(__u32);
	strncpy(req.bdf, "ffff:ff:00.0", sizeof(req.bdf) - 1);

	EXPECT_EQ(-1, ioctl(self->hp_fd, SLASH_HOTPLUG_IOCTL_TOGGLE_SBR, &req));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(hotplug, hotplug_size_below_struct_returns_einval)
{
	struct slash_hotplug_device_request req;

	memset(&req, 0, sizeof(req));
	req.size = sizeof(__u32);
	strncpy(req.bdf, "ffff:ff:1f.7", sizeof(req.bdf) - 1);

	EXPECT_EQ(-1, ioctl(self->hp_fd, SLASH_HOTPLUG_IOCTL_HOTPLUG, &req));
	EXPECT_EQ(EINVAL, errno);
}

TEST_F(hotplug, full_sbr_cycle)
{
	if (getenv("SLASH_TEST_DESTRUCTIVE") == NULL)
		SKIP(return, "full board reset (~10 s); "
					 "set SLASH_TEST_DESTRUCTIVE=1 to run");

	ASSERT_EQ(0, hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE,
							  self->accels[0].pf1_bdf));
	ASSERT_EQ(0, hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE,
							  self->accels[0].pf2_bdf));

	ASSERT_EQ(0, hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_TOGGLE_SBR,
							  self->accels[0].pf0_bdf));

	sleep(SBR_SETTLE_SECONDS);

	ASSERT_EQ(0, ioctl(self->hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN));

	EXPECT_EQ(0, poll_accelerators_present(self->accels, self->n_accels,
										   NODE_RECOVERY_TIMEOUT_S))
	TH_LOG("not all accelerators reappeared after SBR + RESCAN");
}

TEST_HARNESS_MAIN
