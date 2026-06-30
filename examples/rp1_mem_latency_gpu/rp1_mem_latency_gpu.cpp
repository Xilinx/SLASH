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
 * rp1_mem_latency_gpu
 *
 * Imports PF2 BAR4 as a dma-buf into GPU address space, then launches a
 * single-thread HIP kernel that measures raw volatile loads and stores to a
 * scratch region in the RP1 shared DDR aperture. The host CPU does not mmap
 * BAR4 and does not participate in the measured loop.
 */

#include <slash/ctldev.h>
#include <slash/uapi/rp1_protocol.h>
#include <slash/uapi/slash_interface.h>

#include <algorithm>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <numeric>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "rp1_mem_latency_result.h"

static constexpr int      kBarNumber           = 4;
static constexpr uint64_t kBarCtrlOffset       = 64ULL * 1024ULL * 1024ULL;
static constexpr uint64_t kSharedApertureBytes = 64ULL * 1024ULL * 1024ULL;
static constexpr uint64_t kDefaultScratchOffset = 0x00200000ULL;
static constexpr uint64_t kDefaultScratchBytes  = 0x00100000ULL;
static constexpr uint32_t kDefaultIterations    = 10000;
static constexpr uint32_t kDefaultWarmup        = 1000;
static constexpr uint32_t kDefaultStride        = 64;

struct Options {
    std::string ctl_dev_path;
    uint64_t scratch_offset = kDefaultScratchOffset;
    uint64_t scratch_bytes = kDefaultScratchBytes;
    uint32_t iterations = kDefaultIterations;
    uint32_t warmup = kDefaultWarmup;
    uint32_t stride = kDefaultStride;
    uint32_t mode = RP1_MEM_LATENCY_MODE_RW;
    double clock_mhz = 0.0;
    bool clock_mhz_override = false;
};

static void print_usage(const char* argv0)
{
    std::cerr
        << "Usage: " << argv0 << " <slash_ctl_dev> [options]\n"
        << "\n"
        << "Options:\n"
        << "  --iterations N       measured iterations (default: 10000)\n"
        << "  --warmup N           unreported warmup iterations (default: 1000)\n"
        << "  --scratch-offset N   offset inside RP1 shared DDR aperture (default: 0x200000)\n"
        << "  --scratch-bytes N    scratch span in bytes (default: 0x100000)\n"
        << "  --stride N           byte stride between sampled words (default: 64)\n"
        << "  --mode read|write|rw measured operations (default: rw)\n"
        << "  --clock-mhz N        clock64() conversion rate override in MHz\n"
        << "\n"
        << "Example:\n"
        << "  " << argv0 << " /dev/slash_ctl0 --iterations 20000 --mode rw\n";
}

static uint64_t parse_u64(const std::string& text, const char* what)
{
    size_t pos = 0;
    uint64_t value = 0;
    try {
        value = std::stoull(text, &pos, 0);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("Invalid ") + what + ": " + text);
    }
    if (pos != text.size())
        throw std::runtime_error(std::string("Invalid ") + what + ": " + text);
    return value;
}

static uint32_t parse_u32(const std::string& text, const char* what)
{
    uint64_t value = parse_u64(text, what);
    if (value > std::numeric_limits<uint32_t>::max())
        throw std::runtime_error(std::string(what) + " is too large: " + text);
    return static_cast<uint32_t>(value);
}

static double parse_double(const std::string& text, const char* what)
{
    size_t pos = 0;
    double value = 0.0;
    try {
        value = std::stod(text, &pos);
    } catch (const std::exception&) {
        throw std::runtime_error(std::string("Invalid ") + what + ": " + text);
    }
    if (pos != text.size() || !std::isfinite(value))
        throw std::runtime_error(std::string("Invalid ") + what + ": " + text);
    return value;
}

static uint32_t parse_mode(const std::string& text)
{
    if (text == "read")
        return RP1_MEM_LATENCY_MODE_READ;
    if (text == "write")
        return RP1_MEM_LATENCY_MODE_WRITE;
    if (text == "rw")
        return RP1_MEM_LATENCY_MODE_RW;
    throw std::runtime_error("Invalid mode '" + text + "' (expected read, write, or rw)");
}

