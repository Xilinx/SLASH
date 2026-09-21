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

/**
 * @file shell_build_id.h
 * @brief Shell build-ID register: the shell variant as reported by hardware.
 *
 * The static shell instantiates an AXI GPIO holding a build-time constant that
 * identifies the bitstream: the leading 60 bits of the source commit hash, a
 * dirty-tree flag, and — the field this module exists for — the shell variant.
 * Each shell's @c create_project.tcl forces the variant bit to its own value,
 * so the bit is correct even for bitstreams built outside the linker, and it is
 * the only shell identity that comes from the device rather than from vrtd's
 * own bookkeeping.
 *
 * The register layout matches the reader in @c smi/src/shell_build_id.hpp.
 */

#ifndef VRTD_SHELL_BUILD_ID_H
#define VRTD_SHELL_BUILD_ID_H

#include <stdint.h>

#include <slash/ctldev.h>

#include "vrtd/wire.h"

/** @brief PCI BAR index carrying the static-region build-ID GPIO. */
#define BUILD_ID_BAR_NUMBER 4

/**
 * @brief Offset of the build-ID AXI GPIO within BAR4.
 *
 * Its block-design address is 0x0204_0002_0000 and BAR4 maps the
 * 0x0204_0000_0000 aperture.
 */
#define BUILD_ID_OFFSET 0x00020000u

/** @brief AXI GPIO channel-1 data register: low 32 bits of the commit prefix. */
#define BUILD_ID_REG_LO (BUILD_ID_OFFSET + 0x0u)

/**
 * @brief AXI GPIO channel-2 data register.
 *
 * bits[27:0] high hash bits, bit[28] shell variant, bits[30:29] reserved,
 * bit[31] dirty flag.
 */
#define BUILD_ID_REG_HI (BUILD_ID_OFFSET + 0x8u)

/** @brief Shell-variant flag within the high word: 0 = service, 1 = compute. */
#define BUILD_ID_HI_SHELL_MASK 0x10000000u

/** @brief Reserved bits within the high word; every shell leaves them clear. */
#define BUILD_ID_HI_RESERVED_MASK 0x60000000u

/**
 * @brief Decode the shell variant from the build-ID high word.
 *
 * @param hi Value read from @c BUILD_ID_REG_HI.
 * @return @c VRTD_SHELL_COMPUTE or @c VRTD_SHELL_SERVICE for a well-formed
 *         word, or @c VRTD_SHELL_UNKNOWN when the word cannot have come from a
 *         build-ID register: all bits set (the pattern an MMIO read returns
 *         when it does not reach the device) or any reserved bit set.
 */
enum vrtd_shell_type build_id_decode_shell(uint32_t hi);

/**
 * @brief Read the shell variant the device reports for itself.
 *
 * @param bar Open mapping of BAR4. May be NULL.
 * @return The decoded variant, or @c VRTD_SHELL_UNKNOWN if the BAR is absent,
 *         too small to contain the register, or does not respond.
 */
enum vrtd_shell_type build_id_read_shell(const struct slash_bar_file *bar);

/**
 * @brief Verify that hardware reports the shell vrtd expects it to be running.
 *
 * A no-op returning success when @p expected is @c VRTD_SHELL_UNKNOWN: there is
 * then no claim to contradict. Otherwise any disagreement — including a
 * build-ID register that does not respond — is an error, because the caller has
 * asserted a specific shell and the device is not confirming it.
 *
 * @param bar      Open mapping of BAR4. May be NULL.
 * @param expected The shell vrtd believes is loaded.
 * @param context  Short caller identifier used as the log prefix.
 * @return 0 if hardware agrees (or there was nothing to check), -1 otherwise
 *         with @c errno set to @c EIO.
 */
int build_id_check_shell(
    const struct slash_bar_file *bar,
    enum vrtd_shell_type expected,
    const char *context
);

/** @brief Lowercase name of a shell variant, for log messages. */
const char *build_id_shell_name(enum vrtd_shell_type shell);

#endif // VRTD_SHELL_BUILD_ID_H
