# ##################################################################################################
#  The MIT License (MIT)
#  Copyright (c) 2025-2026 Advanced Micro Devices, Inc. All rights reserved.
# 
#  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
#  and associated documentation files (the "Software"), to deal in the Software without restriction,
#  including without limitation the rights to use, copy, modify, merge, publish, distribute,
#  sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
#  furnished to do so, subject to the following conditions:
# 
#  The above copyright notice and this permission notice shall be included in all copies or
#  substantial portions of the Software.
# 
# THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
# NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
# NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
# DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
# OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
# ##################################################################################################

set(AMI_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../../submodules/AVED/sw/AMI")
set(AMI_API_DIR "${AMI_DIR}/api")
set(AMI_API_SRC_DIR "${AMI_API_DIR}/src")
set(AMI_API_INCLUDE_DIR "${AMI_API_DIR}/include")

add_library(
    ami
    
    STATIC
    
    "${AMI_API_SRC_DIR}/ami.c"
    "${AMI_API_SRC_DIR}/ami_device.c"
    "${AMI_API_SRC_DIR}/ami_eeprom_access.c"
    "${AMI_API_SRC_DIR}/ami_mem_access.c"
    "${AMI_API_SRC_DIR}/ami_mfg_info.c"
    "${AMI_API_SRC_DIR}/ami_module_access.c"
    "${AMI_API_SRC_DIR}/ami_program.c"
    "${AMI_API_SRC_DIR}/ami_sensor.c"
)

target_include_directories(
    ami

    PUBLIC
        ${AMI_API_INCLUDE_DIR}
        # AMI_API_SRC_DIR is exposed as PUBLIC rather than PRIVATE so that vrtd can
        # include ami_ioctl.h and ami_device_internal.h directly.  This is necessary
        # because ami_prog_device_boot() (the natural public API for setting the boot
        # partition) internally calls ami_dev_hot_reset(), which performs its own
        # remove-device / toggle-SBR / rescan sequence by opening the PCI config-space
        # sysfs file (O_RDWR on /sys/bus/pci/devices/<port>/config).  That file is
        # mode 0600 and owned by root, so the open fails with EBADF when vrtd runs as
        # the unprivileged 'vrtd' user.  Even if we granted the capability required to
        # open it, ami_dev_hot_reset would still conflict with vrtd's own hotplug
        # reset sequence (slash_hotplug_remove / slash_hotplug_toggle_sbr /
        # slash_hotplug_rescan), resulting in the device being reset twice.
        #
        # The correct behaviour for vrtd is to issue the AMI_IOC_DEVICE_BOOT ioctl
        # only (to set the AMC firmware boot partition), and then let vrtd manage the
        # full hotplug reset itself.  There is no public AMI API that does just the
        # ioctl without the embedded hot-reset, so we call it directly from reset.c
        # using the internal headers.  The AMI library is a static library consumed
        # exclusively by vrtd, so widening the include visibility has no impact on
        # other consumers.
        ${AMI_API_SRC_DIR}
)

add_library(ami::ami ALIAS ami)
