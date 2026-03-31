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
 * @file clock.c
 * @brief Xilinx Clocking Wizard IP configuration via AXI memory-mapped registers.
 *
 * This module drives the Xilinx Clocking Wizard (MMCM/PLL) IP cores on the
 * AMD Alveo V80 FPGA. The clock wizards are accessed through BAR4 of the PCI
 * device, with two independent wizard instances mapped at different offsets:
 *
 *   - USER region wizard   at BAR4 + 0x00000000  (user-logic clock domain)
 *   - SERVICE region wizard at BAR4 + 0x00010000  (infrastructure/service clock domain)
 *
 * Each wizard instance exposes AXI-lite registers for reading the current
 * clock configuration and programming new multiplier (M), divider (D), and
 * output divider (O) values. The frequency synthesis formula is:
 *
 *   f_VCO  = f_primary_in * M / D
 *   f_out  = f_VCO / O_effective
 *
 * where f_primary_in is the reference clock (100 MHz by default) and
 * O_effective accounts for the half-cycle and edge encoding of the output
 * divider register fields.
 *
 * The overall flow for setting a new clock rate is:
 *   1. Generate candidate (M, D, O) tuples that produce frequencies close
 *      to the target, subject to VCO range constraints (2160-4320 MHz).
 *   2. Sort candidates by frequency error, then by output divider quality
 *      (divisible-by-4 preferred, then even, then higher VCO for jitter).
 *   3. For each candidate in order, program the M/D/O registers into the
 *      clock wizard, write common tail configuration, and trigger
 *      reconfiguration.
 *   4. Poll the lock status register until the PLL/MMCM locks or a
 *      timeout expires.
 *   5. On lock, read back the achieved frequency and return it to the caller.
 *
 * Register offsets and bit field definitions are derived from the Xilinx
 * xclk_wiz_hw.h header. Hardware-specific magic values are marked with
 * TODO comments for future documentation.
 */

#include "clock.h"

#include <inttypes.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <syslog.h>
#include <time.h>
#include <unistd.h>

#include <systemd/sd-journal.h>

#include "utils.h"

/*
 * Xilinx Clocking Wizard AXI register offsets.
 * Sourced from xclk_wiz_hw.h in the Xilinx driver headers.
 */
#define XCLK_WIZ_RECONFIG_OFFSET 0x00000014u  /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG1_OFFSET     0x00000330u  /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG2_OFFSET     0x00000334u  /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG3_OFFSET     0x00000338u  /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG4_OFFSET     0x0000033Cu  /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG11_OFFSET    0x00000378u  /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG12_OFFSET    0x00000380u  /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG13_OFFSET    0x00000384u  /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG14_OFFSET    0x00000398u  /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG15_OFFSET    0x0000039Cu  /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG16_OFFSET    0x000003A0u  /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG17_OFFSET    0x000003A8u  /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG19_OFFSET    0x000003CCu  /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG25_OFFSET    0x000003F0u  /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG26_OFFSET    0x000003FCu  /* TODO(vserbu): explain this register offset/bit field */

/*
 * Bit masks and shift constants for clock wizard register fields.
 */
#define XCLK_WIZ_LOCK               0x1u       /* Lock status bit in REG4 */
#define XCLK_WIZ_RECONFIG_LOAD      0x1u       /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_RECONFIG_SADDR     0x2u       /* TODO(vserbu): explain this register offset/bit field */

#define XCLK_WIZ_REG1_EDGE_MASK     0x100u     /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_CLKFBOUT_L_MASK    0xFFu      /* Low byte: low-time count for feedback divider */
#define XCLK_WIZ_CLKFBOUT_H_MASK    0xFF00u    /* High byte: high-time count for feedback divider */
#define XCLK_WIZ_CLKFBOUT_H_SHIFT   8u

#define XCLK_WIZ_EDGE_MASK          (1u << 10) /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_P5EN_MASK          (1u << 8)  /* TODO(vserbu): explain this register offset/bit field */

#define XCLK_WIZ_REG3_PREDIV2       (1u << 11) /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG3_USED          (1u << 12) /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG3_MX            (1u << 9)  /* TODO(vserbu): explain this register offset/bit field */

#define XCLK_WIZ_REG1_PREDIV2       (1u << 12) /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG1_EN            (1u << 9)  /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG1_MX            (1u << 10) /* TODO(vserbu): explain this register offset/bit field */

#define XCLK_WIZ_CLKOUT0_P5EN_SHIFT    13u     /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_CLKOUT0_P5FEDGE_SHIFT 15u     /* TODO(vserbu): explain this register offset/bit field */
#define XCLK_WIZ_REG12_EDGE_SHIFT      10u     /* TODO(vserbu): explain this register offset/bit field */

#define XCLK_MHZ 1000000ull

/*
 * Versal MMCM/PLL parameter limits.
 * M = feedback multiplier, D = input divider, O = output divider.
 * VCO frequency must stay within [VCO_MIN, VCO_MAX] MHz.
 */
#define XCLK_M_MIN 4u
#define XCLK_M_MAX 432u
#define XCLK_D_MIN 1u
#define XCLK_D_MAX 123u
#define XCLK_VCO_MIN 2160u    /* Minimum VCO frequency in MHz */
#define XCLK_VCO_MAX 4320u    /* Maximum VCO frequency in MHz */
#define XCLK_O_MIN 2u
#define XCLK_O_MAX 511u