static std::string mode_str(uint32_t mode)
{
    switch (mode) {
    case RP1_MEM_LATENCY_MODE_READ:  return "read";
    case RP1_MEM_LATENCY_MODE_WRITE: return "write";
    case RP1_MEM_LATENCY_MODE_RW:    return "rw";
    default:                         return "?";
    }
}

static const char* status_str(uint32_t status)
{
    switch (status) {
    case RP1_MEM_LATENCY_STATUS_PENDING:  return "PENDING";
    case RP1_MEM_LATENCY_STATUS_PASS:     return "PASS";
    case RP1_MEM_LATENCY_STATUS_BAD_ARGS: return "BAD_ARGS";
    case RP1_MEM_LATENCY_STATUS_MISMATCH: return "MISMATCH";
    default:                              return "?";
    }
}

static Options parse_args(int argc, char** argv)
{
    if (argc == 2 && (std::string(argv[1]) == "--help" || std::string(argv[1]) == "-h")) {
        print_usage(argv[0]);
        std::exit(0);
    }

    if (argc < 2) {
        print_usage(argv[0]);
        throw std::runtime_error("missing <slash_ctl_dev>");
    }

    Options opt{};
    opt.ctl_dev_path = argv[1];

    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        auto need_value = [&](const char* name) -> std::string {
            if (i + 1 >= argc)
                throw std::runtime_error(std::string("Missing value for ") + name);
            return argv[++i];
        };

        if (arg == "--iterations") {
            opt.iterations = parse_u32(need_value("--iterations"), "iterations");
        } else if (arg == "--warmup") {
            opt.warmup = parse_u32(need_value("--warmup"), "warmup");
        } else if (arg == "--scratch-offset") {
            opt.scratch_offset = parse_u64(need_value("--scratch-offset"), "scratch offset");
        } else if (arg == "--scratch-bytes") {
            opt.scratch_bytes = parse_u64(need_value("--scratch-bytes"), "scratch bytes");
        } else if (arg == "--stride") {
            opt.stride = parse_u32(need_value("--stride"), "stride");
        } else if (arg == "--mode") {
            opt.mode = parse_mode(need_value("--mode"));
        } else if (arg == "--clock-mhz") {
            opt.clock_mhz = parse_double(need_value("--clock-mhz"), "clock MHz");
            opt.clock_mhz_override = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown option: " + arg);
        }
    }

    if (opt.iterations == 0)
        throw std::runtime_error("--iterations must be non-zero");
    if (opt.scratch_bytes < sizeof(uint32_t))
        throw std::runtime_error("--scratch-bytes must be at least 4");
    if (opt.stride < sizeof(uint32_t))
        throw std::runtime_error("--stride must be at least 4");
    if ((opt.scratch_offset & 0x3u) != 0 ||
        (opt.scratch_bytes & 0x3u) != 0 ||
        (opt.stride & 0x3u) != 0) {
        throw std::runtime_error("scratch offset, scratch bytes, and stride must be 4-byte aligned");
    }
    if (opt.scratch_offset < kDefaultScratchOffset) {
        std::ostringstream oss;
        oss << "--scratch-offset 0x" << std::hex << opt.scratch_offset
            << " overlaps the protocol-reserved RP1 layout; use >= 0x"
            << kDefaultScratchOffset;
        throw std::runtime_error(oss.str());
    }
    if (opt.scratch_offset > kSharedApertureBytes ||
        opt.scratch_bytes > kSharedApertureBytes - opt.scratch_offset) {
        throw std::runtime_error("scratch range exceeds the 64 MiB RP1 shared DDR aperture");
    }
    if (opt.clock_mhz_override && opt.clock_mhz <= 0.0)
        throw std::runtime_error("--clock-mhz must be positive");

    return opt;
}

struct SampleStats {
    uint64_t min = 0;
    uint64_t p50 = 0;
    uint64_t p90 = 0;
    uint64_t p99 = 0;
    uint64_t max = 0;
    double mean = 0.0;
};

struct ClockInfo {
    double mhz = 0.0;
    std::string source;
};

