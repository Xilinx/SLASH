#/**
# * Copyright (C) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
# * This program is free software; you can redistribute it and/or modify it under the terms of the
# * GNU General Public License as published by the Free Software Foundation; version 2.
# *
# * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
# * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
# * General Public License for more details.
# *
# * You should have received a copy of the GNU General Public License along with this program; if
# * not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
# * 02110-1301, USA.
# */


MODULE      := slash

ifneq ($(KERNELDIR),)
KDIR        ?= $(KERNELDIR)
else
KDIR        ?= /lib/modules/$(shell uname -r)/build
endif
PWD         := $(shell pwd)

# Driver version. Priority:
#   1. SLASH_VERSION passed in (e.g. from DKMS MAKE[0]).
#   2. packaging/version file (in-tree builds).
#   3. "unknown" fallback (DKMS install tree has no packaging/version).
SLASH_VERSION ?= $(strip $(shell cat $(src)/../packaging/version 2>/dev/null))
ifeq ($(SLASH_VERSION),)
SLASH_VERSION := unknown
endif

# This exists when installed for dkms
LIBQDMA_LOCAL_DIR := ./libqdma
# This exists when building from tree
LIBQDMA_FALLBACK  := ../submodules/qdma_drv/QDMA/linux-kernel/driver/libqdma

ifneq (,$(wildcard $(src)/$(LIBQDMA_LOCAL_DIR)/.))
  LIBQDMA_PATH := $(LIBQDMA_LOCAL_DIR)
else
  LIBQDMA_PATH := $(LIBQDMA_FALLBACK)
endif

# SLASH carries a few local modifications to the pinned QDMA submodule's
# libqdma sources (see $(LIBQDMA_PATCH_DIR)/). The submodule itself stays
# pristine; the patches are applied to whichever libqdma tree is being built
# (the DKMS-local ./libqdma or the in-tree submodule) by the libqdma-patches
# target before the module is compiled. See that target for details.
LIBQDMA_PATCH_DIR := patches

SLASH_QDMA_OP_DEBUG ?= 0

# Per-transfer timing instrumentation. Set to 1 to emit one dmesg line per
# DMA transfer breaking down the kernel phases. Default off (zero overhead).
SLASH_QDMA_TIMING ?= 0

# Build libqdma's runtime CSR setters so probe can tune the global writeback
# interval before any queue is created.
ccflags-y += -DQDMA_CSR_REG_UPDATE=1

# Kcompat feature flags. Defaults are "n"; the all: recipe runs
# driver/kcompat/probe.sh against $(KDIR) to detect the actual values
# and passes them into the kbuild recursion. Each pair (modern API +
# legacy fallback) is covered by one probe — if the modern form is
# absent, the legacy form is the unconditional fallback in slash_compat.h.
SLASH_HAVE_VM_FLAGS_SET ?= n
SLASH_HAVE_MODULE_IMPORT_NS_TOKEN ?= n
SLASH_HAVE_URING_CMD ?= n
SLASH_HAVE_URING_SQE_CMD ?= n
SLASH_HAVE_CLASS_CREATE_ONE_ARG ?= n
SLASH_HAVE_CLASS_DEVNODE_CONST ?= n

# Set GCOV=1 to instrument the module for kernel gcov coverage.
# Not set by default — never enable this in production builds.
ifdef GCOV
GCOV_PROFILE := y
endif

obj-m          := $(MODULE).o
$(MODULE)-objs := $(MODULE)_main.o $(MODULE)_chrdev.o $(MODULE)_ctldev.o $(MODULE)_pcie.o $(MODULE)_dmabuf.o $(MODULE)_hotplug.o $(MODULE)_qdma.o
ccflags-y += \
	-I$(src)/libslash/include \
	-I$(src)/libslash/include/slash/uapi \
	-I$(src)/$(LIBQDMA_PATH) \
	-I$(src)/$(LIBQDMA_PATH)/qdma_access \
	-I$(src)/$(LIBQDMA_PATH)/qdma_access/eqdma_cpm5_access \
	-I$(src)/$(LIBQDMA_PATH)/qdma_access/eqdma_soft_access \
	-I$(src)/$(LIBQDMA_PATH)/qdma_access/qdma_cpm4_access \
	-I$(src)/$(LIBQDMA_PATH)/qdma_access/qdma_soft_access \
	\
	-DTANDEM_BOOT_SUPPORTED=1 \
	-DSLASH_QDMA_OP_DEBUG=$(SLASH_QDMA_OP_DEBUG) \
	-DSLASH_QDMA_TIMING=$(SLASH_QDMA_TIMING) \
	-DSLASH_VERSION_STR=\"$(SLASH_VERSION)\"

