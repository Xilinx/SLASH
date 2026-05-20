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

/// @file inspect.cpp
/// @brief Implementation of the Inspect (and Query) command.
///
/// Reads vbin metadata - either from a vbin file on disk or from the
/// system-map of whatever was last loaded on a device - and prints
/// kernel information (name, physical address, arguments) in text or JSON.

#include "inspect.hpp"

#include <charconv>
#include <filesystem>
#include <iostream>
#include <map>
#include <sstream>

#include <vrt/parser/utilization_parser.hpp>
#include <vrt/vrtbin.hpp>

#include "bdf.hpp"

#include "utils.hpp"

//. BDF string corresponding to the all-ones sentinel value (0xFFFF).
///
/// Passed to vrt::Vrtbin when we're inspecting a file and have no real device.
/// This will only determine the name of the path where the vbin is extracted.
/// This BDF should never occur in reality.
constexpr char BDF_SENTINEL[] = "FF:1F.7";


// ---------------------------------------------------------------------------
// Direction helpers
// ---------------------------------------------------------------------------

/// Converts readable/writable flags into a human-readable direction string.
/// @return "Read", "Write", "ReadWrite", or "" if neither flag is set.
std::string directionToString(bool readable, bool writable) {
    std::stringstream ss;

    ss << (readable ? "Read" : "") << (writable ? "Write" : "");

    return ss.str();
}

/// Convenience overload that extracts the flags from a FunctionalArg.
std::string directionToString(const vrt::FunctionalArg& arg) {
    return directionToString(arg.readable, arg.writable);
}

// ---------------------------------------------------------------------------
// FunctionalArg formatting (text & JSON)
// ---------------------------------------------------------------------------

/// Human-readable output for a single kernel argument.
std::ostream& operator<<(std::ostream& out, const vrt::FunctionalArg& arg) {
    return out
        << INDENT2 << "Argument:\n"
        << INDENT3 << "Index: " << arg.idx << "\n"
        << INDENT3 << "Name: " << arg.name << "\n"
        << INDENT3 << "Type: " << arg.type << "\n"
        << INDENT3 << "Offset: " << arg.offset << "\n"
        << INDENT3 << "Range: " << arg.range << "\n"
        << INDENT3 << "Direction: " << directionToString(arg) << "\n";
}

/// JSON representation of a single kernel argument.
Json::Value toJson(const vrt::FunctionalArg& arg) {
    Json::Value j;

    j["index"] = toHexString(arg.idx);
    j["name"] = arg.name;
    j["type"] = arg.type;
    j["offset"] = toHexString(arg.offset); // Prevent JSON number issues
    j["range"] = toHexString(arg.range); // Prevent JSON number issues
    j["direction"] = directionToString(arg);

    return j;
}

// ---------------------------------------------------------------------------
// KernelData — lightweight snapshot of a vrt::Kernel
// ---------------------------------------------------------------------------

/// @brief Holds the subset of vrt::Kernel data needed for display.
struct KernelData {
    std::string name;                       ///< Kernel name from the system map.
    uint64_t physAddress{};                 ///< Physical (mapped) address of the kernel.
    std::vector<vrt::FunctionalArg> args;   ///< HLS functional arguments.

    /// Extracts display-relevant data from a live vrt::Kernel object.
    static KernelData fromKernel(const vrt::Kernel& kernel) {
        return KernelData {
            .name{kernel.getName()},
            .physAddress{kernel.getPhysAddr()},
            .args{kernel.getFunctionalArgs()},
        };
    }
};

/// Human-readable output for a kernel and its arguments.
std::ostream& operator<<(std::ostream& out, const KernelData& kernel) {
    out
        << INDENT1 << "Kernel:\n"
        << INDENT2 << "Name: " << kernel.name << "\n"
        << INDENT2 << "Physical address: " << toHexString(kernel.physAddress) << "\n";

    for (const auto& arg : kernel.args) {
        out << arg;
    }

    return out;
}

/// JSON representation of a kernel and its arguments.
Json::Value toJson(const KernelData& kernel) {
    Json::Value j;

    j["name"] = kernel.name;
    j["address"] = toHexString(kernel.physAddress);

    if (!kernel.args.empty()) {
        j["args"] = Json::Value(Json::arrayValue);

        for (const auto& arg : kernel.args) {
            j["args"].append(toJson(arg));
        }
    }
    
    return j;
}

// ---------------------------------------------------------------------------
// Utilization formatting (text & JSON)
// ---------------------------------------------------------------------------

/// Helper to append " (X.XX%)" to a stream if a percentage is present.
void streamPct(std::ostream& out, const std::optional<float>& pct) {
    if (pct) {
        out << " (" << *pct << "%)";
    }
}