static uint64_t percentile(const std::vector<uint64_t>& sorted, uint32_t pct)
{
    if (sorted.empty())
        return 0;
    const size_t idx = ((sorted.size() - 1) * pct + 50) / 100;
    return sorted[idx];
}

static SampleStats summarize(std::vector<uint64_t> samples)
{
    SampleStats stats{};
    if (samples.empty())
        return stats;

    std::sort(samples.begin(), samples.end());
    stats.min = samples.front();
    stats.p50 = percentile(samples, 50);
    stats.p90 = percentile(samples, 90);
    stats.p99 = percentile(samples, 99);
    stats.max = samples.back();

    long double sum = 0.0;
    for (uint64_t value : samples)
        sum += static_cast<long double>(value);
    stats.mean = static_cast<double>(sum / samples.size());
    return stats;
}

static void print_time_value(double cycles, double clock_mhz, const char* unit)
{
    const double us = cycles / clock_mhz;
    const double value = (std::strcmp(unit, "ns") == 0) ? us * 1000.0 : us;
    std::cout << std::fixed << std::setprecision(2) << value << unit
              << std::defaultfloat;
}

static const char* time_unit_for(const SampleStats& stats, double clock_mhz)
{
    const double p50_us = static_cast<double>(stats.p50) / clock_mhz;
    return (p50_us < 1.0) ? "ns" : "us";
}

static void print_stats(const char* label,
                        const std::vector<uint64_t>& samples,
                        double clock_mhz)
{
    if (samples.empty())
        return;

    SampleStats stats = summarize(samples);
    std::cout << std::left << std::setw(18) << label
              << " min=" << std::setw(8) << stats.min
              << " p50=" << std::setw(8) << stats.p50
              << " p90=" << std::setw(8) << stats.p90
              << " p99=" << std::setw(8) << stats.p99
              << " max=" << std::setw(8) << stats.max
              << " mean=" << std::fixed << std::setprecision(1) << stats.mean
              << " cycles\n"
              << std::defaultfloat;

    if (clock_mhz <= 0.0)
        return;

    const char* unit = time_unit_for(stats, clock_mhz);
    std::cout << std::left << std::setw(18) << "  time"
              << " min=";
    print_time_value(static_cast<double>(stats.min), clock_mhz, unit);
    std::cout << " p50=";
    print_time_value(static_cast<double>(stats.p50), clock_mhz, unit);
    std::cout << " p90=";
    print_time_value(static_cast<double>(stats.p90), clock_mhz, unit);
    std::cout << " p99=";
    print_time_value(static_cast<double>(stats.p99), clock_mhz, unit);
    std::cout << " max=";
    print_time_value(static_cast<double>(stats.max), clock_mhz, unit);
    std::cout << " mean=";
    print_time_value(stats.mean, clock_mhz, unit);
    std::cout << "\n";
}

#if SLASH_HIP

#include <hip/hip_runtime.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

__global__ void rp1_mem_latency_gpu_kernel(
    volatile uint8_t* bar,
    size_t bar_len,
    rp1_mem_latency_config_t cfg,
    rp1_mem_latency_result_t* result_out,
    uint64_t* write_cycles,
    uint64_t* read_cycles,
    uint64_t* rw_cycles);