ifeq ($(SLASH_HAVE_VM_FLAGS_SET),y)
ccflags-y += -DSLASH_HAVE_VM_FLAGS_SET
endif

ifeq ($(SLASH_HAVE_MODULE_IMPORT_NS_TOKEN),y)
ccflags-y += -DSLASH_HAVE_MODULE_IMPORT_NS_TOKEN
endif

ifeq ($(SLASH_HAVE_CLASS_CREATE_ONE_ARG),y)
ccflags-y += -DSLASH_HAVE_CLASS_CREATE_ONE_ARG
endif

ifeq ($(SLASH_HAVE_CLASS_DEVNODE_CONST),y)
ccflags-y += -DSLASH_HAVE_CLASS_DEVNODE_CONST
endif

# Optional io_uring uring_cmd async transfer path. Probed by kcompat; absent on
# kernels without CONFIG_IO_URING or uring_cmd support (e.g. RHEL 9, Ubuntu
# 22.04 GA), where the synchronous transfer ioctl remains the only path.
ifeq ($(SLASH_HAVE_URING_CMD),y)
ccflags-y += -DSLASH_HAVE_URING_CMD
endif

# Selects the io_uring SQE payload accessor: io_uring_sqe_cmd(cmd->sqe) when
# present (newer kernels + distro backports), else cmd->cmd. Only meaningful
# when SLASH_HAVE_URING_CMD is also set.
ifeq ($(SLASH_HAVE_URING_SQE_CMD),y)
ccflags-y += -DSLASH_HAVE_URING_SQE_CMD
endif

# Force-include the compat header into every TU (including the pinned libqdma
# submodule sources we don't modify) so kernel-API shims such as from_timer()
# reach third-party code too. Safe on all kernels: the shims are guarded.
ccflags-y += -include $(src)/slash_compat.h


LIBQDMA_OBJS := \
	$(LIBQDMA_PATH)/qdma_mbox.o \
	$(LIBQDMA_PATH)/qdma_intr.o \
	$(LIBQDMA_PATH)/qdma_st_c2h.o \
	$(LIBQDMA_PATH)/qdma_thread.o \
	$(LIBQDMA_PATH)/libqdma_export.o \
	$(LIBQDMA_PATH)/qdma_context.o \
	$(LIBQDMA_PATH)/qdma_sriov.o \
	$(LIBQDMA_PATH)/qdma_platform.o \
	$(LIBQDMA_PATH)/qdma_descq.o \
	$(LIBQDMA_PATH)/qdma_regs.o \
	$(LIBQDMA_PATH)/qdma_debugfs.o \
	$(LIBQDMA_PATH)/qdma_debugfs_dev.o \
	$(LIBQDMA_PATH)/qdma_debugfs_queue.o \
	$(LIBQDMA_PATH)/libqdma_config.o \
	$(LIBQDMA_PATH)/qdma_device.o \
	$(LIBQDMA_PATH)/xdev.o \
	$(LIBQDMA_PATH)/thread.o

QDMA_ACCESS_OBJS := \
	$(LIBQDMA_PATH)/qdma_access/qdma_mbox_protocol.o \
	$(LIBQDMA_PATH)/qdma_access/qdma_list.o \
	$(LIBQDMA_PATH)/qdma_access/qdma_access_common.o \
	$(LIBQDMA_PATH)/qdma_access/qdma_resource_mgmt.o \
	$(LIBQDMA_PATH)/qdma_access/qdma_cpm4_access/qdma_cpm4_access.o \
	$(LIBQDMA_PATH)/qdma_access/qdma_cpm4_access/qdma_cpm4_reg_dump.o \
	$(LIBQDMA_PATH)/qdma_access/qdma_soft_access/qdma_soft_access.o \
	$(LIBQDMA_PATH)/qdma_access/eqdma_soft_access/eqdma_soft_access.o \
	$(LIBQDMA_PATH)/qdma_access/eqdma_soft_access/eqdma_soft_reg_dump.o \
	$(LIBQDMA_PATH)/qdma_access/eqdma_cpm5_access/eqdma_cpm5_access.o \
	$(LIBQDMA_PATH)/qdma_access/eqdma_cpm5_access/eqdma_cpm5_reg_dump.o

