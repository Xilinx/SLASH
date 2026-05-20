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

SLASH_QDMA_OP_DEBUG ?= 0

# Kcompat feature flags. Defaults are "n"; the all: recipe runs
# driver/kcompat/probe.sh against $(KDIR) to detect the actual values
# and passes them into the kbuild recursion. Each pair (modern API +
# legacy fallback) is covered by one probe — if the modern form is
# absent, the legacy form is the unconditional fallback in slash_compat.h.
SLASH_HAVE_VM_FLAGS_SET ?= n
SLASH_HAVE_MODULE_IMPORT_NS_TOKEN ?= n

# Set GCOV=1 to instrument the module for kernel gcov coverage.
# Not set by default — never enable this in production builds.
ifdef GCOV
GCOV_PROFILE := y
endif

obj-m          := $(MODULE).o
$(MODULE)-objs := $(MODULE)_main.o $(MODULE)_ctldev.o $(MODULE)_pcie.o $(MODULE)_dmabuf.o $(MODULE)_hotplug.o $(MODULE)_qdma.o
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
	-DSLASH_VERSION_STR=\"$(SLASH_VERSION)\"

ifeq ($(SLASH_HAVE_VM_FLAGS_SET),y)
ccflags-y += -DSLASH_HAVE_VM_FLAGS_SET
endif

ifeq ($(SLASH_HAVE_MODULE_IMPORT_NS_TOKEN),y)
ccflags-y += -DSLASH_HAVE_MODULE_IMPORT_NS_TOKEN
endif


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

all:
	@flags="$$($(KCOMPAT) "$(KDIR)" | tr '\n' ' ')"; \
	echo "slash: kcompat: $$flags"; \
	$(MAKE) -C "$(KDIR)" M="$(PWD)" $$flags modules

clean:
	$(MAKE) -C "$(KDIR)" M="$(PWD)" clean
	rm -rf "$(PWD)/kcompat/.scratch"

install: all
	sudo install -d -m 755 /lib/modules/$(shell uname -r)/extra
	sudo install -m 644 $(MODULE).ko /lib/modules/$(shell uname -r)/extra
	sudo depmod -a

.PHONY: all clean install
