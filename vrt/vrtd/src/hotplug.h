/**
 * The MIT License (MIT)
 * Copyright (c) 2026 Advanced Micro Devices, Inc. All rights reserved.
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy of this software
 * and associated documentation files (the "Software"), to deal in the Software without restriction,
 * including without limitation the rights to use, copy, modify, merge, publish, distribute,
 * sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all copies or
 * substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
 * NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

#ifndef VRTD_HOTPLUG_H
#define VRTD_HOTPLUG_H

#include <slash/hotplug.h>
#include <vrtd/wire.h>

// Hotplug is a single device so it can be a global.
extern struct slash_hotplug *g_hotplug;

void hotplug_global_init(void);
void hotplug_global_destroy(void);

uint16_t hotplug_errno_to_vrtd_ret(int err);

// Helper function but useful and generally used with hotplug
int pci_bdf_set_function(const char *bdf, uint8_t func, char out_bdf[VRTD_PCI_BDF_LEN]);

// Extract BDF prefix (bus:device without function), e.g. "0000:65:00.2" -> "0000:65:00"
int pci_bdf_prefix(const char *bdf, char out_prefix[VRTD_PCI_BDF_LEN]);

#endif