$(MODULE)-objs += $(LIBQDMA_OBJS) $(QDMA_ACCESS_OBJS)


KCOMPAT := "$(SHELL)" "$(PWD)/kcompat/probe.sh"

all: libqdma-patches
	@flags="$$($(KCOMPAT) "$(KDIR)" | tr '\n' ' ')"; \
	echo "slash: kcompat: $$flags"; \
	$(MAKE) -C "$(KDIR)" M="$(PWD)" $$flags modules

# Apply SLASH's local libqdma patches ($(LIBQDMA_PATCH_DIR)/*.patch) to the
# libqdma source tree in use, in filename order, right before building.
#
# The pinned submodule is not edited directly by commits: patches live in-tree
# and are stamped onto the working copy here. Application is idempotent — each patch is first tested
# for being already applied (reverse dry-run) and skipped if so — so repeated
# `make` runs, incremental builds, and DKMS rebuilds are all safe. A patch that
# neither applies cleanly nor is already present aborts the build.
#
# $(PWD) is the driver dir for both `make` (in-tree) and DKMS (MAKE[0] runs
# `make -C driver ...`); ./libqdma is the DKMS-packaged copy, otherwise fall
# back to the in-tree submodule path. Uses patch(1) so it is independent of
# whether the libqdma tree lives inside a git checkout.
libqdma-patches:
	@set -e; \
	patch_dir="$(PWD)/$(LIBQDMA_PATCH_DIR)"; \
	set -- "$$patch_dir"/*.patch; \
	if [ ! -e "$$1" ]; then exit 0; fi; \
	if [ -d "$(PWD)/libqdma" ]; then lq="$(PWD)/libqdma"; \
	else lq="$(PWD)/$(LIBQDMA_FALLBACK)"; fi; \
	if [ ! -d "$$lq" ]; then \
		echo "slash: ERROR libqdma sources not found at $$lq" >&2; \
		echo "slash:       run 'git submodule update --init --recursive' first" >&2; \
		exit 1; \
	fi; \
	command -v patch >/dev/null 2>&1 || { \
		echo "slash: ERROR patch(1) not found; it is required to apply libqdma patches" >&2; \
		exit 1; }; \
	for p in "$$@"; do \
		name="$$(basename "$$p")"; \
		if patch -R -p1 -d "$$lq" --dry-run -f -s -i "$$p" >/dev/null 2>&1; then \
			echo "slash: libqdma patch already applied, skipping: $$name"; \
		elif patch -p1 -d "$$lq" --dry-run -f -s -i "$$p" >/dev/null 2>&1; then \
			echo "slash: applying libqdma patch: $$name"; \
			patch -p1 -d "$$lq" -f -s -i "$$p"; \
		else \
			echo "slash: ERROR libqdma patch does not apply cleanly: $$name" >&2; \
			echo "slash:       (libqdma tree at $$lq is neither pristine nor already patched)" >&2; \
			exit 1; \
		fi; \
	done

# Best-effort revert of the libqdma patches, restoring the submodule working
# copy to pristine. Useful when editing the patches themselves. Never fails the
# build: patches that are not currently applied are simply skipped.
unpatch-libqdma:
	@set -e; \
	patch_dir="$(PWD)/$(LIBQDMA_PATCH_DIR)"; \
	set -- "$$patch_dir"/*.patch; \
	if [ ! -e "$$1" ]; then exit 0; fi; \
	if [ -d "$(PWD)/libqdma" ]; then lq="$(PWD)/libqdma"; \
	else lq="$(PWD)/$(LIBQDMA_FALLBACK)"; fi; \
	[ -d "$$lq" ] || exit 0; \
	for p in $$(printf '%s\n' "$$@" | tac); do \
		name="$$(basename "$$p")"; \
		if patch -R -p1 -d "$$lq" --dry-run -f -s -i "$$p" >/dev/null 2>&1; then \
			echo "slash: reverting libqdma patch: $$name"; \
			patch -R -p1 -d "$$lq" -f -s -i "$$p"; \
		fi; \
	done

clean:
	-$(MAKE) -C "$(KDIR)" M="$(PWD)" clean
	rm -rf "$(PWD)/kcompat/.scratch"
	$(MAKE) unpatch-libqdma

install: all
	sudo install -d -m 755 /lib/modules/$(shell uname -r)/extra
	sudo install -m 644 $(MODULE).ko /lib/modules/$(shell uname -r)/extra
	sudo depmod -a

.PHONY: all clean install libqdma-patches unpatch-libqdma
