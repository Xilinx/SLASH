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
// Hotplug (daemon-level) IOCTL ABI
// ─────────────────────────────────────────────────────────────────────────────
//
// Like ctl_ioctls.h / qdma_ioctls.h, slash_emu consumes the OFFICIAL libslash
// UAPI header for the hotplug structs and command numbers rather than mirroring
// them, so the daemon's slash_hotplug socket wire format stays byte-for-byte
// identical with what libslash and the real driver use.
//
// The header provides:
//   * struct slash_hotplug_device_request { __u32 size; char bdf[SLASH_HOTPLUG_BDF_LEN]; }
//   * SLASH_HOTPLUG_IOCTL_RESCAN     (_IO ('w', 0x30))          — no argument
//   * SLASH_HOTPLUG_IOCTL_REMOVE     (_IOW('w', 0x31, request)) — targets a PF by bdf
//   * SLASH_HOTPLUG_IOCTL_TOGGLE_SBR (_IOW('w', 0x32, request)) — resets a bus
//   * SLASH_HOTPLUG_IOCTL_HOTPLUG    (_IOW('w', 0x33, request)) — remove + rescan
//   * SLASH_HOTPLUG_BDF_LEN, SLASH_HOTPLUG_IOCTL_MAGIC ('w')
//
// BDF targeting (interpreted by HotplugSubsystem::resolve_target for REMOVE /
// HOTPLUG / TOGGLE_SBR): the request's `bdf` is a board BDF ("DDDD:BB:DD") with an
// optional ".F" function suffix:
//   * ".2" → PF2 (slash_ctl), ".1" → PF1 (slash_qdma_ctl), ".0" → PF0 stub;
//   * a bare board BDF (no suffix) → board-level (all PFs of that accelerator);
//   * an empty bdf → the single tracked device (-EOPNOTSUPP if more than one is
//     tracked, -ENODEV if none) — matching the slash_hotplug.h convention.
// TOGGLE_SBR uses only the bus ("BB") field and fans out to every accelerator on
// that bus.

#include <slash/uapi/slash_hotplug.h>

#include <cstdint>

namespace slash_emu {

// Command numbers as uint32_t, matching slash_emu_socket_header::ioctl_op.  The
// _IO/_IOW macros yield the same 32-bit encoding libslash uses to dispatch ioctls
// (magic 'w', distinct from the 'v' used by the PF2/QDMA devices).
inline constexpr uint32_t kSlashHotplugIoctlRescan =
    static_cast<uint32_t>(SLASH_HOTPLUG_IOCTL_RESCAN);
inline constexpr uint32_t kSlashHotplugIoctlRemove =
    static_cast<uint32_t>(SLASH_HOTPLUG_IOCTL_REMOVE);
inline constexpr uint32_t kSlashHotplugIoctlToggleSbr =
    static_cast<uint32_t>(SLASH_HOTPLUG_IOCTL_TOGGLE_SBR);
inline constexpr uint32_t kSlashHotplugIoctlHotplug =
    static_cast<uint32_t>(SLASH_HOTPLUG_IOCTL_HOTPLUG);

} // namespace slash_emu