/* Default configuration values for the clock driver. */
#define CLOCK_DRIVER_DEFAULT_PRIM_IN_HZ 100000000u  /* 100 MHz reference clock */
#define CLOCK_DRIVER_DEFAULT_MIN_ERR_HZ 500000u     /* 0.5 MHz acceptable error */
#define CLOCK_DRIVER_DEFAULT_MAX_CANDIDATES 50u     /* Max M/D/O tuples to evaluate */
#define CLOCK_DRIVER_DEFAULT_O_WINDOW 6u            /* Search window around estimated O */
#define CLOCK_DRIVER_DEFAULT_LOCK_TIMEOUT_MS 200u   /* PLL lock polling timeout */

/**
 * Initialize a clock driver struct, opening the BAR4 mapping for register access.
 *
 * @param clk  Pre-allocated clock_driver struct to initialize.
 * @param ctl  Opened libslash control device (non-owning reference stored).
 * @return 0 on success, -1 on error (errno set).
 */
static int clock_driver_init(struct clock_driver *clk, struct slash_ctldev *ctl)
{
    if (clk == NULL || ctl == NULL) {
        errno = EINVAL;
        return -1;
    }

    *clk = (struct clock_driver) {
        .ctl = ctl,
        .bar = NULL,
        .regs = NULL,
        .len = 0,
        .prim_in_hz = CLOCK_DRIVER_DEFAULT_PRIM_IN_HZ,
        .m = 0,
        .d = 0,
        .o = 0,
        .min_err_hz = CLOCK_DRIVER_DEFAULT_MIN_ERR_HZ,
    };

    /* Open BAR4 which contains the clock wizard register windows. */
    clk->bar = slash_bar_file_open(ctl, CLOCK_DRIVER_BAR_NUMBER, O_CLOEXEC);
    if (clk->bar == NULL) {
        return -1;
    }

    /* Map the BAR into the process address space for direct MMIO register access. */
    clk->regs = (volatile uint32_t *) clk->bar->map;
    clk->len = clk->bar->len;
    if (clk->regs == NULL || clk->len == 0) {
        return -1;
    }

    return 0;
}

/**
 * Allocate and initialize a clock driver instance.
 *
 * Opens BAR4 on the device referenced by @p ctl and prepares the driver
 * for clock frequency queries and programming.
 *
 * @param ctl  Opened libslash control device.
 * @return Heap-allocated clock_driver on success, NULL on error (logged).
 *         Caller must free with cleanup_clock_driver().
 */
struct clock_driver *clock_driver_create(struct slash_ctldev *ctl)
{
    struct clock_driver *clk = calloc(1, sizeof(*clk));
    if (clk == NULL) {
        LOG(LOG_ERR, "Failed to allocate clock driver: %m");
        return NULL;
    }

    if (clock_driver_init(clk, ctl) != 0) {
        LOG(LOG_ERR, "Failed to initialize clock driver: %m");
        cleanup_clock_driver(clk);
        return NULL;
    }

    return clk;
}

/**
 * Destroy a clock driver, closing its BAR mapping and freeing memory.
 *
 * Safe to call with NULL (no-op). The ctl pointer is non-owning and
 * is simply cleared without being closed.
 *
 * @param clk  Clock driver to destroy, or NULL.
 */
void cleanup_clock_driver(struct clock_driver *clk)
{
    if (clk == NULL) {
        return;
    }

    if (clk->bar != NULL) {
        (void) slash_bar_file_close(clk->bar);
        clk->bar = NULL;
    }

    clk->regs = NULL;
    clk->len = 0;
    clk->ctl = NULL;

    free(clk);
}

/**
 * Validate that a 32-bit register access at @p offset is within the
 * mapped BAR region.
 *
 * @return 0 if in bounds, -1 with errno set on error.
 */
static int clock_driver_check_bounds(const struct clock_driver *clk, uint32_t offset)
{
    if (clk == NULL || clk->regs == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (offset + sizeof(uint32_t) > clk->len) {
        errno = EOVERFLOW;
        return -1;
    }
    return 0;
}

/**
 * Read a 32-bit register from the clock wizard BAR mapping.
 *
 * @param clk     Clock driver with valid regs pointer.
 * @param offset  Byte offset into the BAR (converted to uint32_t index internally).
 * @return The 32-bit register value.
 */
static inline uint32_t clock_driver_r32(struct clock_driver *clk, uint32_t offset)
{
    return clk->regs[offset / sizeof(uint32_t)];
}

/**
 * Write a 32-bit value to a clock wizard register in the BAR mapping.
 *
 * @param clk     Clock driver with valid regs pointer.
 * @param offset  Byte offset into the BAR.
 * @param value   Value to write.
 */
static inline void clock_driver_w32(struct clock_driver *clk, uint32_t offset, uint32_t value)
{
    clk->regs[offset / sizeof(uint32_t)] = value;
}

/**
 * Compute the absolute BAR offset for a register within a given clock
 * wizard instance.
 *
 * @param wizard_offset  Base offset of the wizard (USER or SERVICE region).
 * @param reg_offset     Register offset within the wizard.
 * @return Combined byte offset into the BAR.
 */
static inline uint32_t clock_driver_reg(uint32_t wizard_offset, uint32_t reg_offset)
{
    return wizard_offset + reg_offset;
}

/**
 * Check that the entire register range of a clock wizard instance
 * (up through REG26, the highest used offset) fits within the BAR mapping.
 */
static int clock_driver_check_wizard_bounds(const struct clock_driver *clk, uint32_t wizard_offset)
{
    return clock_driver_check_bounds(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG26_OFFSET));
}

/**
 * Read the current VCO frequency from the clock wizard registers.
 *
 * Computes VCO frequency as:
 *   f_VCO = f_primary_in * M / D
 *
 * where M (multiplier) is decoded from REG1/REG2 (feedback path) and
 * D (input divider) is decoded from REG12/REG13.
 *
 * The multiplier and divider are each encoded as a pair of low-time and
 * high-time counts plus an edge bit:
 *   effective_value = low_count + high_count + edge_bit
 *
 * @param clk            Clock driver.
 * @param wizard_offset  Base offset of the wizard instance in BAR4.
 * @return VCO frequency in Hz.
 */
