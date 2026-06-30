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
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
 * BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
 * NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
 * DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
 */

/**
 * rp1_bringup_gpu — GPU-driven port of `rp1_bringup signal`.
 *
 * Imports PF2 BAR4 (the RP1 64 MiB DDR aperture) as a dma-buf into the
 * GPU via hsa_amd_interop_map_buffer, then dispatches a single-thread
 * HIP kernel that performs the entire RP1 protocol exchange:
 *
 *   - verify ctrl->magic == "SQR1"
 *   - stage a one-node SIGNAL graph at the node array
 *   - program the control block bases + node_count
 *   - bump graph_seq
 *   - poll graph_done_seq
 *   - read back signal slot 0 (expect 0xDEADBEEF)
 *
 * The host CPU writes nothing to BAR4 in the hot path. This validates
 * end-to-end that:
 *
 *   - the bitstream marks PF2 BAR4 prefetchable so
 *     pci_p2pdma_add_resource() succeeded at probe,
 *   - the GPU's IOMMU mapping reaches the FPGA,
 *   - RP1 firmware sees writes that originated on the GPU,
 *   - RP1's responses are visible back to the GPU.
 *
 * Usage:
 *   ./rp1_bringup_gpu <slash_ctl_dev>
 *
 * Example:
 *   ./rp1_bringup_gpu /dev/slash_ctl0
 */

#include <slash/ctldev.h>
#include <slash/uapi/rp1_protocol.h>
#include <slash/uapi/slash_interface.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <sstream>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "rp1_bringup_result.h"

/* ── Constants ─────────────────────────────────────────────────────────── */

/* PF2 BAR4 holds the RP1 DDR aperture on V80; matches the convention in
 * examples/rp1_bringup/rp1_bringup.c and examples/07_rp1_memcheck. */
static constexpr int      kBarNumber       = 4;
static constexpr uint64_t kBarCtrlOffset   = 64ULL * 1024ULL * 1024ULL;

#if SLASH_HIP

#include <hip/hip_runtime.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

/* Forward declaration — defined in rp1_bringup_kernel.hip. */
__global__ void rp1_bringup_signal_gpu(volatile uint8_t* bar,
                                       size_t bar_len,
                                       rp1_bringup_result_t* result_out);

/* ── HIP / HSA error helpers ───────────────────────────────────────────── */

