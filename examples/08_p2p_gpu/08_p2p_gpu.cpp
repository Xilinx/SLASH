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
 * 08_p2p_gpu — GPU → SLASH peer-to-peer example.
 *
 * Demonstrates a HIP GPU kernel dispatching the slash_add FPGA kernel by
 * writing directly to the SLASH AXI-Lite BAR over PCIe P2P DMA. The GPU
 * imports the SLASH BAR as a dma-buf via hsa_amd_interop_map_buffer, which
 * invokes the kernel's slash_bar_dmabuf_attach / slash_bar_dmabuf_map
 * callbacks. No CPU mmap or CPU-side register writes are used in the hot path.
 *
 * Usage:
 *   ./08_p2p_gpu <slash_ctl_dev> <a> <b> [bar_number]
 *
 * Example:
 *   ./08_p2p_gpu /dev/slash_ctl0 7 9
 *   ./08_p2p_gpu /dev/slash_ctl0 7 9 0
 */

#include <slash/ctldev.h>
#include <slash/uapi/slash_interface.h>

#include <cerrno>
#include <cstdint>
#include <cstring>
#include <iostream>
#include <stdexcept>
#include <string>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#if SLASH_HIP
#include <hip/hip_runtime.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

/* Forward declaration — defined in gpu_kernel.hip */
__global__ void slash_add_gpu(
    volatile uint32_t* bar,
    uint32_t a,
    uint32_t b,
    uint32_t* result_out,
    uint32_t* poll_count_out
);

/* ── HIP error helpers ─────────────────────────────────────────────────── */