/// Human-readable output for resource metrics.
std::ostream& operator<<(std::ostream& out, const vrt::ResourceMetrics& m) {
    out << "LUTs: " << m.totalLuts;
    streamPct(out, m.totalLutsPct);
    out << ", FFs: " << m.ff;
    streamPct(out, m.ffPct);
    out << ", LUTRAM: " << m.lutram;
    streamPct(out, m.lutramPct);
    out << ", SRL: " << m.srl;
    streamPct(out, m.srlPct);
    out << ", RAMB36: " << m.ramb36;
    streamPct(out, m.ramb36Pct);
    out << ", RAMB18: " << m.ramb18;
    streamPct(out, m.ramb18Pct);
    out << ", URAM: " << m.uram;
    streamPct(out, m.uramPct);
    out << ", DSP: " << m.dsp;
    streamPct(out, m.dspPct);
    return out;
}

/// JSON representation of resource metrics.
Json::Value toJson(const vrt::ResourceMetrics& m) {
    Json::Value j;
    j["total_luts"] = m.totalLuts;
    if (m.totalLutsPct) j["total_luts_pct"] = *m.totalLutsPct;
    j["lutram"] = m.lutram;
    if (m.lutramPct) j["lutram_pct"] = *m.lutramPct;
    j["srl"] = m.srl;
    if (m.srlPct) j["srl_pct"] = *m.srlPct;
    j["ff"] = m.ff;
    if (m.ffPct) j["ff_pct"] = *m.ffPct;
    j["ramb36"] = m.ramb36;
    if (m.ramb36Pct) j["ramb36_pct"] = *m.ramb36Pct;
    j["ramb18"] = m.ramb18;
    if (m.ramb18Pct) j["ramb18_pct"] = *m.ramb18Pct;
    j["ramb"] = m.ramb;
    j["uram"] = m.uram;
    if (m.uramPct) j["uram_pct"] = *m.uramPct;
    j["dsp"] = m.dsp;
    if (m.dspPct) j["dsp_pct"] = *m.dspPct;
    return j;
}

/// JSON representation of a utilization cell.
Json::Value toJson(const vrt::UtilizationCell& cell) {
    Json::Value j;
    j["instance"] = cell.instance;
    j["module"] = cell.module;
    j["metrics"] = toJson(cell.metrics);
    return j;
}

/// JSON representation of a utilization block.
Json::Value toJson(const vrt::UtilizationBlock& block) {
    Json::Value j;
    j["totals"] = toJson(block.totals);
    if (block.subhierarchy) {
        const auto& sub = *block.subhierarchy;
        if (!sub.cells.empty()) {
            j["cells"] = Json::Value(Json::arrayValue);
            for (const auto& cell : sub.cells) {
                j["cells"].append(toJson(cell));
            }
        }
        if (!sub.slashLogic.empty()) {
            j["slash_logic"] = Json::Value(Json::arrayValue);
            for (const auto& cell : sub.slashLogic) {
                j["slash_logic"].append(toJson(cell));
            }
        }
        j["subhierarchy_sum"] = toJson(sub.subhierarchySum);
        j["slash_logic_sum"] = toJson(sub.slashLogicSum);
    }
    return j;
}

/// JSON representation of the full utilization report.
Json::Value toJson(const vrt::UtilizationReport& report) {
    Json::Value j;
    j["slash"] = toJson(report.slash);
    if (report.serviceLayer) {
        j["service_layer"] = toJson(*report.serviceLayer);
    }
    return j;
}

/// Human-readable output for a utilization block.
void printBlock(std::ostream& out, const vrt::UtilizationBlock& block, const char* indent) {
    out << indent << block.name << ": " << block.totals << "\n";
    if (block.subhierarchy) {
        const auto& sub = *block.subhierarchy;
        if (!sub.cells.empty()) {
            out << indent << INDENT1 << "Cells:\n";
            for (const auto& cell : sub.cells) {
                out << indent << INDENT2 << cell.instance
                    << " (" << cell.module << "): " << cell.metrics << "\n";
            }
        }
        if (!sub.slashLogic.empty()) {
            out << indent << INDENT1 << "Slash logic:\n";
            for (const auto& cell : sub.slashLogic) {
                out << indent << INDENT2 << cell.instance
                    << " (" << cell.module << "): " << cell.metrics << "\n";
            }
        }
    }
}

/// Human-readable output for the full utilization report.
std::ostream& operator<<(std::ostream& out, const vrt::UtilizationReport& report) {
    printBlock(out, report.slash, INDENT2);
    if (report.serviceLayer) {
        printBlock(out, *report.serviceLayer, INDENT2);
    }
    return out;
}

// ---------------------------------------------------------------------------
// VbinData — lightweight snapshot of a whole vbin / system-map
// ---------------------------------------------------------------------------

/// @brief Holds the metadata extracted from a vbin or a device's system map.
struct VbinData {
    std::string name{};                              ///< Display label (file path or "on <BDF>").
    vrt::Platform platform{vrt::Platform::UNKNOWN};  ///< Target platform (HW / emulation / sim).
    uint64_t clockFrequency{};                       ///< Design clock frequency in Hz.
    std::map<std::string, KernelData> kernels;       ///< Kernels keyed by name.
    std::optional<vrt::UtilizationReport> utilization; ///< Utilization report (if present).