static uint64_t clock_driver_get_vco_hz(struct clock_driver *clk, uint32_t wizard_offset)
{
    /* Read the multiplier (M) from REG1 (edge bit) and REG2 (low/high counts). */
    uint32_t reg = clock_driver_r32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG1_OFFSET));
    uint32_t edge = (reg & XCLK_WIZ_REG1_EDGE_MASK) ? 1u : 0u;  /* TODO(vserbu): explain this register offset/bit field */

    reg = clock_driver_r32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG2_OFFSET));
    uint32_t low = reg & XCLK_WIZ_CLKFBOUT_L_MASK;
    uint32_t high = (reg & XCLK_WIZ_CLKFBOUT_H_MASK) >> XCLK_WIZ_CLKFBOUT_H_SHIFT;
    uint32_t mult = low + high + edge;
    if (mult == 0) {
        mult = 1;
    }

    /* Read the input divider (D) from REG13 (low/high counts) and REG12 (edge bit). */
    reg = clock_driver_r32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG13_OFFSET));
    low = reg & XCLK_WIZ_CLKFBOUT_L_MASK;
    high = (reg & XCLK_WIZ_CLKFBOUT_H_MASK) >> XCLK_WIZ_CLKFBOUT_H_SHIFT;

    reg = clock_driver_r32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG12_OFFSET));
    edge = (reg & XCLK_WIZ_EDGE_MASK) ? 1u : 0u;  /* TODO(vserbu): explain this register offset/bit field */

    uint32_t div = low + high + edge;
    if (div == 0) {
        div = 1;
    }

    /* f_VCO = f_primary_in * M / D */
    return ((uint64_t)clk->prim_in_hz * mult) / div;
}

/**
 * Read the current output clock rate for a specific clock output.
 *
 * Computes:
 *   f_out = f_VCO / O_effective
 *
 * The output divider (O) is read from a per-clock-output "leaf" register
 * pair. The effective output divider accounts for prediv2, p5en, and edge
 * encoding:
 *   O_effective = (prediv + 1) * (high + low + edge) + (prediv * p5en)
 *
 * @param clk            Clock driver.
 * @param wizard_offset  Base offset of the wizard instance.
 * @param clock_id       Output clock index (0-based). Outputs 0-2 use REG3-based
 *                       offsets; outputs 3+ use REG19-based offsets.
 * @return Output frequency in Hz.
 */
static uint64_t clock_driver_get_rate_hz(struct clock_driver *clk, uint32_t wizard_offset, uint32_t clock_id)
{
    uint64_t fvco = clock_driver_get_vco_hz(clk, wizard_offset);

    /*
     * Compute the register offset for this clock output's leaf divider.
     * Clock outputs 0-2 are packed starting at REG3 (8 bytes apart);
     * clock outputs 3+ start at REG19 (also 8 bytes apart).
     */
    uint32_t leaf_off = (clock_id < 3)
        ? (XCLK_WIZ_REG3_OFFSET + clock_id * 8u)
        : (XCLK_WIZ_REG19_OFFSET + clock_id * 8u);
    uint32_t reg_off = clock_driver_reg(wizard_offset, leaf_off);

    /* First register of the leaf pair: edge, p5en, prediv2 flags. */
    uint32_t reg = clock_driver_r32(clk, reg_off);
    uint32_t edge = (reg & (1u << XCLK_WIZ_CLKOUT0_P5FEDGE_SHIFT)) ? 1u : 0u;  /* TODO(vserbu): explain this register offset/bit field */
    uint32_t p5en = (reg & XCLK_WIZ_P5EN_MASK) ? 1u : 0u;   /* TODO(vserbu): explain this register offset/bit field */
    uint32_t prediv = (reg & XCLK_WIZ_REG3_PREDIV2) ? 1u : 0u;  /* TODO(vserbu): explain this register offset/bit field */

    /* Second register of the leaf pair: low-time and high-time counts. */
    uint32_t reg2 = clock_driver_r32(clk, reg_off + 4u);
    uint32_t low = reg2 & XCLK_WIZ_CLKFBOUT_L_MASK;
    uint32_t high = (reg2 & XCLK_WIZ_CLKFBOUT_H_MASK) >> XCLK_WIZ_CLKFBOUT_H_SHIFT;

    /*
     * Decode the effective output divider from the register fields.
     * leaf = high_count + low_count + edge
     * divo = (prediv + 1) * leaf + (prediv * p5en)
     */
    uint32_t leaf = high + low + edge;
    uint32_t divo = (prediv + 1u) * leaf + (prediv * p5en);
    if (divo == 0) {
        divo = 1;
    }

    return fvco / divo;
}

/**
 * Compute the effective output divider that will be produced by
 * programming a given O value into the clock wizard registers.
 *
 * This mirrors the encoding logic in clock_driver_update_o(): the O value
 * is decomposed into high_time, edge, and p5en fields, then the effective
 * divider is reconstructed as it would be read back from hardware:
 *   high_time = O / 4
 *   edge      = O % 2
 *   p5en      = (O % 4 <= 1) ? 0 : 1
 *   leaf      = high_time * 2 + edge
 *   divo      = 2 * leaf + p5en
 *
 * @param o  Raw output divider value (clamped to XCLK_O_MAX).
 * @return Effective divider ratio.
 */
