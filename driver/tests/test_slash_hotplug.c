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
 * system at setup time by scanning /sys/class/misc/ for slash_ctl_<BDF>
 * (PF2) and slash_qdma_ctl_<BDF> (PF1), then pairs them by the
 * "DDDD:BB:SS" bus prefix.  Operations target the first accelerator;
 * the teardown polls until *every* discovered accelerator has both its
 * ctl and qdma sysfs entries back and pointing at a /dev node whose
 * major:minor match the sysfs `dev` attribute.  This catches both
 * cross-card damage (touching the wrong accelerator) and stale /dev
 * state (sysfs back but udev missed the node, or wrong minor).
 *
 * Two tests perform a full board reset (TOGGLE_SBR) or accept extended
 * PCIe downtime; these are gated by SLASH_TEST_DESTRUCTIVE=1.  All other
 * tests run on every invocation.
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
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>

#define NODE_RECOVERY_TIMEOUT_S 10

#define SLASH_TEST_MAX_ACCELERATORS 16
#define SYSFS_MISC_DIR "/sys/class/misc"
#define CTL_SYSFS_PREFIX "slash_ctl_"
#define QDMA_SYSFS_PREFIX "slash_qdma_ctl_"

/* ====================================================================
 * Accelerator discovery + verification
 * ==================================================================== */

struct accelerator
{
	char pf0_bdf[SLASH_PCI_BDF_LEN]; /* derived: "<bus_prefix>.0" */
	char pf1_bdf[SLASH_PCI_BDF_LEN]; /* from slash_qdma_ctl_<BDF>   */
	char pf2_bdf[SLASH_PCI_BDF_LEN]; /* from slash_ctl_<BDF>        */
};

/* Slurp a sysfs file into a NUL-terminated buffer, trimming a trailing newline. */
static int read_sysfs_string(const char *path, char *buf, size_t buf_sz)
{
	int fd = open(path, O_RDONLY);
	ssize_t n;

	if (fd < 0)
		return -errno;
	n = read(fd, buf, buf_sz - 1);
	close(fd);
	if (n < 0)
		return -errno;
	buf[n] = '\0';
	while (n > 0 && (buf[n - 1] == '\n' || buf[n - 1] == '\r'))
		buf[--n] = '\0';
	return 0;
}

/* Read DEVNAME=... from /sys/class/misc/<basename>/uevent. */
static int get_misc_devname(const char *sysfs_basename, char *out, size_t out_sz)
{
	char path[256];
	char buf[1024];
	char *line, *saveptr;
	int err;

	snprintf(path, sizeof(path), SYSFS_MISC_DIR "/%s/uevent", sysfs_basename);
	err = read_sysfs_string(path, buf, sizeof(buf));
	if (err)
		return err;

	for (line = strtok_r(buf, "\n", &saveptr); line;
		 line = strtok_r(NULL, "\n", &saveptr))
	{
		if (strncmp(line, "DEVNAME=", 8) == 0)
		{
			strncpy(out, line + 8, out_sz - 1);
			out[out_sz - 1] = '\0';
			return 0;
		}
	}
	return -ENOENT;
}

/* Read MAJOR:MINOR from /sys/class/misc/<basename>/dev. */
static int get_misc_devnum(const char *sysfs_basename,
						   unsigned int *major_out, unsigned int *minor_out)
{
	char path[256];
	char buf[64];
	int err;

	snprintf(path, sizeof(path), SYSFS_MISC_DIR "/%s/dev", sysfs_basename);
	err = read_sysfs_string(path, buf, sizeof(buf));
	if (err)
		return err;
	if (sscanf(buf, "%u:%u", major_out, minor_out) != 2)
		return -EINVAL;
	return 0;
}

/*
 * Verify a misc-class sysfs entry exists and points at a real /dev node.
 *
 *   0       — sysfs entry exists, /dev node exists, major:minor match
 *   -ENOENT — sysfs entry or /dev node missing
 *   -EIO    — /dev node isn't a char dev, or its rdev doesn't match sysfs
 *   other   — read/stat failure
 */
static int verify_misc_node(const char *sysfs_basename)
{
	char devname[64];
	char devpath[128];
	unsigned int sysfs_major = 0, sysfs_minor = 0;
	struct stat st;
	int err;

	err = get_misc_devnum(sysfs_basename, &sysfs_major, &sysfs_minor);
	if (err)
		return err;
	err = get_misc_devname(sysfs_basename, devname, sizeof(devname));
	if (err)
		return err;

	snprintf(devpath, sizeof(devpath), "/dev/%s", devname);
	if (stat(devpath, &st) < 0)
		return -errno;
	if (!S_ISCHR(st.st_mode))
		return -EIO;
	if (major(st.st_rdev) != sysfs_major || minor(st.st_rdev) != sysfs_minor)
		return -EIO;
	return 0;
}

