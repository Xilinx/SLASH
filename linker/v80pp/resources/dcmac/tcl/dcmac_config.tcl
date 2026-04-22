# Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
# SPDX-License-Identifier: MIT

# A front view of the V80 and QSFP56 index and associated DCMAC
# Use this diagram to guide the configuration
#
#   _________________________
#   |  0  |  1  |  2  |  3  |
#   ----------------------------> PCIe
#
#   \___________/\__________/
#         |           |
#       DCMAC0      DCMAC1


### Enable the DCMAC core(s) that you wish to use
set DCMAC0_ENABLED 1
set DCMAC1_ENABLED 1

## Each DCMAC can support 2 QSFP56 interfaces
## select how many QSFP56 you want for each DCMAC, provided they are enabled

## Setup number of QSFP56 interfaces for DCMAC0
set DUAL_QSFP_DCMAC0 0

## Setup number of QSFP56 interfaces for DCMAC1
set DUAL_QSFP_DCMAC1 0