static uint32_t clock_driver_effective_divo_from_o(uint32_t o)
{
    if (o > XCLK_O_MAX) {
        o = XCLK_O_MAX;
    }

    uint32_t high_time = o / 4u;
    uint32_t edge = o % 2u;
    uint32_t p5en = ((o % 4u) <= 1u) ? 0u : 1u;
    uint32_t leaf = (high_time * 2u) + edge;
    uint32_t divo = (2u * leaf) + p5en;
    if (divo == 0) {
        divo = 1u;
    }

    return divo;
}

/**
 * Log the current state of all relevant clock wizard registers for debugging.
 *
 * Reads and logs the multiplier, divider, leaf output, status, and
 * reconfiguration registers, along with computed VCO and output frequencies.
 *
 * @param clk            Clock driver.
 * @param wizard_offset  Base offset of the wizard instance.
 * @param clock_id       Output clock index being inspected.
 * @param stage          Human-readable label for the log entry (e.g., "before_program").
 */
static void clock_driver_log_state(
    struct clock_driver *clk,
    uint32_t wizard_offset,
    uint32_t clock_id,
    const char *stage
)
{
    uint32_t leaf_off = (clock_id < 3)
        ? (XCLK_WIZ_REG3_OFFSET + clock_id * 8u)
        : (XCLK_WIZ_REG19_OFFSET + clock_id * 8u);
    uint32_t leaf_reg_off = clock_driver_reg(wizard_offset, leaf_off);

    uint32_t reg1 = clock_driver_r32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG1_OFFSET));
    uint32_t reg2 = clock_driver_r32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG2_OFFSET));
    uint32_t reg12 = clock_driver_r32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG12_OFFSET));
    uint32_t reg13 = clock_driver_r32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG13_OFFSET));
    uint32_t leaf0 = clock_driver_r32(clk, leaf_reg_off);
    uint32_t leaf1 = clock_driver_r32(clk, leaf_reg_off + 4u);
    uint32_t status = clock_driver_r32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG4_OFFSET));
    uint32_t reconfig = clock_driver_r32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_RECONFIG_OFFSET));

    uint64_t fvco_hz = clock_driver_get_vco_hz(clk, wizard_offset);
    uint64_t rate_hz = clock_driver_get_rate_hz(clk, wizard_offset, clock_id);

    LOG(
        LOG_INFO,
        "clock_driver[%s]: wiz=0x%08x clk=%u prim_in_hz=%u fvco_hz=%" PRIu64
        " rate_hz=%" PRIu64 " status=0x%08x reconfig=0x%08x reg1=0x%08x reg2=0x%08x"
        " reg12=0x%08x reg13=0x%08x leaf0=0x%08x leaf1=0x%08x",
        stage,
        wizard_offset,
        clock_id,
        clk->prim_in_hz,
        fvco_hz,
        rate_hz,
        status,
        reconfig,
        reg1,
        reg2,
        reg12,
        reg13,
        leaf0,
        leaf1
    );
}

/**
 * Program the output divider (O) registers for a specific clock output.
 *
 * Encodes clk->o into the leaf register pair for clock_id:
 *   - First register: control flags (PREDIV2, USED, MX) and edge/p5 encoding.
 *   - Second register: high_time in both low and high bytes.
 *
 * @param clk            Clock driver with clk->o set to the desired O value.
 * @param wizard_offset  Base offset of the wizard instance.
 * @param clock_id       Output clock index.
 */
static void clock_driver_update_o(struct clock_driver *clk, uint32_t wizard_offset, uint32_t clock_id)
{
    uint32_t o = clk->o;
    if (o > XCLK_O_MAX) {
        o = XCLK_O_MAX;
    }

    /* Compute register offset for this clock output's leaf divider pair. */
    uint32_t leaf_off = (clock_id < 3)
        ? (XCLK_WIZ_REG3_OFFSET + clock_id * 8u)
        : (XCLK_WIZ_REG19_OFFSET + clock_id * 8u);
    uint32_t reg_off = clock_driver_reg(wizard_offset, leaf_off);

    /* Encode O into high_time, div_edge, p5_enable, and p5f_edge fields. */
    uint32_t high_time = o / 4u;
    uint32_t reg = XCLK_WIZ_REG3_PREDIV2 | XCLK_WIZ_REG3_USED | XCLK_WIZ_REG3_MX;  /* TODO(vserbu): explain this register offset/bit field */

    uint32_t div_edge = ((o % 4u) <= 1u) ? 0u : 1u;
    reg |= (div_edge << 8u);  /* TODO(vserbu): explain this register offset/bit field */

    uint32_t p5f_edge = o % 2u;
    uint32_t p5_enable = o % 2u;
    reg |= (p5_enable << XCLK_WIZ_CLKOUT0_P5EN_SHIFT) |
           (p5f_edge << XCLK_WIZ_CLKOUT0_P5FEDGE_SHIFT);  /* TODO(vserbu): explain this register offset/bit field */

    clock_driver_w32(clk, reg_off, reg);
    clock_driver_w32(clk, reg_off + 4u, (high_time | (high_time << 8u)));  /* TODO(vserbu): explain this register offset/bit field */
}

/**
 * Program the input divider (D) registers of the clock wizard.
 *
 * Encodes clk->d into REG12 (edge bit) and REG13 (high/low time counts).
 *
 * @param clk            Clock driver with clk->d set to the desired D value.
 * @param wizard_offset  Base offset of the wizard instance.
 */
static void clock_driver_update_d(struct clock_driver *clk, uint32_t wizard_offset)
{
    uint32_t d = clk->d;
    uint32_t high_time = d / 2u;

    uint32_t reg = 0;
    reg &= ~(1u << XCLK_WIZ_REG12_EDGE_SHIFT);  /* TODO(vserbu): explain this register offset/bit field */
    uint32_t div_edge = d % 2u;
    reg |= (div_edge << XCLK_WIZ_REG12_EDGE_SHIFT);

    clock_driver_w32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG12_OFFSET), reg);
    clock_driver_w32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG13_OFFSET), (high_time | (high_time << 8u)));  /* TODO(vserbu): explain this register offset/bit field */
}

