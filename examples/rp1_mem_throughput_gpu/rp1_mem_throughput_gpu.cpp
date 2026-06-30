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
 * rp1_mem_throughput_gpu
 *
 * Imports PF2 BAR4 as a dma-buf into GPU address space, then measures whether
 * ROCm's copy path can move bulk data between GPU VRAM and the imported RP1
 * shared-DDR BAR aperture using hipMemcpyAsync(DeviceToDevice).
 */

#include <slash/ctldev.h>
#include <slash/uapi/rp1_protocol.h>
#include <slash/uapi/slash_interface.h>

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <limits>
#include <sstream>
#include <stdexcept>
#include <string>
#include <vector>

#include <fcntl.h>
#include <sys/ioctl.h>
#include <unistd.h>

static constexpr int      kBarNumber            = 4;
static constexpr uint64_t kBarCtrlOffset        = 64ULL * 1024ULL * 1024ULL;
static constexpr uint64_t kSharedApertureBytes  = 64ULL * 1024ULL * 1024ULL;
static constexpr uint64_t kDefaultScratchOffset = 0x00200000ULL;
static constexpr uint64_t kDefaultBytes         = 64ULL * 1024ULL * 1024ULL;
static constexpr uint32_t kDefaultIters         = 10;
static constexpr uint32_t kDefaultWarmup        = 2;
static constexpr uint8_t  kPatternByte          = 0x5a;
static constexpr size_t   kVerifyChunkBytes     = 4096;

enum Direction : uint32_t {
    kDirectionRead  = 1u << 0,
    kDirectionWrite = 1u << 1,
    kDirectionBoth  = kDirectionRead | kDirectionWrite,
};

struct Options {
    std::string ctl_dev_path;
    uint64_t requested_bytes = kDefaultBytes;
    bool bytes_specified = false;
    uint64_t scratch_offset = kDefaultScratchOffset;
    uint32_t iters = kDefaultIters;
    uint32_t warmup = kDefaultWarmup;
    uint32_t direction = kDirectionBoth;
    bool verify = true;
};

struct TimingStats {
    float min_ms = 0.0f;
    float median_ms = 0.0f;
    float mean_ms = 0.0f;
    float max_ms = 0.0f;
};

