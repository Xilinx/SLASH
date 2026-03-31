/**
 * The MIT License (MIT)
 * Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
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
 * @file clock.h
 * @brief AXI clock wizard driver for SLASH FPGA devices.
 *
 * The V80 FPGA exposes two independent clock wizard regions inside PCI BAR4:
 *   - @b User region (offset 0x00000000): controls the user-logic clock.
 *   - @b Service region (offset 0x00010000): controls the service/infrastructure clock.
 *
 * Each clock wizard is an AXI-mapped Xilinx Clocking Wizard IP core that
 * provides a programmable output frequency derived from a fixed primary
 * input clock.  The driver computes optimal MMCM divider settings (M, D, O)
 * to achieve the requested frequency with minimum error.
 */

#ifndef VRTD_CLOCK_H
#define VRTD_CLOCK_H

#include <stddef.h>
#include <stdint.h>

#include <slash/ctldev.h>

/** @brief PCI BAR index where the clock wizard registers are located. */
// BAR index used by the clock driver.
#define CLOCK_DRIVER_BAR_NUMBER 4

/**
 * @name Clock wizard region offsets within BAR4.
 * Each region contains the AXI register set for one clock wizard instance.
 * @{
 */
/** @brief Register offset of the user-region clock wizard inside BAR4. */
// Clock wizard register windows inside BAR4.
#define CLOCK_DRIVER_USER_REGION_WIZARD_OFFSET 0x00000000u
/** @brief Register offset of the service-region clock wizard inside BAR4. */
#define CLOCK_DRIVER_SERVICE_REGION_WIZARD_OFFSET 0x00010000u
/** @} */

/** @brief Output clock index within each wizard (clk_out1 = index 0). */
// Each wizard exposes clk_out1 as output index 0.
#define CLOCK_DRIVER_WIZARD_CLKOUT_ID 0u

/**
 * @brief Clock driver state for one SLASH FPGA device.
 *
 * Manages the memory-mapped AXI clock wizard register region and caches
 * the MMCM parameters used to synthesize the current output frequency.
 */
struct clock_driver {
    /** @brief libslash control device handle (non-owning, borrowed from struct device). */
    struct slash_ctldev *ctl; /* non-owning */
    /** @brief Memory-mapped BAR4 file handle (owning, opened by the clock driver). */
    struct slash_bar_file *bar; /* owning */
    /** @brief Pointer to the memory-mapped clock wizard AXI register bank (volatile for MMIO). */
    volatile uint32_t *regs;
    /** @brief Length in bytes of the mapped register region. */
    size_t len;
    /** @brief Primary input clock frequency in Hz (fixed, read from hardware). */
    uint32_t prim_in_hz;
    /** @brief MMCM feedback multiplier (M value in the clocking equation). */
    uint32_t m;
    /** @brief MMCM input divider (D value in the clocking equation). */
    uint32_t d;
    /** @brief MMCM output divider (O value in the clocking equation).
     *  Output frequency = prim_in_hz * M / (D * O). */
    uint32_t o;
    /** @brief Minimum frequency error in Hz achieved by the current M/D/O settings. */
    uint32_t min_err_hz;
};

/**
 * @brief Create and initialize a clock driver for the given device.
 *
 * Opens BAR4, maps the register region, and reads the primary input
 * clock frequency from hardware.
 *
 * @param ctl libslash control device handle (borrowed, must outlive the clock_driver).
 * @return Heap-allocated clock_driver on success, NULL on failure.
 */
struct clock_driver *clock_driver_create(struct slash_ctldev *ctl);

/**
 * @brief Release all resources owned by the clock driver (BAR mapping).
 * @param clk Pointer to the clock_driver to clean up. May be NULL (no-op).
 */
void cleanup_clock_driver(struct clock_driver *clk);

/**
 * @brief Cleanup helper for use with __attribute__((cleanup)).
 * @param clkp Address of a @c struct @c clock_driver pointer.
 */
static inline
void cleanup_clock_driverp(struct clock_driver **clkp)
{
    cleanup_clock_driver(*clkp);
    *clkp = NULL;
}

/**
 * @brief Read the current service-region clock frequency.
 * @param clk          The clock driver instance.
 * @param[out] rate_hz_out Receives the current frequency in Hz.
 * @return 0 on success, -1 on error.
 */
int clock_driver_get_service_region_rate_hz(struct clock_driver *clk, uint32_t *rate_hz_out);

/**
 * @brief Set the service-region clock to the requested frequency.
 *
 * The driver finds the closest achievable frequency by searching MMCM
 * divider space.  On return, @p rate_hz_inout is updated to the actual
 * frequency achieved (which may differ slightly from the request).
 *
 * @param clk              The clock driver instance.
 * @param[in,out] rate_hz_inout On entry, the desired frequency in Hz.
 *                              On exit, the actual frequency achieved.
 * @return 0 on success, -1 on error.
 */
int clock_driver_set_service_region_rate_hz(struct clock_driver *clk, uint32_t *rate_hz_inout);

/**
 * @brief Read the current user-region clock frequency.
 * @param clk          The clock driver instance.
 * @param[out] rate_hz_out Receives the current frequency in Hz.
 * @return 0 on success, -1 on error.
 */
int clock_driver_get_user_region_rate_hz(struct clock_driver *clk, uint32_t *rate_hz_out);

/**
 * @brief Set the user-region clock to the requested frequency.
 *
 * Behaves identically to @c clock_driver_set_service_region_rate_hz but
 * targets the user-region clock wizard.
 *
 * @param clk              The clock driver instance.
 * @param[in,out] rate_hz_inout On entry, the desired frequency in Hz.
 *                              On exit, the actual frequency achieved.
 * @return 0 on success, -1 on error.
 */
int clock_driver_set_user_region_rate_hz(struct clock_driver *clk, uint32_t *rate_hz_inout);

#endif // VRTD_CLOCK_H