/**
 * Program the feedback multiplier (M) registers of the clock wizard.
 *
 * Encodes clk->m into REG25 (clear), REG2 (high/low time counts), and
 * REG1 (control flags and edge bit).
 *
 * @param clk            Clock driver with clk->m set to the desired M value.
 * @param wizard_offset  Base offset of the wizard instance.
 */
static void clock_driver_update_m(struct clock_driver *clk, uint32_t wizard_offset)
{
    uint32_t m = clk->m;
    clock_driver_w32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG25_OFFSET), 0);  /* TODO(vserbu): explain this register offset/bit field */

    uint32_t div_edge = m % 2u;
    uint32_t high_time = m / 2u;
    clock_driver_w32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG2_OFFSET), (high_time | (high_time << 8u)));  /* TODO(vserbu): explain this register offset/bit field */

    uint32_t reg = XCLK_WIZ_REG1_PREDIV2 | XCLK_WIZ_REG1_EN | XCLK_WIZ_REG1_MX;  /* TODO(vserbu): explain this register offset/bit field */
    if (div_edge) {
        reg |= (1u << 8u);   /* TODO(vserbu): explain this register offset/bit field */
    } else {
        reg &= ~(1u << 8u);  /* TODO(vserbu): explain this register offset/bit field */
    }
    clock_driver_w32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG1_OFFSET), reg);
}

/**
 * Write the common tail registers required after programming M, D, and O.
 *
 * These register values are fixed/magic configuration needed by the Xilinx
 * Clocking Wizard to finalize a reconfiguration sequence. They configure
 * filter and lock detection parameters for the MMCM/PLL.
 *
 * @param clk            Clock driver.
 * @param wizard_offset  Base offset of the wizard instance.
 */
static void clock_driver_program_common_tail(struct clock_driver *clk, uint32_t wizard_offset)
{
    clock_driver_w32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG11_OFFSET), 0x2Eu);    /* TODO(vserbu): explain this register offset/bit field */
    clock_driver_w32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG14_OFFSET), 0xE80u);   /* TODO(vserbu): explain this register offset/bit field */
    clock_driver_w32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG15_OFFSET), 0x4271u);  /* TODO(vserbu): explain this register offset/bit field */
    clock_driver_w32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG16_OFFSET), 0x43E9u);  /* TODO(vserbu): explain this register offset/bit field */
    clock_driver_w32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG17_OFFSET), 0x001Cu);  /* TODO(vserbu): explain this register offset/bit field */
    clock_driver_w32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG26_OFFSET), 0x0001u);  /* TODO(vserbu): explain this register offset/bit field */
}

/**
 * Trigger the clock wizard's dynamic reconfiguration sequence.
 *
 * Writes the LOAD and SADDR bits to the reconfiguration register, which
 * causes the wizard to latch the newly programmed M/D/O values and begin
 * the PLL/MMCM re-lock process.
 *
 * @param clk            Clock driver.
 * @param wizard_offset  Base offset of the wizard instance.
 */
static void clock_driver_trigger_reconfig(struct clock_driver *clk, uint32_t wizard_offset)
{
    clock_driver_w32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_RECONFIG_OFFSET),
                     (XCLK_WIZ_RECONFIG_LOAD | XCLK_WIZ_RECONFIG_SADDR));
}

/**
 * Poll the clock wizard lock status register until the PLL/MMCM locks
 * or the timeout expires.
 *
 * Uses CLOCK_MONOTONIC to measure elapsed time. Polls with 100 us sleep
 * intervals between reads.
 *
 * @param clk            Clock driver.
 * @param wizard_offset  Base offset of the wizard instance.
 * @param timeout_ms     Maximum time to wait for lock, in milliseconds.
 * @return 0 if lock acquired, -1 on timeout (errno = ETIMEDOUT) or clock error.
 */
static int clock_driver_wait_for_lock(struct clock_driver *clk, uint32_t wizard_offset, uint32_t timeout_ms)
{
    struct timespec start;
    if (clock_gettime(CLOCK_MONOTONIC, &start) != 0) {
        return -1;
    }

    for (;;) {
        /* Check the LOCK bit in the status register (REG4). */
        if ((clock_driver_r32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG4_OFFSET)) & XCLK_WIZ_LOCK) != 0u) {
            return 0;
        }

        /* Compute elapsed time and check against timeout. */
        struct timespec now;
        if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
            return -1;
        }

        time_t sec = now.tv_sec - start.tv_sec;
        long nsec_signed = now.tv_nsec - start.tv_nsec;
        if (nsec_signed < 0) {
            sec -= 1;
            nsec_signed += 1000000000l;
        }

        uint64_t elapsed_ms = (uint64_t)sec * 1000ull;
        uint64_t nsec = (uint64_t)nsec_signed;
        elapsed_ms += nsec / 1000000ull;

        if (elapsed_ms > timeout_ms) {
            errno = ETIMEDOUT;
            return -1;
        }

        (void) usleep(100);
    }
}

/**
 * Program the M, D, and O values into the clock wizard, trigger
 * reconfiguration, and wait for PLL/MMCM lock.
 *
 * This is the core "apply configuration" routine. It writes all divider
 * registers, writes the common tail configuration, triggers reconfiguration,
 * then polls for lock.
 *
 * @param clk            Clock driver with m, d, o fields set.
 * @param wizard_offset  Base offset of the wizard instance.
 * @param clock_id       Output clock index.
 * @param timeout_ms     Lock timeout in milliseconds.
 * @param ok             Output: set to 0 on success, remains -1 on failure.
 * @return Achieved output frequency in Hz (only valid when *ok == 0).
 */
