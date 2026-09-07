// ################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//  and associated documentation files (the "Software"), to deal in the Software without
//  restriction, including without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all copies or
//  substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ################################################################################################

#pragma once

// ─────────────────────────────────────────────────────────────────────────────
// PF2 control-device IOCTL ABI
// ─────────────────────────────────────────────────────────────────────────────
//
// slash_sysemu uses the OFFICIAL kernel/userspace UAPI header shipped with libslash
// (driver/libslash/include/slash/uapi/slash_interface.h, wired up via the
// SLASH_UAPI_INCLUDE_DIR CMake variable) for the PF2 control-device structs and
// command numbers.  Consuming the authoritative header — rather than mirroring
// the structs in slash_sysemu — guarantees the daemon's socket wire format stays
// byte-for-byte identical with what libslash and the real driver use, and that
// any future ABI change is picked up automatically.
//
// The header provides:
//   * struct slash_ioctl_bar_info                (GET_BAR_INFO payload)
//   * struct slash_ioctl_bar_fd_request          (GET_BAR_FD payload)
//   * struct slash_ioctl_device_info             (GET_DEVICE_INFO payload)
//   * SLASH_CTLDEV_IOCTL_GET_BAR_INFO   (_IOWR('v', 0x30, ...))
//   * SLASH_CTLDEV_IOCTL_GET_BAR_FD     (_IOWR('v', 0x31, ...))
//   * SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO(_IOWR('v', 0x32, ...))
//   * SLASH_PCI_BDF_LEN
//
// This wrapper adds only the slash_sysemu-specific PF2 identity constants and
// convenient C++ aliases for the command numbers (the raw macros are unsigned
// long from _IOWR; the socket header carries a uint32_t ioctl_op).

#include <slash/uapi/slash_interface.h>

#include <cstdint>

namespace slash_sysemu {

// Command numbers as uint32_t, matching slash_sysemu_socket_header::ioctl_op.  The
// _IOWR macro yields the same 32-bit encoding libslash uses to dispatch ioctls.
inline constexpr uint32_t kSlashCtldevIoctlGetBarInfo =
    static_cast<uint32_t>(SLASH_CTLDEV_IOCTL_GET_BAR_INFO);
inline constexpr uint32_t kSlashCtldevIoctlGetBarFd =
    static_cast<uint32_t>(SLASH_CTLDEV_IOCTL_GET_BAR_FD);
inline constexpr uint32_t kSlashCtldevIoctlGetDeviceInfo =
    static_cast<uint32_t>(SLASH_CTLDEV_IOCTL_GET_DEVICE_INFO);

// PF2 PCI identity constants (returned by GET_DEVICE_INFO).
inline constexpr uint16_t kPf2VendorId          = 0x10EE; // AMD/Xilinx
inline constexpr uint16_t kPf2DeviceId          = 0x50B6;
inline constexpr uint16_t kPf2SubsystemVendorId = 0x10EE;
inline constexpr uint16_t kPf2SubsystemDeviceId = 0x000e;

} // namespace slash_sysemu