/* Both ctl and qdma nodes for one accelerator verify cleanly. */
static int verify_accelerator_present(const struct accelerator *a)
{
	char name[64];
	int err;

	snprintf(name, sizeof(name), CTL_SYSFS_PREFIX "%s", a->pf2_bdf);
	err = verify_misc_node(name);
	if (err)
		return err;

	snprintf(name, sizeof(name), QDMA_SYSFS_PREFIX "%s", a->pf1_bdf);
	return verify_misc_node(name);
}

/*
 * Discover every SLASH accelerator by scanning /sys/class/misc/.
 *
 * Unpaired entries (PF2 without a matching PF1, or vice versa) indicate
 * a partial probe failure — log a warning and skip the orphan.
 */
static int discover_accelerators(struct accelerator *out, int max, int *n_out)
{
	char pf2_bdfs[SLASH_TEST_MAX_ACCELERATORS][SLASH_PCI_BDF_LEN] = {{0}};
	char pf1_bdfs[SLASH_TEST_MAX_ACCELERATORS][SLASH_PCI_BDF_LEN] = {{0}};
	int pf2_paired[SLASH_TEST_MAX_ACCELERATORS] = {0};
	int pf1_paired[SLASH_TEST_MAX_ACCELERATORS] = {0};
	int n_pf2 = 0, n_pf1 = 0;
	int n_accels = 0;
	int i, j;
	DIR *d;
	struct dirent *de;

	d = opendir(SYSFS_MISC_DIR);
	if (!d)
		return -errno;

	while ((de = readdir(d)) != NULL)
	{
		/* Match QDMA prefix first — neither prefix is a prefix of
		 * the other, but the ordering keeps intent obvious. */
		if (strncmp(de->d_name, QDMA_SYSFS_PREFIX,
					strlen(QDMA_SYSFS_PREFIX)) == 0)
		{
			if (n_pf1 < SLASH_TEST_MAX_ACCELERATORS)
			{
				strncpy(pf1_bdfs[n_pf1],
						de->d_name + strlen(QDMA_SYSFS_PREFIX),
						SLASH_PCI_BDF_LEN - 1);
				n_pf1++;
			}
		}
		else if (strncmp(de->d_name, CTL_SYSFS_PREFIX,
						 strlen(CTL_SYSFS_PREFIX)) == 0)
		{
			if (n_pf2 < SLASH_TEST_MAX_ACCELERATORS)
			{
				strncpy(pf2_bdfs[n_pf2],
						de->d_name + strlen(CTL_SYSFS_PREFIX),
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

			pf2_paired[i] = 1;
			pf1_paired[j] = 1;
			n_accels++;
			break;
		}
	}

	for (i = 0; i < n_pf2; i++)
		if (!pf2_paired[i])
			fprintf(stderr,
					"# WARNING: unpaired " CTL_SYSFS_PREFIX
					"%s (no matching " QDMA_SYSFS_PREFIX "<%.*s.x>)\n",
					pf2_bdfs[i],
					SLASH_TEST_BUS_PREFIX_LEN - 1, pf2_bdfs[i]);
	for (j = 0; j < n_pf1; j++)
		if (!pf1_paired[j])
			fprintf(stderr,
					"# WARNING: unpaired " QDMA_SYSFS_PREFIX
					"%s (no matching " CTL_SYSFS_PREFIX "<%.*s.x>)\n",
					pf1_bdfs[j],
					SLASH_TEST_BUS_PREFIX_LEN - 1, pf1_bdfs[j]);

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

/* Wait until /sys/class/misc/<basename> is gone. */
static int poll_misc_absent(const char *sysfs_basename, int timeout_s)
{
	char path[256];
	int i;

	snprintf(path, sizeof(path), SYSFS_MISC_DIR "/%s", sysfs_basename);
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
	struct accelerator accels[SLASH_TEST_MAX_ACCELERATORS];
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

	ASSERT_EQ(0, discover_accelerators(self->accels,
									   SLASH_TEST_MAX_ACCELERATORS,
									   &self->n_accels));
	if (self->n_accels == 0)
		SKIP(return, "no SLASH accelerators discovered in " SYSFS_MISC_DIR);

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

TEST_F(hotplug, rescan_smoke)
{
	EXPECT_EQ(0, ioctl(self->hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN));

	/* All accelerators that were present before must still be present. */
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

TEST_F(hotplug, remove_then_rescan_recovers_pf2)
{
	char sysfs_name[64];

	snprintf(sysfs_name, sizeof(sysfs_name),
			 CTL_SYSFS_PREFIX "%s", self->accels[0].pf2_bdf);

	ASSERT_EQ(0, hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE,
							  self->accels[0].pf2_bdf));
	EXPECT_EQ(0, poll_misc_absent(sysfs_name, NODE_RECOVERY_TIMEOUT_S))
	TH_LOG("%s/%s did not disappear after REMOVE",
		   SYSFS_MISC_DIR, sysfs_name);

	ASSERT_EQ(0, ioctl(self->hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN));
	EXPECT_EQ(0, poll_accelerators_present(self->accels, self->n_accels,
										   NODE_RECOVERY_TIMEOUT_S))
	TH_LOG("not all accelerators reappeared after RESCAN");
}

TEST_F(hotplug, remove_then_rescan_recovers_pf1)
{
	char sysfs_name[64];

	snprintf(sysfs_name, sizeof(sysfs_name),
			 QDMA_SYSFS_PREFIX "%s", self->accels[0].pf1_bdf);

	ASSERT_EQ(0, hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE,
							  self->accels[0].pf1_bdf));
	EXPECT_EQ(0, poll_misc_absent(sysfs_name, NODE_RECOVERY_TIMEOUT_S))
	TH_LOG("%s/%s did not disappear after REMOVE",
		   SYSFS_MISC_DIR, sysfs_name);

	ASSERT_EQ(0, ioctl(self->hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN));
	EXPECT_EQ(0, poll_accelerators_present(self->accels, self->n_accels,
										   NODE_RECOVERY_TIMEOUT_S))
	TH_LOG("not all accelerators reappeared after RESCAN");
}

TEST_F(hotplug, remove_pf1_with_live_qpair_cleans_up)
{
	/*
	 * Closing the qdma_ctl fd does not release qpairs — they live on
	 * the device, not the fd. If userspace leaks a qpair and the device
	 * is then removed, slash_qdma_destroy_qdma_device's xa_for_each
	 * teardown loop must stop and reclaim the leaked HW queues via
	 * slash_qdma_ioctl_qpair_rm_q. This test exercises that path: add a
	 * qpair, close the fd without DEL, REMOVE the PF1, then RESCAN.
	 * Recovery is verified via poll_accelerators_present.
	 */
	char qdma_sysfs[64];
	char qdma_devname[64];
	char qdma_path[128];
	int qdma_fd;
	uint32_t qid;

	if (getenv("SLASH_TEST_DESTRUCTIVE") == NULL)
		SKIP(return, "remove PF1 with live queue pairs."
					 "set SLASH_TEST_DESTRUCTIVE=1 to run");

	snprintf(qdma_sysfs, sizeof(qdma_sysfs),
			 QDMA_SYSFS_PREFIX "%s", self->accels[0].pf1_bdf);
	ASSERT_EQ(0, get_misc_devname(qdma_sysfs, qdma_devname,
								  sizeof(qdma_devname)));
	snprintf(qdma_path, sizeof(qdma_path), "/dev/%s", qdma_devname);

	qdma_fd = open(qdma_path, O_RDWR);
	ASSERT_GE(qdma_fd, 0)
	TH_LOG("open(%s) failed: %s", qdma_path, strerror(errno));

	/* Add a qpair and intentionally skip DEL — the teardown loop must
	 * reclaim it when the device disappears. Bidirectional so both
	 * H2C and C2H queue handles need cleanup. */
	ASSERT_EQ(0, slash_qpair_add(qdma_fd, 0 /* MM */, 0x3, &qid));

	close(qdma_fd);

	ASSERT_EQ(0, hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE,
							  self->accels[0].pf1_bdf));
	EXPECT_EQ(0, poll_misc_absent(qdma_sysfs, NODE_RECOVERY_TIMEOUT_S))
	TH_LOG("%s/%s did not disappear after REMOVE",
		   SYSFS_MISC_DIR, qdma_sysfs);

	ASSERT_EQ(0, ioctl(self->hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN));
	EXPECT_EQ(0, poll_accelerators_present(self->accels, self->n_accels,
										   NODE_RECOVERY_TIMEOUT_S))
	TH_LOG("not all accelerators reappeared after RESCAN");
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

TEST_F(hotplug, toggle_sbr_rejects_live_bus)
{
	EXPECT_EQ(-EBUSY,
			  hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_TOGGLE_SBR,
						   self->accels[0].pf0_bdf));
}

/* ====================================================================
 * Destructive (env-gated)
 * ==================================================================== */

/* ====================================================================
 * ABI size-versioning tests
 *
 * REMOVE, TOGGLE_SBR, and HOTPLUG share slash_hotplug_copy_request,
 * which rejects any size < sizeof(struct) with -EINVAL. Non-destructive:
 * no real device is touched.
 * ==================================================================== */

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
		SKIP(return, "full board reset (may take up to 30 s); "
					 "set SLASH_TEST_DESTRUCTIVE=1 to run");

	ASSERT_EQ(0, hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE,
							  self->accels[0].pf0_bdf));
	ASSERT_EQ(0, hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE,
							  self->accels[0].pf1_bdf));
	ASSERT_EQ(0, hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_REMOVE,
							  self->accels[0].pf2_bdf));

	ASSERT_EQ(0, hp_ioctl_bdf(self->hp_fd, SLASH_HOTPLUG_IOCTL_TOGGLE_SBR,
							  self->accels[0].pf0_bdf));

	ASSERT_EQ(0, ioctl(self->hp_fd, SLASH_HOTPLUG_IOCTL_RESCAN));

	EXPECT_EQ(0, poll_accelerators_present(self->accels, self->n_accels,
										   NODE_RECOVERY_TIMEOUT_S))
	TH_LOG("not all accelerators reappeared after SBR + RESCAN");
}

TEST_HARNESS_MAIN