static void print_usage(const char* argv0)
{
    std::cerr
        << "Usage: " << argv0 << " <slash_ctl_dev> [options]\n"
        << "\n"
        << "Options:\n"
        << "  --bytes N              transfer size in bytes (default: 67108864, bounded)\n"
        << "  --iters N              timed repetitions (default: 10)\n"
        << "  --warmup N             untimed repetitions (default: 2)\n"
        << "  --direction read|write|both\n"
        << "                         transfer direction (default: both)\n"
        << "  --scratch-offset N     offset inside RP1 shared DDR aperture (default: 0x200000)\n"
        << "  --verify               enable prefix/suffix verification (default)\n"
        << "  --no-verify            disable verification\n"
        << "\n"
        << "Example:\n"
        << "  " << argv0 << " /dev/slash_ctl0 --bytes 67108864 --iters 10\n";
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

static uint32_t parse_direction(const std::string& text)
{
    if (text == "read")
        return kDirectionRead;
    if (text == "write")
        return kDirectionWrite;
    if (text == "both")
        return kDirectionBoth;
    throw std::runtime_error("Invalid direction '" + text + "' (expected read, write, or both)");
}

static std::string direction_str(uint32_t direction)
{
    switch (direction) {
    case kDirectionRead:  return "read";
    case kDirectionWrite: return "write";
    case kDirectionBoth:  return "both";
    default:              return "?";
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

        if (arg == "--bytes") {
            opt.requested_bytes = parse_u64(need_value("--bytes"), "bytes");
            opt.bytes_specified = true;
        } else if (arg == "--iters") {
            opt.iters = parse_u32(need_value("--iters"), "iters");
        } else if (arg == "--warmup") {
            opt.warmup = parse_u32(need_value("--warmup"), "warmup");
        } else if (arg == "--direction") {
            opt.direction = parse_direction(need_value("--direction"));
        } else if (arg == "--scratch-offset") {
            opt.scratch_offset = parse_u64(need_value("--scratch-offset"), "scratch offset");
        } else if (arg == "--verify") {
            opt.verify = true;
        } else if (arg == "--no-verify") {
            opt.verify = false;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            throw std::runtime_error("Unknown option: " + arg);
        }
    }

    if (opt.requested_bytes == 0)
        throw std::runtime_error("--bytes must be non-zero");
    if (opt.iters == 0)
        throw std::runtime_error("--iters must be non-zero");
    if ((opt.scratch_offset & 0x3u) != 0)
        throw std::runtime_error("--scratch-offset must be 4-byte aligned");
    if (opt.scratch_offset < kDefaultScratchOffset) {
        std::ostringstream oss;
        oss << "--scratch-offset 0x" << std::hex << opt.scratch_offset
            << " overlaps the protocol-reserved RP1 layout; use >= 0x"
            << kDefaultScratchOffset;
        throw std::runtime_error(oss.str());
    }
    if (opt.scratch_offset >= kSharedApertureBytes)
        throw std::runtime_error("--scratch-offset exceeds the RP1 shared DDR aperture");

    return opt;
}

static TimingStats summarize(std::vector<float> samples)
{
    TimingStats stats{};
    if (samples.empty())
        return stats;

    std::sort(samples.begin(), samples.end());
    stats.min_ms = samples.front();
    stats.median_ms = samples[samples.size() / 2];
    stats.max_ms = samples.back();

    double sum = 0.0;
    for (float sample : samples)
        sum += sample;
    stats.mean_ms = static_cast<float>(sum / samples.size());
    return stats;
}

static double gib_per_second(uint64_t bytes, float ms)
{
    if (ms <= 0.0f)
        return 0.0;
    const double gib = static_cast<double>(bytes) / (1024.0 * 1024.0 * 1024.0);
    return gib / (static_cast<double>(ms) / 1000.0);
}

static void print_stats(const char* label, uint64_t bytes, const std::vector<float>& samples)
{
    TimingStats stats = summarize(samples);
    std::cout << std::left << std::setw(8) << label
              << " median=" << std::fixed << std::setprecision(3) << stats.median_ms << " ms"
              << " mean=" << stats.mean_ms << " ms"
              << " min=" << stats.min_ms << " ms"
              << " max=" << stats.max_ms << " ms"
              << " throughput_median=" << std::setprecision(3)
              << gib_per_second(bytes, stats.median_ms) << " GiB/s"
              << " throughput_mean=" << gib_per_second(bytes, stats.mean_ms) << " GiB/s\n"
              << std::defaultfloat;
}

struct VerifyChunk {
    uint64_t offset = 0;
    size_t size = 0;
};

static std::vector<VerifyChunk> verify_chunks(uint64_t bytes)
{
    std::vector<VerifyChunk> chunks;
    const size_t first_size = static_cast<size_t>(
        std::min<uint64_t>(bytes, kVerifyChunkBytes));
    chunks.push_back(VerifyChunk{0, first_size});

    if (bytes > first_size) {
        const size_t last_size = static_cast<size_t>(
            std::min<uint64_t>(bytes, kVerifyChunkBytes));
        const uint64_t last_offset = bytes - last_size;
        if (last_offset >= first_size)
            chunks.push_back(VerifyChunk{last_offset, last_size});
    }
    return chunks;
}

static void verify_pattern(const std::vector<uint8_t>& bytes,
                           uint64_t offset,
                           const char* label)
{
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != kPatternByte) {
            std::ostringstream oss;
            oss << label << " verification failed at transfer offset 0x"
                << std::hex << (offset + i)
                << ": got 0x" << static_cast<unsigned>(bytes[i])
                << ", expected 0x" << static_cast<unsigned>(kPatternByte);
            throw std::runtime_error(oss.str());
        }
    }
}

#if SLASH_HIP

#include <hip/hip_runtime.h>
#include <hsa/hsa.h>
#include <hsa/hsa_ext_amd.h>

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

static void sdma_copy_async(const char* label,
                            void* dst,
                            const void* src,
                            size_t bytes,
                            hipStream_t stream)
{
    hipError_t err = hipMemcpyAsync(dst, src, bytes, hipMemcpyDeviceToDevice, stream);
    if (err != hipSuccess) {
        std::ostringstream oss;
        oss << "hipMemcpyAsync(" << label << ") failed: "
            << hipGetErrorString(err)
            << ". ROCm SDMA may not accept this imported foreign BAR mapping "
            << "as a DeviceToDevice copy source/destination, even though HIP "
            << "kernels can access it via shader loads/stores.";
        throw std::runtime_error(oss.str());
    }
}

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

struct HipStream {
    hipStream_t stream = nullptr;
    HipStream() { HIP_CHECK(hipStreamCreate(&stream)); }
    ~HipStream() { if (stream) (void)hipStreamDestroy(stream); }
    HipStream(const HipStream&) = delete;
    HipStream& operator=(const HipStream&) = delete;
};

struct HipEvent {
    hipEvent_t event = nullptr;
    HipEvent() { HIP_CHECK(hipEventCreate(&event)); }
    ~HipEvent() { if (event) (void)hipEventDestroy(event); }
    HipEvent(const HipEvent&) = delete;
    HipEvent& operator=(const HipEvent&) = delete;
};

static void verify_device_pattern(const char* label, uint8_t* device_ptr, uint64_t bytes)
{
    for (const VerifyChunk& chunk : verify_chunks(bytes)) {
        std::vector<uint8_t> host(chunk.size);
        HIP_CHECK(hipMemcpy(host.data(), device_ptr + chunk.offset, chunk.size,
                            hipMemcpyDeviceToHost));
        verify_pattern(host, chunk.offset, label);
    }
}