static uint64_t clock_driver_program_mdo_and_reconfig(
    struct clock_driver *clk,
    uint32_t wizard_offset,
    uint32_t clock_id,
    uint32_t timeout_ms,
    int *ok
)
{
    *ok = -1;

    clock_driver_w32(clk, clock_driver_reg(wizard_offset, XCLK_WIZ_REG25_OFFSET), 0);  /* TODO(vserbu): explain this register offset/bit field */

    /* Program output divider, input divider, and feedback multiplier. */
    clock_driver_update_o(clk, wizard_offset, clock_id);
    clock_driver_update_d(clk, wizard_offset);
    clock_driver_update_m(clk, wizard_offset);
    clock_driver_program_common_tail(clk, wizard_offset);

    /* Trigger the dynamic reconfiguration and wait for PLL lock. */
    clock_driver_trigger_reconfig(clk, wizard_offset);

    if (clock_driver_wait_for_lock(clk, wizard_offset, timeout_ms) != 0) {
        return 0;
    }

    *ok = 0;
    return clock_driver_get_rate_hz(clk, wizard_offset, clock_id);
}

/**
 * A candidate (M, D, O) tuple for clock frequency synthesis.
 *
 * Used during the search for the best divider configuration to reach a
 * target frequency. Candidates are ranked by frequency error, then by
 * output divider quality (divisible by 4, then even), then by VCO frequency.
 */
struct clock_candidate {
    uint64_t diff_hz;      /* Absolute frequency error: |achieved - target| */
    uint64_t achieved_hz;  /* Frequency this candidate produces */
    uint32_t m;            /* Feedback multiplier */
    uint32_t d;            /* Input divider */
    uint32_t o;            /* Output divider */
    uint64_t fvco_hz;      /* VCO frequency for this M/D combination */
};

/**
 * Compare two clock candidates for sorting.
 *
 * Ordering priority (best first):
 *   1. Smallest frequency error (diff_hz).
 *   2. O divisible by 4 (stable, easy-to-represent output divisors).
 *   3. Even O values (better duty cycle symmetry).
 *   4. Higher VCO frequency (better jitter performance).
 *   5. Smaller O value (tie-breaker).
 *
 * @return Negative if a < b, positive if a > b, 0 if equal.
 */
static int clock_driver_candidate_cmp(
    const struct clock_candidate *a,
    const struct clock_candidate *b
)
{
    if (a->diff_hz != b->diff_hz) {
        return (a->diff_hz < b->diff_hz) ? -1 : 1;
    }

    // Prefer o divisible by 4 (stable/easy-to-represent output divisors).
    uint32_t ao4 = (a->o % 4u == 0u) ? 0u : 1u;
    uint32_t bo4 = (b->o % 4u == 0u) ? 0u : 1u;
    if (ao4 != bo4) {
        return (ao4 < bo4) ? -1 : 1;
    }

    // Then prefer even o, then higher VCO for better jitter behavior.
    uint32_t ao2 = (a->o % 2u == 0u) ? 0u : 1u;
    uint32_t bo2 = (b->o % 2u == 0u) ? 0u : 1u;
    if (ao2 != bo2) {
        return (ao2 < bo2) ? -1 : 1;
    }

    if (a->fvco_hz != b->fvco_hz) {
        return (a->fvco_hz > b->fvco_hz) ? -1 : 1;
    }

    if (a->o != b->o) {
        return (a->o < b->o) ? -1 : 1;
    }

    return 0;
}

/**
 * Generate and rank candidate (M, D, O) tuples for a target frequency.
 *
 * Exhaustively searches over all valid M and D values. For each (M, D) pair
 * that produces a VCO within the allowed range [2160, 4320] MHz, it evaluates
 * O values in a window around the estimated optimal O. Candidates are
 * maintained in a bounded buffer (max_candidates), replacing the worst
 * candidate when the buffer is full and a better one is found.
 *
 * After generation, candidates are sorted best-first using
 * clock_driver_candidate_cmp() (selection sort).
 *
 * The frequency for each candidate is computed as:
 *   achieved_hz = (prim_in_hz * M / D) / O
 *
 * @param clk             Clock driver (provides prim_in_hz).
 * @param target_hz       Desired output frequency in Hz.
 * @param cands           Output array of candidates (caller-allocated).
 * @param max_candidates  Maximum number of candidates to retain.
 * @return Number of candidates generated (0 if none found).
 */