    /// Builds a VbinData from an already-parsed system-map XMLParser.
    static VbinData fromParser(vrt::XMLParser& parser, const std::string& name) {
        std::map<std::string, KernelData> kernels;

        for (const auto& [kernelName, kernel] : parser.getKernels()) {
            kernels.emplace(kernelName, KernelData::fromKernel(kernel));
        }

        return VbinData {
            .name{name},
            .platform{parser.getPlatform()},
            .clockFrequency{parser.getClockFrequency()},
            .kernels{std::move(kernels)},
        };
    }

    /// Builds a VbinData from a vrt::Vrtbin that has already been opened.
    static VbinData fromVbin(vrt::Vrtbin& vbin, const std::string& name) {
        vrt::XMLParser parser{vbin.getSystemMapPath()};
        parser.parseXML();

        auto data = fromParser(parser, name);

        const auto utilPath = vbin.getUtilizationReportPath();
        if (!utilPath.empty() && std::filesystem::exists(utilPath)) {
            vrt::UtilizationParser utilParser{utilPath};
            utilParser.parse();
            data.utilization = utilParser.getReport();
        }

        return data;
    }

    /// Builds a VbinData by querying the system map currently loaded on
    /// the device at the given BDF address.
    static VbinData fromBdf(const std::string& bdf) {
        const std::string mapPath = vrt::Vrtbin::getSystemMapPathFromBdf(bdf);

        if (!std::filesystem::exists(mapPath)) {
            throw std::runtime_error(
                "No vbin has been programmed on device " + bdf +
                " (system map not found: " + mapPath + ")");
        }

        if ((std::filesystem::status(mapPath).permissions() & std::filesystem::perms::owner_read) ==
            std::filesystem::perms::none) {
            throw std::runtime_error(
                "Cannot read system map for device " + bdf +
                " (permission denied: " + mapPath + ")");
        }

        vrt::XMLParser parser{mapPath};
        parser.parseXML();

        auto data = fromParser(parser, "on " + bdf);

        const std::string utilPath = vrt::Vrtbin::getUtilizationReportPathFromBdf(bdf);
        if (std::filesystem::exists(utilPath)) {
            vrt::UtilizationParser utilParser{utilPath};
            utilParser.parse();
            data.utilization = utilParser.getReport();
        }

        return data;
    }

    /// Builds a VbinData by extracting and parsing a vbin file on disk.
    static VbinData fromPath(const std::string& path) {
        // BDF_SENTINEL is used because Vrtbin requires a BDF string even
        // when we only need to inspect the file contents, not target a device.
        vrt::Vrtbin vbin{path, BDF_SENTINEL};
        vbin.extract();

        return fromVbin(vbin, path);
    }
};

/// Converts a vrt::Platform enum to its string name.
const char* toString(vrt::Platform platform) {
    switch (platform) {
    case vrt::Platform::HARDWARE:
        return "HARDWARE";
    case vrt::Platform::EMULATION:
        return "EMULATION";
    case vrt::Platform::SIMULATION:
        return "SIMULATION";
    default:
        return "UNKNOWN";
    }
}

/// Human-readable output for an entire vbin's metadata.
std::ostream& operator<<(std::ostream& out, const VbinData& vbin) {
    out
        << "Vbin " << vbin.name << ":\n"
        << INDENT1 << "Platform: " << toString(vbin.platform) << "\n"
        << INDENT1 << "Clock frequency: " << vbin.clockFrequency << "\n";

    if (vbin.utilization) {
        out << INDENT1 << "Utilization:\n" << *vbin.utilization;
    }

    for (const auto& [_, kernel] : vbin.kernels) {
        out << kernel;
    }

    return out;
}

/// JSON representation of an entire vbin's metadata.
Json::Value toJson(const VbinData& vbin) {
    Json::Value j;

    j["clock_frequency"] = toHexString(vbin.clockFrequency);

    if (vbin.utilization) {
        j["utilization"] = toJson(*vbin.utilization);
    }

    if (!vbin.kernels.empty()) {
        j["kernels"] = Json::Value{};

        for (const auto& [name, kernel] : vbin.kernels) {
            j["kernels"][name] = toJson(kernel);
        }
    }

    return j;
}

// ---------------------------------------------------------------------------
// Command entry-point
// ---------------------------------------------------------------------------

/// Loads and reads the data source (BDF query or file path) based on the options.
VbinData getVbinData(const Inspect::Options& options) {
    if (options.isBdfQuery) {
        std::string bdf = resolveBoardBdf(options.bdf, "query");
        return VbinData::fromBdf(bdf);
    } else {
        return VbinData::fromPath(options.vbinPath);
    }
}

/// Runs the inspect/query command: loads vbin metadata and prints it.
int Inspect::run(const Options& options) {
    const auto vbinData{getVbinData(options)};

    print(vbinData, options.jsonOutput, options.prettyJsonOutput);

    return 0;
}
