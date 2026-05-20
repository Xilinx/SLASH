..
   comment:: SPDX-License-Identifier: MIT
   comment:: Copyright (C) 2025 Advanced Micro Devices, Inc

################
libslash C API
################

libslash is a userspace C library wrapping the SLASH kernel driver. It provides
three modules: Control (BAR access), QDMA (DMA transfers), and Hotplug (PCIe lifecycle).

.. toctree::
   :maxdepth: 1

   ctldev
   qdma
   hotplug