static size_t clock_driver_generate_candidates(
    struct clock_driver *clk,
    uint32_t target_hz,
    struct clock_candidate *cands,
    size_t max_candidates
)
{
    if (clk == NULL || target_hz == 0 || cands == NULL || max_candidates == 0) {
        return 0;
    }

    uint64_t vco_min_hz = (uint64_t)XCLK_VCO_MIN * XCLK_MHZ;
    uint64_t vco_max_hz = (uint64_t)XCLK_VCO_MAX * XCLK_MHZ;
    size_t count = 0;

    /* Iterate over all valid (M, D) pairs. */
    for (uint32_t m = XCLK_M_MIN; m <= XCLK_M_MAX; ++m) {
        uint64_t numerator = (uint64_t)clk->prim_in_hz * m;
        for (uint32_t d = XCLK_D_MIN; d <= XCLK_D_MAX; ++d) {
            uint64_t fvco_hz = numerator / d;

            /* Skip (M, D) pairs whose VCO falls outside allowed range. */
            if (fvco_hz < vco_min_hz || fvco_hz > vco_max_hz) {
                continue;
            }

            /*
             * Estimate the ideal O for this VCO, then search a window around it.
             * o_est = round(f_VCO / target_hz)
             */
            uint64_t o_est = (fvco_hz + target_hz / 2u) / target_hz;
            if (o_est < XCLK_O_MIN) {
                o_est = XCLK_O_MIN;
            } else if (o_est > XCLK_O_MAX) {
                o_est = XCLK_O_MAX;
            }

            uint32_t o_lo = (o_est > CLOCK_DRIVER_DEFAULT_O_WINDOW)
                ? (uint32_t)(o_est - CLOCK_DRIVER_DEFAULT_O_WINDOW)
                : XCLK_O_MIN;
            if (o_lo < XCLK_O_MIN) {
                o_lo = XCLK_O_MIN;
            }

            uint32_t o_hi = (uint32_t)(o_est + CLOCK_DRIVER_DEFAULT_O_WINDOW);
            if (o_hi > XCLK_O_MAX) {
                o_hi = XCLK_O_MAX;
            }

            for (uint32_t o = o_lo; o <= o_hi; ++o) {
                uint64_t achieved_hz = fvco_hz / o;
                uint64_t diff_hz = (achieved_hz > target_hz)
                    ? (achieved_hz - target_hz)
                    : (target_hz - achieved_hz);
                struct clock_candidate candidate = {
                    .diff_hz = diff_hz,
                    .achieved_hz = achieved_hz,
                    .m = m,
                    .d = d,
                    .o = o,
                    .fvco_hz = fvco_hz,
                };

                /* If buffer has room, just append. */
                if (count < max_candidates) {
                    cands[count++] = candidate;
                    continue;
                }

                /* Buffer full: replace the worst candidate if this one is better. */
                size_t worst_index = 0;
                for (size_t i = 1; i < count; ++i) {
                    if (clock_driver_candidate_cmp(&cands[worst_index], &cands[i]) < 0) {
                        worst_index = i;
                    }
                }
                if (clock_driver_candidate_cmp(&candidate, &cands[worst_index]) < 0) {
                    cands[worst_index] = candidate;
                }
            }
        }
    }

    /* Sort candidates best-first (selection sort). */
    for (size_t i = 0; i < count; ++i) {
        for (size_t j = i + 1; j < count; ++j) {
            if (clock_driver_candidate_cmp(&cands[j], &cands[i]) < 0) {
                struct clock_candidate tmp = cands[i];
                cands[i] = cands[j];
                cands[j] = tmp;
            }
        }
    }

    return count;
}

/**
 * Attempt to set the clock output to a target frequency.
 *
 * Generates candidate (M, D, O) tuples, then tries each in ranked order:
 * programs the wizard, triggers reconfiguration, and waits for lock. The
 * first candidate that achieves lock is accepted and its achieved frequency
 * is written back to *rate_hz_inout.
 *
 * @param clk            Clock driver.
 * @param wizard_offset  Base offset of the wizard instance (USER or SERVICE).
 * @param clock_id       Output clock index.
 * @param rate_hz_inout  On entry: target frequency in Hz. On success: achieved
 *                       frequency in Hz.
 * @return 0 on success, -1 on error (no valid candidates or all timed out).
 */
static int clock_driver_try_set_rate_hz(
    struct clock_driver *clk,
    uint32_t wizard_offset,
    uint32_t clock_id,
    uint32_t *rate_hz_inout
)
{
    if (clk == NULL || rate_hz_inout == NULL || *rate_hz_inout == 0) {
        errno = EINVAL;
        LOG(
            LOG_WARNING,
            "clock_driver: invalid set_rate arguments (clk=%p rate_ptr=%p rate_hz=%u)",
            (void *)clk,
            (void *)rate_hz_inout,
            (rate_hz_inout != NULL) ? *rate_hz_inout : 0u
        );
        return -1;
    }

    struct clock_candidate candidates[CLOCK_DRIVER_DEFAULT_MAX_CANDIDATES];
    size_t count = clock_driver_generate_candidates(
        clk,
        *rate_hz_inout,
        candidates,
        CLOCK_DRIVER_DEFAULT_MAX_CANDIDATES
    );
    if (count == 0) {
        errno = ERANGE;
        LOG(
            LOG_WARNING,
            "clock_driver: failed to calculate divisors for request_hz=%u: %m",
            *rate_hz_inout
        );
        return -1;
    }

    /* Try each candidate in priority order until one achieves PLL lock. */
    for (size_t i = 0; i < count; ++i) {
        const struct clock_candidate *cand = &candidates[i];
        clk->m = cand->m;
        clk->d = cand->d;
        clk->o = cand->o;

        uint64_t predicted_fvco_hz = ((uint64_t)clk->prim_in_hz * clk->m) / clk->d;
        uint32_t predicted_divo = clock_driver_effective_divo_from_o(clk->o);
        uint64_t predicted_rate_hz = predicted_fvco_hz / predicted_divo;
        LOG(
            LOG_INFO,
            "clock_driver: request_hz=%u trying candidate=%zu/%zu m=%u d=%u o=%u est_hz=%" PRIu64
            " diff_hz=%" PRIu64 " predicted_divo=%u predicted_rate_hz=%" PRIu64,
            *rate_hz_inout,
            i + 1u,
            count,
            clk->m,
            clk->d,
            clk->o,
            cand->achieved_hz,
            cand->diff_hz,
            predicted_divo,
            predicted_rate_hz
        );

        clock_driver_log_state(clk, wizard_offset, clock_id, "before_program");

        int ok = 0;
        uint64_t reported = clock_driver_program_mdo_and_reconfig(
            clk, wizard_offset, clock_id, CLOCK_DRIVER_DEFAULT_LOCK_TIMEOUT_MS, &ok
        );

        clock_driver_log_state(clk, wizard_offset, clock_id, "after_program");

        if (ok == 0) {
            LOG(
                LOG_INFO,
                "clock_driver: set_rate request_hz=%u reported_hz=%" PRIu64
                " m=%u d=%u o=%u candidate=%zu/%zu",
                *rate_hz_inout,
                reported,
                clk->m,
                clk->d,
                clk->o,
                i + 1u,
                count
            );
            *rate_hz_inout = (uint32_t)reported;
            return 0;
        }

        LOG(
            LOG_WARNING,
            "clock_driver: lock timeout request_hz=%u candidate=%zu/%zu m=%u d=%u o=%u timeout_ms=%u",
            *rate_hz_inout,
            i + 1u,
            count,
            clk->m,
            clk->d,
            clk->o,
            CLOCK_DRIVER_DEFAULT_LOCK_TIMEOUT_MS
        );
    }

    errno = ETIMEDOUT;
    return -1;
}