#define HIP_CHECK(call)                                                     \
    do {                                                                    \
        hipError_t _e = (call);                                             \
        if (_e != hipSuccess) {                                             \
            throw std::runtime_error(                                       \
                std::string("HIP error in " #call ": ") +                  \
                hipGetErrorString(_e) + " (" __FILE__ ":" +                \
                std::to_string(__LINE__) + ")");                           \
        }                                                                   \
    } while (0)

#define HSA_CHECK(call)                                                     \
    do {                                                                    \
        hsa_status_t _s = (call);                                           \
        if (_s != HSA_STATUS_SUCCESS && _s != HSA_STATUS_INFO_BREAK) {      \
            const char* _msg = nullptr;                                     \
            hsa_status_string(_s, &_msg);                                   \
            throw std::runtime_error(                                       \
                std::string("HSA error in " #call ": ") +                  \
                (_msg ? _msg : "unknown") + " (" __FILE__ ":" +            \
                std::to_string(__LINE__) + ")");                           \
        }                                                                   \
    } while (0)

static hsa_status_t find_gpu_agent_cb(hsa_agent_t agent, void* data)
{
    hsa_device_type_t type;
    if (hsa_agent_get_info(agent, HSA_AGENT_INFO_DEVICE, &type) != HSA_STATUS_SUCCESS)
        return HSA_STATUS_SUCCESS;

    if (type == HSA_DEVICE_TYPE_GPU) {
        *reinterpret_cast<hsa_agent_t*>(data) = agent;
        return HSA_STATUS_INFO_BREAK;
    }
    return HSA_STATUS_SUCCESS;
}

struct CtldevHandle {
    slash_ctldev* ptr = nullptr;
    explicit CtldevHandle(const char* path) : ptr(slash_ctldev_open(path)) {}
    ~CtldevHandle() { if (ptr) slash_ctldev_close(ptr); }
    CtldevHandle(const CtldevHandle&) = delete;
    CtldevHandle& operator=(const CtldevHandle&) = delete;
};

struct BarInfoHandle {
    slash_ioctl_bar_info* ptr = nullptr;
    BarInfoHandle(slash_ctldev* dev, int bar) : ptr(slash_bar_info_read(dev, bar)) {}
    ~BarInfoHandle() { if (ptr) slash_bar_info_free(ptr); }
    BarInfoHandle(const BarInfoHandle&) = delete;
    BarInfoHandle& operator=(const BarInfoHandle&) = delete;
};

struct FdHandle {
    int fd = -1;
    FdHandle() = default;
    ~FdHandle() { if (fd >= 0) close(fd); }
    FdHandle(const FdHandle&) = delete;
    FdHandle& operator=(const FdHandle&) = delete;
};

struct HsaRuntime {
    bool active = false;
    HsaRuntime()
    {
        HSA_CHECK(hsa_init());
        active = true;
    }
    ~HsaRuntime()
    {
        if (active)
            hsa_shut_down();
    }
    HsaRuntime(const HsaRuntime&) = delete;
    HsaRuntime& operator=(const HsaRuntime&) = delete;
};

struct HsaMap {
    void* ptr = nullptr;
    size_t size = 0;
    HsaMap() = default;
    ~HsaMap()
    {
        if (ptr)
            hsa_amd_interop_unmap_buffer(ptr);
    }
    HsaMap(const HsaMap&) = delete;
    HsaMap& operator=(const HsaMap&) = delete;
};

template <typename T>
struct DeviceBuffer {
    T* ptr = nullptr;
    size_t count = 0;

    DeviceBuffer() = default;
    explicit DeviceBuffer(size_t n) : count(n)
    {
        if (count)
            HIP_CHECK(hipMalloc(&ptr, count * sizeof(T)));
    }
    ~DeviceBuffer()
    {
        if (ptr)
            (void)hipFree(ptr);
    }
    DeviceBuffer(const DeviceBuffer&) = delete;
    DeviceBuffer& operator=(const DeviceBuffer&) = delete;
};

static std::vector<uint64_t> copy_samples(const DeviceBuffer<uint64_t>& d)
{
    std::vector<uint64_t> out(d.count);
    if (!out.empty())
        HIP_CHECK(hipMemcpy(out.data(), d.ptr, out.size() * sizeof(uint64_t),
                            hipMemcpyDeviceToHost));
    return out;
}

static ClockInfo resolve_clock_info(const Options& opt)
{
    if (opt.clock_mhz_override)
        return ClockInfo{opt.clock_mhz, "--clock-mhz override"};

    int device = 0;
    int clock_khz = 0;
    HIP_CHECK(hipGetDevice(&device));
    HIP_CHECK(hipDeviceGetAttribute(&clock_khz, hipDeviceAttributeClockRate, device));
    if (clock_khz <= 0)
        throw std::runtime_error("HIP reported a non-positive device clock rate");

    return ClockInfo{static_cast<double>(clock_khz) / 1000.0,
                     "HIP device attribute"};
}

static int run(const Options& opt)
{
    CtldevHandle ctldev(opt.ctl_dev_path.c_str());
    if (!ctldev.ptr)
        throw std::runtime_error(std::string("slash_ctldev_open: ") + strerror(errno));

    slash_ioctl_device_info* dev_info = slash_device_info_read(ctldev.ptr);
    if (dev_info) {
        std::cout << "SLASH device: " << dev_info->bdf
                  << "  vendor=0x" << std::hex << dev_info->vendor_id
                  << "  device=0x" << dev_info->device_id << std::dec << "\n";
        slash_device_info_free(dev_info);
    }

    BarInfoHandle bar_info(ctldev.ptr, kBarNumber);
    if (!bar_info.ptr)
        throw std::runtime_error(std::string("slash_bar_info_read: ") + strerror(errno));

    const uint64_t scratch_bar_offset = kBarCtrlOffset + opt.scratch_offset;
    const uint64_t bar_required = scratch_bar_offset + opt.scratch_bytes;
    if (!bar_info.ptr->usable) {
        throw std::runtime_error(
            "BAR" + std::to_string(kBarNumber) +
            " is not usable. The bitstream may not mark PF2 BAR" +
            std::to_string(kBarNumber) +
            " prefetchable, or pci_p2pdma_add_resource() failed at probe.");
    }
    if (static_cast<uint64_t>(bar_info.ptr->length) < bar_required) {
        std::ostringstream oss;
        oss << "BAR" << kBarNumber << " length 0x" << std::hex
            << bar_info.ptr->length << " < required 0x" << bar_required;
        throw std::runtime_error(oss.str());
    }

    std::cout << "BAR" << kBarNumber
              << "  addr=0x" << std::hex << bar_info.ptr->start_address
              << "  len=0x" << bar_info.ptr->length << std::dec
              << "  in_use=" << static_cast<int>(bar_info.ptr->in_use) << "\n";

    slash_ioctl_bar_fd_request req{};
    req.size = sizeof(req);
    req.bar_number = static_cast<uint8_t>(kBarNumber);
    req.flags = O_CLOEXEC;

    FdHandle bar_fd;
    bar_fd.fd = ioctl(ctldev.ptr->fd, SLASH_CTLDEV_IOCTL_GET_BAR_FD, &req);
    if (bar_fd.fd < 0) {
        throw std::runtime_error(
            std::string("SLASH_CTLDEV_IOCTL_GET_BAR_FD(BAR") +
            std::to_string(kBarNumber) + "): " + strerror(errno));
    }
    std::cout << "BAR dma-buf fd=" << bar_fd.fd << "  len=" << req.length << "\n";

    HsaRuntime hsa;
    hsa_agent_t gpu_agent{};
    gpu_agent.handle = 0;
    HSA_CHECK(hsa_iterate_agents(find_gpu_agent_cb, &gpu_agent));
    if (gpu_agent.handle == 0)
        throw std::runtime_error("No GPU HSA agent found");

    char agent_name[64]{};
    hsa_agent_get_info(gpu_agent, HSA_AGENT_INFO_NAME, agent_name);
    std::cout << "GPU agent: " << agent_name << "\n";

    ClockInfo clock_info = resolve_clock_info(opt);

    HsaMap map;
    hsa_status_t map_status = hsa_amd_interop_map_buffer(
        1, &gpu_agent,
        bar_fd.fd,
        0,
        &map.size,
        &map.ptr,
        nullptr,
        nullptr);
    if (map_status != HSA_STATUS_SUCCESS) {
        const char* hsa_msg = nullptr;
        hsa_status_string(map_status, &hsa_msg);
        std::ostringstream oss;
        oss << "hsa_amd_interop_map_buffer failed: "
            << (hsa_msg ? hsa_msg : "unknown")
            << " (status=" << map_status << "). Check dmesg for BAR P2PDMA "
            << "registration, active CPU mmap users, or PCIe topology failures.";
        throw std::runtime_error(oss.str());
    }

    std::cout << "BAR" << kBarNumber << " mapped into GPU VA: " << map.ptr
              << "  mapped_size=" << map.size << "\n";
    std::cout << "scratch BAR offset=0x" << std::hex << scratch_bar_offset
              << "  aperture offset=0x" << opt.scratch_offset
              << "  bytes=0x" << opt.scratch_bytes << std::dec << "\n";
    std::cout << "iterations=" << opt.iterations
              << "  warmup=" << opt.warmup
              << "  stride=" << opt.stride
              << "  mode=" << mode_str(opt.mode) << "\n";
    std::cout << "clock rate=" << std::fixed << std::setprecision(1)
              << clock_info.mhz << " MHz (" << clock_info.source << ")\n"
              << std::defaultfloat;

    rp1_mem_latency_config_t cfg{};
    cfg.scratch_bar_offset = scratch_bar_offset;
    cfg.scratch_bytes = opt.scratch_bytes;
    cfg.iterations = opt.iterations;
    cfg.warmup = opt.warmup;
    cfg.stride_bytes = opt.stride;
    cfg.mode = opt.mode;

    DeviceBuffer<rp1_mem_latency_result_t> d_result(1);
    HIP_CHECK(hipMemset(d_result.ptr, 0, sizeof(rp1_mem_latency_result_t)));

    DeviceBuffer<uint64_t> d_write(
        (opt.mode & RP1_MEM_LATENCY_MODE_WRITE) ? opt.iterations : 0);
    DeviceBuffer<uint64_t> d_read(
        (opt.mode & RP1_MEM_LATENCY_MODE_READ) ? opt.iterations : 0);
    DeviceBuffer<uint64_t> d_rw(
        (opt.mode == RP1_MEM_LATENCY_MODE_RW) ? opt.iterations : 0);

    std::cout << "Launching GPU kernel: rp1_mem_latency_gpu_kernel<<<1, 1>>>\n";
    rp1_mem_latency_gpu_kernel<<<dim3(1), dim3(1), 0, 0>>>(
        reinterpret_cast<volatile uint8_t*>(map.ptr),
        map.size,
        cfg,
        d_result.ptr,
        d_write.ptr,
        d_read.ptr,
        d_rw.ptr);
    HIP_CHECK(hipDeviceSynchronize());

    rp1_mem_latency_result_t res{};
    HIP_CHECK(hipMemcpy(&res, d_result.ptr, sizeof(res), hipMemcpyDeviceToHost));

    std::vector<uint64_t> write_samples = copy_samples(d_write);
    std::vector<uint64_t> read_samples = copy_samples(d_read);
    std::vector<uint64_t> rw_samples = copy_samples(d_rw);

    std::cout << "status          = " << res.status << " (" << status_str(res.status) << ")\n"
              << "magic_seen      = 0x" << std::hex << res.magic_seen << std::dec
              << " (" << ((res.magic_seen == RP1_CTRL_MAGIC) ? "SQR1" : "not SQR1") << ")\n"
              << "sample_count    = " << res.sample_count << "\n"
              << "checksum        = 0x" << std::hex << res.checksum << std::dec << "\n";

    if (res.status == RP1_MEM_LATENCY_STATUS_MISMATCH) {
        std::cout << "first_mismatch = iter " << res.first_mismatch_iter
                  << " expected 0x" << std::hex << res.first_mismatch_expected
                  << " observed 0x" << res.first_mismatch_observed << std::dec << "\n";
    }

    std::cout << "\nLatency distributions (GPU clock cycles and derived time):\n";
    print_stats("write+fence", write_samples, clock_info.mhz);
    print_stats("read", read_samples, clock_info.mhz);
    print_stats("write+read", rw_samples, clock_info.mhz);

    if (res.status != RP1_MEM_LATENCY_STATUS_PASS)
        return 1;

    std::cout << "PASSED\n";
    return 0;
}

#else /* !SLASH_HIP */

static int run(const Options&)
{
    std::cerr << "ERROR: HIP/ROCm was not found at build time.\n"
                 "       Rebuild with ROCm installed and "
                 "-DCMAKE_HIP_COMPILER=/opt/rocm/bin/amdclang++.\n";
    return 1;
}

#endif /* SLASH_HIP */

int main(int argc, char** argv)
{
    try {
        Options opt = parse_args(argc, argv);
        return run(opt);
    } catch (const std::exception& e) {
        std::cerr << "ERROR: " << e.what() << "\n";
        return 1;
    }
}