static void verify_bar_pattern(const char* label,
                               uint8_t* bar_ptr,
                               uint64_t bytes,
                               hipStream_t stream)
{
    DeviceBuffer<uint8_t> sample(kVerifyChunkBytes);
    for (const VerifyChunk& chunk : verify_chunks(bytes)) {
        std::vector<uint8_t> host(chunk.size);
        sdma_copy_async("verify BAR -> GPU", sample.ptr, bar_ptr + chunk.offset,
                        chunk.size, stream);
        HIP_CHECK(hipStreamSynchronize(stream));
        HIP_CHECK(hipMemcpy(host.data(), sample.ptr, chunk.size, hipMemcpyDeviceToHost));
        verify_pattern(host, chunk.offset, label);
    }
}

static std::vector<float> run_direction(const char* label,
                                        void* dst,
                                        const void* src,
                                        uint64_t bytes,
                                        uint32_t warmup,
                                        uint32_t iters,
                                        hipStream_t stream)
{
    for (uint32_t i = 0; i < warmup; ++i)
        sdma_copy_async(label, dst, src, static_cast<size_t>(bytes), stream);
    HIP_CHECK(hipStreamSynchronize(stream));

    std::vector<float> samples;
    samples.reserve(iters);
    HipEvent start;
    HipEvent stop;
    for (uint32_t i = 0; i < iters; ++i) {
        HIP_CHECK(hipEventRecord(start.event, stream));
        sdma_copy_async(label, dst, src, static_cast<size_t>(bytes), stream);
        HIP_CHECK(hipEventRecord(stop.event, stream));
        HIP_CHECK(hipEventSynchronize(stop.event));

        float elapsed_ms = 0.0f;
        HIP_CHECK(hipEventElapsedTime(&elapsed_ms, start.event, stop.event));
        samples.push_back(elapsed_ms);
    }
    return samples;
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
    if (!bar_info.ptr->usable) {
        throw std::runtime_error(
            "BAR" + std::to_string(kBarNumber) +
            " is not usable. The bitstream may not mark PF2 BAR" +
            std::to_string(kBarNumber) +
            " prefetchable, or pci_p2pdma_add_resource() failed at probe.");
    }

    const uint64_t scratch_bar_offset = kBarCtrlOffset + opt.scratch_offset;
    if (scratch_bar_offset >= static_cast<uint64_t>(bar_info.ptr->length))
        throw std::runtime_error("scratch BAR offset exceeds BAR length");

    const uint64_t aperture_limit = kSharedApertureBytes - opt.scratch_offset;
    const uint64_t bar_limit = static_cast<uint64_t>(bar_info.ptr->length) - scratch_bar_offset;
    const uint64_t max_bytes = std::min(aperture_limit, bar_limit);
    uint64_t bytes = opt.requested_bytes;
    if (max_bytes == 0)
        throw std::runtime_error("scratch range has zero available bytes");
    if (bytes > max_bytes) {
        bytes = max_bytes;
        std::cout << "Requested transfer size clamped to " << bytes
                  << " bytes by BAR/shared-aperture limits\n";
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

    auto* bar_bytes = reinterpret_cast<uint8_t*>(map.ptr);
    uint8_t* scratch = bar_bytes + scratch_bar_offset;

    std::cout << "BAR" << kBarNumber << " mapped into GPU VA: " << map.ptr
              << "  mapped_size=" << map.size << "\n";
    std::cout << "scratch BAR offset=0x" << std::hex << scratch_bar_offset
              << "  aperture offset=0x" << opt.scratch_offset
              << "  bytes=0x" << bytes << std::dec << "\n";
    std::cout << "mode=sdma direction=" << direction_str(opt.direction)
              << " bytes=" << bytes
              << " iters=" << opt.iters
              << " warmup=" << opt.warmup
              << " verify=" << (opt.verify ? "on" : "off") << "\n";

    DeviceBuffer<uint8_t> gpu_buffer(static_cast<size_t>(bytes));
    HIP_CHECK(hipMemset(gpu_buffer.ptr, kPatternByte, static_cast<size_t>(bytes)));
    HipStream stream;

    if (opt.direction & kDirectionWrite) {
        std::vector<float> write_samples = run_direction(
            "GPU -> BAR", scratch, gpu_buffer.ptr, bytes, opt.warmup, opt.iters,
            stream.stream);
        if (opt.verify)
            verify_bar_pattern("write", scratch, bytes, stream.stream);
        print_stats("write", bytes, write_samples);
    }

    if (opt.direction & kDirectionRead) {
        if (opt.verify) {
            sdma_copy_async("read verification initialize GPU -> BAR",
                            scratch, gpu_buffer.ptr, static_cast<size_t>(bytes),
                            stream.stream);
            HIP_CHECK(hipStreamSynchronize(stream.stream));
        }

        std::vector<float> read_samples = run_direction(
            "BAR -> GPU", gpu_buffer.ptr, scratch, bytes, opt.warmup, opt.iters,
            stream.stream);
        if (opt.verify)
            verify_device_pattern("read", gpu_buffer.ptr, bytes);
        print_stats("read", bytes, read_samples);
    }

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