/**
 * Get the current clock frequency of the SERVICE region.
 *
 * The service region wizard is at BAR4 + 0x00010000. This reads the
 * output clock rate without modifying any registers.
 *
 * @param clk          Clock driver.
 * @param rate_hz_out  Receives the current service clock frequency in Hz.
 * @return 0 on success, -1 on error.
 */
int clock_driver_get_service_region_rate_hz(struct clock_driver *clk, uint32_t *rate_hz_out)
{
    if (rate_hz_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (clock_driver_check_wizard_bounds(clk, CLOCK_DRIVER_SERVICE_REGION_WIZARD_OFFSET) != 0) {
        return -1;
    }

    uint64_t rate = clock_driver_get_rate_hz(
        clk,
        CLOCK_DRIVER_SERVICE_REGION_WIZARD_OFFSET,
        CLOCK_DRIVER_WIZARD_CLKOUT_ID
    );
    *rate_hz_out = (uint32_t)rate;
    return 0;
}

/**
 * Set the clock frequency of the SERVICE region.
 *
 * Programs the service region clock wizard (BAR4 + 0x00010000) to produce
 * a frequency as close as possible to the requested rate.
 *
 * @param clk            Clock driver.
 * @param rate_hz_inout  On entry: desired frequency in Hz. On success:
 *                       actual achieved frequency in Hz.
 * @return 0 on success, -1 on error (logged).
 */
int clock_driver_set_service_region_rate_hz(struct clock_driver *clk, uint32_t *rate_hz_inout)
{
    if (clock_driver_check_wizard_bounds(clk, CLOCK_DRIVER_SERVICE_REGION_WIZARD_OFFSET) != 0) {
        LOG(LOG_ERR, "clock_driver: service region wizard bounds check failed: %m");
        return -1;
    }
    int ret = clock_driver_try_set_rate_hz(
        clk,
        CLOCK_DRIVER_SERVICE_REGION_WIZARD_OFFSET,
        CLOCK_DRIVER_WIZARD_CLKOUT_ID,
        rate_hz_inout
    );
    if (ret != 0) {
        LOG(
            LOG_WARNING,
            "clock_driver: failed to set service region frequency request_hz=%u: %m",
            (rate_hz_inout != NULL) ? *rate_hz_inout : 0u
        );
    }
    return ret;
}

/**
 * Get the current clock frequency of the USER region.
 *
 * The user region wizard is at BAR4 + 0x00000000. This reads the
 * output clock rate without modifying any registers.
 *
 * @param clk          Clock driver.
 * @param rate_hz_out  Receives the current user clock frequency in Hz.
 * @return 0 on success, -1 on error.
 */
int clock_driver_get_user_region_rate_hz(struct clock_driver *clk, uint32_t *rate_hz_out)
{
    if (rate_hz_out == NULL) {
        errno = EINVAL;
        return -1;
    }
    if (clock_driver_check_wizard_bounds(clk, CLOCK_DRIVER_USER_REGION_WIZARD_OFFSET) != 0) {
        return -1;
    }

    uint64_t rate = clock_driver_get_rate_hz(
        clk,
        CLOCK_DRIVER_USER_REGION_WIZARD_OFFSET,
        CLOCK_DRIVER_WIZARD_CLKOUT_ID
    );
    *rate_hz_out = (uint32_t)rate;
    return 0;
}

/**
 * Set the clock frequency of the USER region.
 *
 * Programs the user region clock wizard (BAR4 + 0x00000000) to produce
 * a frequency as close as possible to the requested rate.
 *
 * @param clk            Clock driver.
 * @param rate_hz_inout  On entry: desired frequency in Hz. On success:
 *                       actual achieved frequency in Hz.
 * @return 0 on success, -1 on error (logged).
 */
int clock_driver_set_user_region_rate_hz(struct clock_driver *clk, uint32_t *rate_hz_inout)
{
    if (clock_driver_check_wizard_bounds(clk, CLOCK_DRIVER_USER_REGION_WIZARD_OFFSET) != 0) {
        LOG(LOG_ERR, "clock_driver: user region wizard bounds check failed: %m");
        return -1;
    }
    int ret = clock_driver_try_set_rate_hz(
        clk,
        CLOCK_DRIVER_USER_REGION_WIZARD_OFFSET,
        CLOCK_DRIVER_WIZARD_CLKOUT_ID,
        rate_hz_inout
    );
    if (ret != 0) {
        LOG(
            LOG_WARNING,
            "clock_driver: failed to set user region frequency request_hz=%u: %m",
            (rate_hz_inout != NULL) ? *rate_hz_inout : 0u
        );
    }
    return ret;
}
