// ################################################################################################
//  The MIT License (MIT)
//  Copyright (c) 2025 Advanced Micro Devices, Inc. All rights reserved.
//
//  Permission is hereby granted, free of charge, to any person obtaining a copy of this software
//  and associated documentation files (the "Software"), to deal in the Software without
//  restriction, including without limitation the rights to use, copy, modify, merge, publish,
//  distribute, sublicense, and/or sell copies of the Software, and to permit persons to whom the
//  Software is furnished to do so, subject to the following conditions:
//
//  The above copyright notice and this permission notice shall be included in all copies or
//  substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING
// BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
// ################################################################################################

#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace slash_emu {

// ─────────────────────────────────────────────────────────────────────────────
// VbinError — dedicated error type for VBIN / system-map parsing
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Category of a VBIN/system-map error.
 *
 * These errors are neither transport (OS socket) nor protocol (wire framing)
 * failures, so they use a dedicated type rather than extending ErrorKind in
 * transport.h.  Keeping the taxonomy separate avoids conflating on-disk
 * container/parse faults with live-connection faults.
 */
enum class VbinErrorKind {
    Io,       /**< Filesystem access failure (open/read/create temp dir). */
    Archive,  /**< Corrupt/unsupported container: bad tar header, gzip, traversal. */
    Contents, /**< Well-formed archive missing a required member (map / executable). */
    Parse,    /**< Malformed system_map.xml: bad XML, missing/invalid fields. */
};

struct VbinError {
    VbinErrorKind kind;
    std::string   message;
};

/**
 * @brief Lightweight Result<T, VbinError>, mirroring transport.h's Result<T>.
 *
 * A separate template (rather than reusing transport.h's Result) keeps the
 * error payload strongly typed as VbinError.  Use ok() / err() to construct.
 */
template <typename T>
class VbinResult {
public:
    static VbinResult ok(T v) {
        VbinResult r;
        r.storage_ = std::move(v);
        return r;
    }

    static VbinResult err(VbinError e) {
        VbinResult r;
        r.storage_ = std::move(e);
        return r;
    }

    [[nodiscard]] bool has_value() const noexcept {
        return std::holds_alternative<T>(storage_);
    }
    [[nodiscard]] explicit operator bool() const noexcept { return has_value(); }

    [[nodiscard]] T& value() & { return std::get<T>(storage_); }
    [[nodiscard]] const T& value() const& { return std::get<T>(storage_); }
    [[nodiscard]] T value() && { return std::get<T>(std::move(storage_)); }

    [[nodiscard]] VbinError& error() & { return std::get<VbinError>(storage_); }
    [[nodiscard]] const VbinError& error() const& { return std::get<VbinError>(storage_); }

private:
    std::variant<T, VbinError> storage_;
};

// ─────────────────────────────────────────────────────────────────────────────
// Data model
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Target platform recorded in <Platform> of system_map.xml.
 *
 * The daemon only ever runs Emulation/Simulation VBINs; Hardware is classified
 * so callers can reject it with a clear error rather than silently accepting.
 */
enum class Platform {
    Hardware,
    Emulation,
    Simulation,
    Unknown,
};

/** @brief Human-readable name for a Platform value ("Hardware", ...). */
const char* platform_name(Platform p) noexcept;

/**
 * @brief A single control register of a kernel.
 *
 * `offset` is the byte offset within the kernel's BAR window (hex in the XML).
 * `bit_width` is the register width in bits (decimal `range` attribute).
 *
 * @note `bit_width` may legitimately be 0 (`range="0"` is accepted).  Downstream
 *       mask/shift math must therefore guard against a 0 (or oversized) width and
 *       never shift-by-width unconditionally.
 */
struct Register {
    std::string name;
    uint64_t    offset{0};
    std::string access;      /**< "R", "W", or "RW". */
    uint32_t    bit_width{0};
    std::string description;
};

/**
 * @brief A kernel functional argument (from <functional_args><arg .../>).
 */
struct FunctionalArg {
    uint32_t    idx{0};
    std::string name;
    std::string type;        /**< "scalar" or "buffer". */
    uint64_t    offset{0};
    uint32_t    bit_width{0};
    bool        readable{false};
    bool        writable{false};
    std::string port;        /**< AXI port name for buffer args; empty otherwise. */
};

/**
 * @brief A port→memory connection (<connection port=... target=.../>).
 */
struct Connection {
    std::string port;
    std::string target;      /**< e.g. "HBM0", "DDR", "MEM". */
};

/**
 * @brief A compute kernel and its register/argument/connection model.
 */
struct Kernel {
    std::string                name;
    uint64_t                   base_address{0};
    uint64_t                   range{0};
    std::vector<Register>      registers;
    std::vector<FunctionalArg> args;
    std::vector<Connection>    connections;

    /**
     * @brief Find a register by exact name; nullptr if absent.
     * @note Downstream (Step 8) resolves output/return registers by name.
     */
    [[nodiscard]] const Register* find_register(const std::string& reg_name) const;

    /**
     * @brief Find a register by byte offset; nullptr if absent.
     * @note The control register (ap_start/ap_done) is expected at offset 0.
     *       The parser does NOT enforce its presence: a kernel may legitimately
     *       have no offset-0 register (e.g. streaming/free-running kernels), so
     *       downstream (Step 8) must handle register_at(0) == nullptr rather than
     *       assuming a control register always exists.
     */
    [[nodiscard]] const Register* register_at(uint64_t offset) const;

    /** @brief Find a functional argument by name; nullptr if absent. */
    [[nodiscard]] const FunctionalArg* find_arg(const std::string& arg_name) const;

    /** @brief Resolve a port name to its connection target; nullptr if absent. */
    [[nodiscard]] const Connection* find_connection(const std::string& port) const;
};

/**
 * @brief A QDMA stream connection (<Qdma> ... </Qdma>).
 */
struct QdmaConnection {
    std::string kernel;
    std::string interface;
    std::string direction;   /**< "HostToDevice" or "DeviceToHost". */
    uint32_t    qid{0};
};

/**
 * @brief Parsed representation of a system_map.xml document.
 */
struct SystemMap {
    Platform                    platform{Platform::Unknown};
    uint64_t                    clock_frequency_hz{0};
    std::vector<Kernel>         kernels;
    std::vector<QdmaConnection> qdma;

    /** @brief Find a kernel by exact name; nullptr if absent. */
    [[nodiscard]] const Kernel* find_kernel(const std::string& kernel_name) const;
};

// ─────────────────────────────────────────────────────────────────────────────
// Parsing entry points
// ─────────────────────────────────────────────────────────────────────────────

/**
 * @brief Parse a system_map.xml file at @p path.
 *
 * Uses libxml2.  Required elements/attributes that are missing or malformed
 * produce a VbinError with kind == VbinErrorKind::Parse and a message naming
 * the offending file and field.  An unknown <Platform> value is an error;
 * Hardware is accepted and classified (callers decide whether to reject it).
 */
VbinResult<SystemMap> parse_system_map_file(const std::string& path);

/**
 * @brief Parse a system_map.xml document from an in-memory buffer.
 *
 * @param xml    The document bytes.
 * @param origin A label used only in error messages (e.g. the source filename).
 */
VbinResult<SystemMap> parse_system_map_buffer(const std::string& xml,
                                              const std::string& origin);

} // namespace slash_emu