#define HIP_CHECK(call)                                                     \
    do {                                                                    \
        hipError_t _e = (call);                                             \
        if (_e != hipSuccess) {                                             \
            throw std::runtime_error(                                       \
                std::string("HIP error in " #call ": ") +                   \
                hipGetErrorString(_e) + " (" __FILE__ ":" +                 \
                std::to_string(__LINE__) + ")");                            \
        }                                                                   \
    } while (0)

/* HSA_STATUS_INFO_BREAK is the documented "success, stopped early" return
 * from iterator APIs (hsa_iterate_agents et al.) when the user callback
 * asks iteration to terminate.  It is not an error and must not throw. */
#define HSA_CHECK(call)                                                     \
    do {                                                                    \
        hsa_status_t _s = (call);                                           \
        if (_s != HSA_STATUS_SUCCESS && _s != HSA_STATUS_INFO_BREAK) {      \
            const char* _msg = nullptr;                                     \
            hsa_status_string(_s, &_msg);                                   \
            throw std::runtime_error(                                       \
                std::string("HSA error in " #call ": ") +                   \
                (_msg ? _msg : "unknown") + " (" __FILE__ ":" +             \
                std::to_string(__LINE__) + ")");                            \
        }                                                                   \
    } while (0)

/* ── HSA agent iterator ────────────────────────────────────────────────── */

static hsa_status_t find_gpu_agent_cb(hsa_agent_t agent, void* data)
{
    hsa_device_type_t type;
    if (hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type) != HSA_STATUS_SUCCESS)
        return HSA_STATUS_SUCCESS;

    if (type == HSA_DEVICE_TYPE_GPU) {
        *reinterpret_cast<hsa_agent_t*>(data) = agent;
        return HSA_STATUS_INFO_BREAK; /* stop iteration */
    }
    return HSA_STATUS_SUCCESS;
}

/* ── Status pretty-printing ────────────────────────────────────────────── */

static const char* status_str(uint32_t s)
{
    switch (s) {
    case RP1_BRINGUP_STATUS_PENDING:  return "PENDING";
    case RP1_BRINGUP_STATUS_PASS:     return "PASS";
    case RP1_BRINGUP_STATUS_NO_FW:    return "NO_FIRMWARE (magic mismatch)";
    case RP1_BRINGUP_STATUS_BAD_SLOT: return "BAD_SLOT (graph completed but slot != 0xDEADBEEF)";
    case RP1_BRINGUP_STATUS_TIMEOUT:  return "TIMEOUT (graph_done_seq never advanced)";
    default:                          return "?";
    }
}

static const char* rp1_state_str(uint32_t s)
{
    switch (s) {
    case RP1_STATE_INIT:    return "INIT";
    case RP1_STATE_READY:   return "READY";
    case RP1_STATE_RUNNING: return "RUNNING";
    case RP1_STATE_ERROR:   return "ERROR";
    case RP1_STATE_HALTED:  return "HALTED";
    default:                return "?";
    }
}

/* ── Main GPU bringup path ─────────────────────────────────────────────── */

static int run(const char* ctl_dev_path)
{
    /* 1. Open SLASH control device. */
    slash_ctldev* ctldev = slash_ctldev_open(ctl_dev_path);
    if (!ctldev)
        throw std::runtime_error(std::string("slash_ctldev_open: ") + strerror(errno));

    /* 2. Print device info. */
    slash_ioctl_device_info* dev_info = slash_device_info_read(ctldev);
    if (dev_info) {
        std::cout << "SLASH device: " << dev_info->bdf
                  << "  vendor=0x" << std::hex << dev_info->vendor_id
                  << "  device=0x" << dev_info->device_id << std::dec << "\n";
        slash_device_info_free(dev_info);
    }

    /* 3. Query BAR info, validate prefetchable + size. */
    slash_ioctl_bar_info* bar_info = slash_bar_info_read(ctldev, kBarNumber);
    if (!bar_info) {
        slash_ctldev_close(ctldev);
        throw std::runtime_error(std::string("slash_bar_info_read: ") + strerror(errno));
    }

    if (!bar_info->usable) {
        slash_bar_info_free(bar_info);
        slash_ctldev_close(ctldev);
        throw std::runtime_error(
            "BAR" + std::to_string(kBarNumber) +
            " is not usable.  The bitstream may not mark PF2 BAR" +
            std::to_string(kBarNumber) +
            " prefetchable, or pci_p2pdma_add_resource() failed at probe.  "
            "Check `dmesg | grep slash` for 'BAR" + std::to_string(kBarNumber) +
            " P2PDMA registration failed'.");
    }

    const uint64_t bar_required = kBarCtrlOffset + RP1_DEFAULT_SIG_ARRAY_OFFSET +
                                  256ull * sizeof(rp1_signal_slot_t);
    if (static_cast<uint64_t>(bar_info->length) < bar_required) {
        std::ostringstream oss;
        oss << "BAR" << kBarNumber
            << " length 0x" << std::hex << bar_info->length
            << " < required 0x" << bar_required
            << std::dec
            << " (need 64 MiB control offset + signal array).";
        slash_bar_info_free(bar_info);
        slash_ctldev_close(ctldev);
        throw std::runtime_error(oss.str());
    }

    std::cout << "BAR" << kBarNumber
              << "  addr=0x" << std::hex << bar_info->start_address
              << "  len=0x"  << bar_info->length << std::dec
              << "  in_use=" << static_cast<int>(bar_info->in_use) << "\n";
    slash_bar_info_free(bar_info);

    /* 4. Obtain the BAR dma-buf fd (returned as ioctl return value). */
    slash_ioctl_bar_fd_request req{};
    req.size       = sizeof(req);
    req.bar_number = static_cast<uint8_t>(kBarNumber);
    req.flags      = O_CLOEXEC;

    int bar_fd = ioctl(ctldev->fd, SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req);
    if (bar_fd < 0) {
        slash_ctldev_close(ctldev);
        throw std::runtime_error(
            std::string("SLASH_CTLDEV_IOCTL_GET_BAR_FD(BAR") +
            std::to_string(kBarNumber) + "): " + strerror(errno));
    }

    size_t bar_len = static_cast<size_t>(req.length);
    std::cout << "BAR dma-buf fd=" << bar_fd << "  len=" << bar_len << "\n";

    /* 5. Initialise HSA and find the first GPU agent. */
    HSA_CHECK(hsa_init());

    hsa_agent_t gpu_agent{};
    gpu_agent.handle = 0;
    HSA_CHECK(hsa_iterate_agents(find_gpu_agent_cb, &gpu_agent));
    if (gpu_agent.handle == 0) {
        close(bar_fd);
        slash_ctldev_close(ctldev);
        hsa_shut_down();
        throw std::runtime_error("No GPU HSA agent found");
    }

    char agent_name[64]{};
    hsa_agent_get_info(gpu_agent, HSA_AGENT_INFO_NAME, agent_name);
    std::cout << "GPU agent: " << agent_name << "\n";

    /* 6. Import the SLASH BAR4 dma-buf into GPU address space.
     *
     *    This calls into the kernel's slash_bar_dmabuf_attach() and
     *    slash_bar_dmabuf_map() (driver/slash_dmabuf.c), which validate
     *    the PCIe P2P topology via pci_p2pdma_distance_many() and build
     *    a scatter-gather table from the BAR pages for the GPU's DMA
     *    engine. While this mapping is live, any host-side
     *    slash_bar_file_open(BAR4) returns -EBUSY.
     */
    size_t mapped_size = 0;
    void*  gpu_ptr     = nullptr;
    hsa_status_t map_status = hsa_amd_interop_map_buffer(
        1, &gpu_agent,
        bar_fd,
        0,              /* flags — reserved, must be 0 */
        &mapped_size,
        &gpu_ptr,
        nullptr,        /* metadata_size */
        nullptr         /* metadata     */
    );
    if (map_status != HSA_STATUS_SUCCESS) {
        const char* hsa_msg = nullptr;
        hsa_status_string(map_status, &hsa_msg);
        std::ostringstream oss;
        oss << "hsa_amd_interop_map_buffer failed: "
            << (hsa_msg ? hsa_msg : "unknown")
            << " (status=" << map_status << "). "
            << "HSA reports generic ERROR for any kernel-side dma-buf attach "
            << "failure; consult dmesg for the slash driver's specific reason "
            << "(BAR not P2PDMA-registered, kernel without CONFIG_PCI_P2PDMA, "
            << "active CPU mmap on BAR" << kBarNumber
            << ", or rejected PCIe topology). See examples/rp1_bringup_gpu/README.md.";
        throw std::runtime_error(oss.str());
    }

    std::cout << "BAR" << kBarNumber
              << " mapped into GPU VA: " << gpu_ptr
              << "  mapped_size=" << mapped_size << "\n";

    /* 7. Allocate the device-side result struct. */
    rp1_bringup_result_t* d_result = nullptr;
    HIP_CHECK(hipMalloc(&d_result, sizeof(*d_result)));
    HIP_CHECK(hipMemset(d_result, 0, sizeof(*d_result)));

    /* 8. Launch the GPU kernel: 1 block × 1 thread. */
    std::cout << "Launching GPU kernel: rp1_bringup_signal_gpu<<<1, 1>>>\n";
    rp1_bringup_signal_gpu<<<dim3(1), dim3(1), 0, 0>>>(
        reinterpret_cast<volatile uint8_t*>(gpu_ptr),
        mapped_size,
        d_result
    );
    HIP_CHECK(hipDeviceSynchronize());

    /* 9. Read back the result. */
    rp1_bringup_result_t res{};
    HIP_CHECK(hipMemcpy(&res, d_result, sizeof(res), hipMemcpyDeviceToHost));
    hipFree(d_result);

    /* 10. Print the diagnostics. */
    std::cout << "status         = " << res.status
              << " (" << status_str(res.status) << ")\n"
              << "magic_seen     = 0x" << std::hex << res.magic_seen << std::dec
              << " (" << ((res.magic_seen == RP1_CTRL_MAGIC) ? "SQR1" : "BAD") << ")\n"
              << "slot0          = 0x" << std::hex << res.slot0 << std::dec << "\n"
              << "graph_done_seq = " << res.graph_done_seq << "\n"
              << "rp1_state      = " << res.rp1_state
              << " (" << rp1_state_str(res.rp1_state) << ")\n"
              << "polls          = " << res.polls << "\n";

    /* 11. Tear down the GPU mapping and the dma-buf fd. */
    HSA_CHECK(hsa_amd_interop_unmap_buffer(gpu_ptr));
    close(bar_fd);
    slash_ctldev_close(ctldev);
    hsa_shut_down();

    if (res.status != RP1_BRINGUP_STATUS_PASS)
        return 1;

    std::cout << "PASSED\n";
    return 0;
}

#else /* !SLASH_HIP */

static int run(const char*)
{
    std::cerr << "ERROR: HIP/ROCm was not found at build time.\n"
                 "       Rebuild with ROCm installed and "
                 "-DCMAKE_HIP_COMPILER=/opt/rocm/bin/amdclang++.\n";
    return 1;
}

#endif /* SLASH_HIP */

/* ── Entry point ───────────────────────────────────────────────────────── */

static void print_usage(const char* argv0)
{
    std::cerr << "Usage: " << argv0 << " <slash_ctl_dev>\n"
              << "Example: " << argv0 << " /dev/slash_ctl0\n";
}

int main(int argc, char** argv)
{
    if (argc != 2) {
        print_usage(argv[0]);
        return 1;
    }

    try {
        return run(argv[1]);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