#define HIP_CHECK(call)                                                     \
    do {                                                                    \
        hipError_t _e = (call);                                             \
        if (_e != hipSuccess) {                                             \
            throw std::runtime_error(                                       \
                std::string("HIP error in " #call ": ") +                  \
                hipGetErrorString(_e) + " (" __FILE__ ":" +                \
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
            hsa_status_string(_s, &_msg);                                  \
            throw std::runtime_error(                                       \
                std::string("HSA error in " #call ": ") +                  \
                (_msg ? _msg : "unknown") + " (" __FILE__ ":" +            \
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

/* ── Main GPU P2P path ─────────────────────────────────────────────────── */

static int run(const char* ctl_dev_path, uint32_t a, uint32_t b, int bar_number)
{
    /* 1. Open SLASH control device */
    slash_ctldev* ctldev = slash_ctldev_open(ctl_dev_path);
    if (!ctldev)
        throw std::runtime_error(std::string("slash_ctldev_open: ") + strerror(errno));

    /* 2. Print device info */
    slash_ioctl_device_info* dev_info = slash_device_info_read(ctldev);
    if (dev_info) {
        std::cout << "SLASH device: " << dev_info->bdf
                  << "  vendor=0x" << std::hex << dev_info->vendor_id
                  << "  device=0x" << dev_info->device_id << std::dec << "\n";
        slash_device_info_free(dev_info);
    }

    /* 3. Query BAR info */
    slash_ioctl_bar_info* bar_info = slash_bar_info_read(ctldev, bar_number);
    if (!bar_info)
        throw std::runtime_error(std::string("slash_bar_info_read: ") + strerror(errno));

    if (!bar_info->usable) {
        slash_bar_info_free(bar_info);
        slash_ctldev_close(ctldev);
        throw std::runtime_error("BAR " + std::to_string(bar_number) + " is not usable");
    }

    std::cout << "BAR" << bar_number
              << "  addr=0x" << std::hex << bar_info->start_address
              << "  len=0x"  << bar_info->length << std::dec
              << "  in_use=" << static_cast<int>(bar_info->in_use) << "\n";
    slash_bar_info_free(bar_info);

    /* 4. Obtain raw dma-buf fd for the BAR (returned as ioctl return value) */
    slash_ioctl_bar_fd_request req{};
    req.size       = sizeof(req);
    req.bar_number = static_cast<uint8_t>(bar_number);
    req.flags      = O_CLOEXEC;

    int bar_fd = ioctl(ctldev->fd, SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req);
    if (bar_fd < 0)
        throw std::runtime_error(std::string("SLASH_CTLDEV_IOCTL_GET_BAR_FD: ") + strerror(errno));

    size_t bar_len = static_cast<size_t>(req.length);
    std::cout << "BAR dma-buf fd=" << bar_fd << "  len=" << bar_len << "\n";

    /* 5. Initialise HSA and find the first GPU agent */
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

    /* 6. Import the SLASH BAR dma-buf into GPU address space.
     *
     *    This calls into the kernel's slash_bar_dmabuf_attach() and
     *    slash_bar_dmabuf_map() (added by the pcip2p merge) which validate
     *    the PCIe P2P topology via pci_p2pdma_distance_many() and build a
     *    scatter-gather table from the BAR pages for the GPU's DMA engine.
     */
    size_t mapped_size = 0;
    void*  gpu_ptr     = nullptr;
    HSA_CHECK(hsa_amd_interop_map_buffer(
        1, &gpu_agent,
        bar_fd,
        0,              /* flags — reserved, must be 0 */
        &mapped_size,
        &gpu_ptr,
        nullptr,        /* metadata_size */
        nullptr         /* metadata */
    ));

    std::cout << "BAR mapped into GPU VA: " << gpu_ptr
              << "  mapped_size=" << mapped_size << "\n";

    /* 7. Allocate device-side output buffers */
    uint32_t* d_result     = nullptr;
    uint32_t* d_poll_count = nullptr;
    HIP_CHECK(hipMalloc(&d_result,     sizeof(uint32_t)));
    HIP_CHECK(hipMalloc(&d_poll_count, sizeof(uint32_t)));
    HIP_CHECK(hipMemset(d_result,     0, sizeof(uint32_t)));
    HIP_CHECK(hipMemset(d_poll_count, 0, sizeof(uint32_t)));

    /* 8. Launch GPU kernel: 1 block × 1 thread */
    std::cout << "Launching GPU kernel: a=" << a << " b=" << b << "\n";
    slash_add_gpu<<<dim3(1), dim3(1), 0, 0>>>(
        reinterpret_cast<volatile uint32_t*>(gpu_ptr),
        a, b,
        d_result, d_poll_count
    );
    HIP_CHECK(hipDeviceSynchronize());

    /* 9. Retrieve results from device */
    uint32_t hw_result  = 0;
    uint32_t poll_count = 0;
    HIP_CHECK(hipMemcpy(&hw_result,  d_result,     sizeof(uint32_t), hipMemcpyDeviceToHost));
    HIP_CHECK(hipMemcpy(&poll_count, d_poll_count, sizeof(uint32_t), hipMemcpyDeviceToHost));

    hipFree(d_result);
    hipFree(d_poll_count);

    /* 10. Validate */
    const uint32_t expected = a + b;
    std::cout << "Result: hw=" << hw_result
              << "  expected=" << expected
              << "  poll_count=" << poll_count << "\n";

    /* 11. Unmap BAR from GPU — triggers slash_bar_dmabuf_unmap in the driver */
    HSA_CHECK(hsa_amd_interop_unmap_buffer(gpu_ptr));

    close(bar_fd);
    slash_ctldev_close(ctldev);
    hsa_shut_down();

    if (hw_result != expected)
        throw std::runtime_error("Result mismatch: expected " + std::to_string(expected) +
                                 " got " + std::to_string(hw_result));

    std::cout << "PASSED\n";
    return 0;
}

#else /* !SLASH_HIP */

static int run(const char*, uint32_t, uint32_t, int)
{
    std::cerr << "ERROR: HIP/ROCm was not found at build time.\n"
                 "       Rebuild with ROCm installed and "
                 "-DCMAKE_PREFIX_PATH=/opt/rocm.\n";
    return 1;
}

#endif /* SLASH_HIP */

/* ── Entry point ───────────────────────────────────────────────────────── */

static void print_usage(const char* argv0)
{
    std::cerr << "Usage: " << argv0 << " <slash_ctl_dev> <a> <b> [bar_number]\n"
              << "  bar_number defaults to 0\n"
              << "Example: " << argv0 << " /dev/slash_ctl0 7 9\n";
}

int main(int argc, char** argv)
{
    if (argc < 4 || argc > 5) {
        print_usage(argv[0]);
        return 1;
    }

    try {
        const char* ctl_dev    = argv[1];
        uint32_t    a          = static_cast<uint32_t>(std::stoul(argv[2]));
        uint32_t    b          = static_cast<uint32_t>(std::stoul(argv[3]));
        int         bar_number = (argc == 5) ? std::stoi(argv[4]) : 0;

        return run(ctl_dev, a, b, bar_number);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
