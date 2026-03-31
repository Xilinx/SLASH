/**
 * Copyright (C) 2025 Advanced Micro Devices, Inc. All rights reserved.
 * This program is free software; you can redistribute it and/or modify it under the terms of the
 * GNU General Public License as published by the Free Software Foundation; version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY; without
 * even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along with this program; if
 * not, write to the Free Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA.
 */

/**
 * @file slash.h
 *
 * Umbrella include for the SLASH kernel module.
 *
 * Pulls in the build-time configuration (PCI IDs, naming, log format)
 * and the user-kernel ABI definitions (ioctl structs and command
 * numbers) so that every driver source file can include a single
 * header for the common definitions.
 */

#ifndef SLASH_H
#define SLASH_H

#include "slash_config.h"
#include "slash_interface.h"

#endif /* SLASH_H */
