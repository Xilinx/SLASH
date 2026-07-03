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

#include "system_map.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cstdlib>
#include <memory>
#include <optional>

#include <libxml/parser.h>
#include <libxml/tree.h>

namespace slash_emu {

// ─────────────────────────────────────────────────────────────────────────────
// Free-function helpers
// ─────────────────────────────────────────────────────────────────────────────

const char* platform_name(Platform p) noexcept {
    switch (p) {
        case Platform::Hardware:   return "Hardware";
        case Platform::Emulation:  return "Emulation";
        case Platform::Simulation: return "Simulation";
        case Platform::Unknown:    return "Unknown";
    }
    return "Unknown";
}

const Register* Kernel::find_register(const std::string& reg_name) const {
    for (const auto& r : registers) {
        if (r.name == reg_name) return &r;
    }
    return nullptr;
}

const Register* Kernel::register_at(uint64_t offset) const {
    for (const auto& r : registers) {
        if (r.offset == offset) return &r;
    }
    return nullptr;
}

const FunctionalArg* Kernel::find_arg(const std::string& arg_name) const {
    for (const auto& a : args) {
        if (a.name == arg_name) return &a;
    }
    return nullptr;
}

const Connection* Kernel::find_connection(const std::string& port) const {
    for (const auto& c : connections) {
        if (c.port == port) return &c;
    }
    return nullptr;
}

const Kernel* SystemMap::find_kernel(const std::string& kernel_name) const {
    for (const auto& k : kernels) {
        if (k.name == kernel_name) return &k;
    }
    return nullptr;
}

// ─────────────────────────────────────────────────────────────────────────────
// libxml2 RAII wrappers
// ─────────────────────────────────────────────────────────────────────────────

namespace {

// Deleter for the owning xmlDoc handle.
struct XmlDocDeleter {
    void operator()(xmlDoc* d) const noexcept {
        if (d) xmlFreeDoc(d);
    }
};
using XmlDocPtr = std::unique_ptr<xmlDoc, XmlDocDeleter>;

// Deleter for xmlChar* buffers returned by xmlGetProp / xmlNodeGetContent.
struct XmlCharDeleter {
    void operator()(xmlChar* s) const noexcept {
        if (s) xmlFree(s);
    }
};
using XmlCharPtr = std::unique_ptr<xmlChar, XmlCharDeleter>;

VbinError parse_err(std::string msg) {
    return VbinError{VbinErrorKind::Parse, std::move(msg)};
}

// True if @p node is an element with the given name.
bool is_element(const xmlNode* node, const char* name) {
    return node->type == XML_ELEMENT_NODE && xmlStrcmp(node->name, BAD_CAST name) == 0;
}

// Return the trimmed text content of an element, or an empty string.
std::string node_text(xmlNode* node) {
    XmlCharPtr content{xmlNodeGetContent(node)};
    if (!content) return {};
    std::string out(reinterpret_cast<const char*>(content.get()));
    // Trim surrounding ASCII whitespace (XML text nodes often carry newlines).
    const auto not_space = [](unsigned char c) {
        return c != ' ' && c != '\t' && c != '\n' && c != '\r';
    };
    auto begin = std::find_if(out.begin(), out.end(), not_space);
    auto end = std::find_if(out.rbegin(), out.rend(), not_space).base();
    if (begin >= end) return {};
    return std::string(begin, end);
}

// Trim surrounding ASCII whitespace (mirrors node_text so attributes and element
// text parse consistently).
std::string trim_ascii(const std::string& s) {
    const auto not_space = [](unsigned char c) {
        return c != ' ' && c != '\t' && c != '\n' && c != '\r';
    };
    auto begin = std::find_if(s.begin(), s.end(), not_space);
    auto end = std::find_if(s.rbegin(), s.rend(), not_space).base();
    if (begin >= end) return {};
    return std::string(begin, end);
}

// Return a whitespace-trimmed attribute value if present.
std::optional<std::string> node_attr(xmlNode* node, const char* name) {
    XmlCharPtr prop{xmlGetProp(node, BAD_CAST name)};
    if (!prop) return std::nullopt;
    return trim_ascii(std::string(reinterpret_cast<const char*>(prop.get())));
}

// Parse a hexadecimal-or-decimal unsigned value.  An explicit 0x/0X prefix
// selects base 16; everything else is base 10.  We deliberately do NOT use
// strtoull's base 0, which would treat a leading-zero value ("010") as octal —
// real offsets/ranges are decimal-or-hex, never octal.  The whole string must be
// consumed and non-empty; a leading '+' or '-' sign is rejected.
std::optional<uint64_t> parse_uint(const std::string& text) {
    if (text.empty()) return std::nullopt;
    // Reject any sign: '-' would wrap, '+' is never used by the schema.
    if (text[0] == '-' || text[0] == '+') return std::nullopt;
    int base = 10;
    if (text.size() > 2 && text[0] == '0' && (text[1] == 'x' || text[1] == 'X')) {
        base = 16;
    }
    errno = 0;
    char* endptr = nullptr;
    const unsigned long long v = std::strtoull(text.c_str(), &endptr, base);
    if (errno != 0 || endptr == text.c_str() || *endptr != '\0') return std::nullopt;
    return static_cast<uint64_t>(v);
}

// Parse a value that must fit in uint32_t (register/arg bit width).  A value
// exceeding 0xFFFFFFFF is a hard error (naming the field) rather than a silent
// narrowing truncation.
std::optional<uint32_t> parse_uint32(const std::string& text) {
    auto v = parse_uint(text);
    if (!v || *v > 0xFFFFFFFFull) return std::nullopt;
    return static_cast<uint32_t>(*v);
}

// ── Per-element parsers ──────────────────────────────────────────────────────

std::optional<VbinError> parse_register(xmlNode* node, const std::string& origin,
                                        const std::string& kernel_name, Register& out) {
    auto offset_s = node_attr(node, "offset");
    if (!offset_s) {
        return parse_err(origin + ": kernel '" + kernel_name +
                         "': <register> missing required 'offset' attribute");
    }
    auto offset = parse_uint(*offset_s);
    if (!offset) {
        return parse_err(origin + ": kernel '" + kernel_name +
                         "': <register> has malformed offset '" + *offset_s + "'");
    }
    out.offset = *offset;
    out.name = node_attr(node, "name").value_or("");
    out.access = node_attr(node, "access").value_or("");
    out.description = node_attr(node, "description").value_or("");

    // 'range' is the bit width in decimal; default to 32 when absent.  It must
    // fit in uint32_t — an out-of-range value is a parse error, not a truncation.
    if (auto range_s = node_attr(node, "range")) {
        auto range = parse_uint32(*range_s);
        if (!range) {
            return parse_err(origin + ": kernel '" + kernel_name + "': register '" + out.name +
                             "' has malformed or out-of-range range '" + *range_s + "'");
        }
        out.bit_width = *range;
    } else {
        out.bit_width = 32;
    }
    return std::nullopt;
}

std::optional<VbinError> parse_arg(xmlNode* node, const std::string& origin,
                                   const std::string& kernel_name, FunctionalArg& out) {
    auto idx_s = node_attr(node, "idx");
    if (!idx_s) {
        return parse_err(origin + ": kernel '" + kernel_name +
                         "': <arg> missing required 'idx' attribute");
    }
    auto idx = parse_uint(*idx_s);
    if (!idx) {
        return parse_err(origin + ": kernel '" + kernel_name + "': <arg> has malformed idx '" +
                         *idx_s + "'");
    }
    out.idx = static_cast<uint32_t>(*idx);
    out.name = node_attr(node, "name").value_or("");
    out.type = node_attr(node, "type").value_or("");
    out.port = node_attr(node, "port").value_or("");

    auto offset_s = node_attr(node, "offset");
    if (!offset_s) {
        return parse_err(origin + ": kernel '" + kernel_name + "': arg '" + out.name +
                         "' missing required 'offset' attribute");
    }
    auto offset = parse_uint(*offset_s);
    if (!offset) {
        return parse_err(origin + ": kernel '" + kernel_name + "': arg '" + out.name +
                         "' has malformed offset '" + *offset_s + "'");
    }
    out.offset = *offset;

    if (auto range_s = node_attr(node, "range")) {
        auto range = parse_uint32(*range_s);
        if (!range) {
            return parse_err(origin + ": kernel '" + kernel_name + "': arg '" + out.name +
                             "' has malformed or out-of-range range '" + *range_s + "'");
        }
        out.bit_width = *range;
    } else {
        out.bit_width = 32;
    }

    // r / w attributes are "0" or "1"; absence means false.
    out.readable = node_attr(node, "r").value_or("0") != "0";
    out.writable = node_attr(node, "w").value_or("0") != "0";
    return std::nullopt;
}

std::optional<VbinError> parse_kernel(xmlNode* kernel_node, const std::string& origin,
                                      Kernel& out) {
    bool have_name = false;
    bool have_base = false;
    bool have_range = false;

    for (xmlNode* child = kernel_node->children; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE) continue;

        if (is_element(child, "Name")) {
            out.name = node_text(child);
            have_name = true;
        } else if (is_element(child, "BaseAddress")) {
            const std::string text = node_text(child);
            auto ba = parse_uint(text);
            if (!ba) {
                return parse_err(origin + ": kernel '" + out.name +
                                 "': malformed <BaseAddress> '" + text + "'");
            }
            out.base_address = *ba;
            have_base = true;
        } else if (is_element(child, "Range")) {
            const std::string text = node_text(child);
            auto r = parse_uint(text);
            if (!r) {
                return parse_err(origin + ": kernel '" + out.name + "': malformed <Range> '" +
                                 text + "'");
            }
            out.range = *r;
            have_range = true;
        } else if (is_element(child, "register")) {
            Register reg;
            if (auto e = parse_register(child, origin, out.name, reg)) return e;
            // Reject duplicate register offsets: register_at() resolves by offset,
            // so a collision would make the shadowed register unaddressable.
            if (out.register_at(reg.offset) != nullptr) {
                return parse_err(origin + ": kernel '" + out.name +
                                 "': duplicate register offset in this kernel");
            }
            out.registers.push_back(std::move(reg));
        } else if (is_element(child, "connection")) {
            Connection conn;
            conn.port = node_attr(child, "port").value_or("");
            conn.target = node_attr(child, "target").value_or("");
            if (!conn.port.empty() && !conn.target.empty()) {
                out.connections.push_back(std::move(conn));
            }
        } else if (is_element(child, "functional_args")) {
            for (xmlNode* arg = child->children; arg; arg = arg->next) {
                if (!is_element(arg, "arg")) continue;
                FunctionalArg fa;
                if (auto e = parse_arg(arg, origin, out.name, fa)) return e;
                out.args.push_back(std::move(fa));
            }
        }
    }

    if (!have_name || out.name.empty()) {
        return parse_err(origin + ": <Kernel> missing required non-empty <Name>");
    }
    if (!have_base) {
        return parse_err(origin + ": kernel '" + out.name + "': missing required <BaseAddress>");
    }
    if (!have_range) {
        return parse_err(origin + ": kernel '" + out.name + "': missing required <Range>");
    }

    // Present args in stable idx order so downstream positional handling is simple.
    std::sort(out.args.begin(), out.args.end(),
              [](const FunctionalArg& a, const FunctionalArg& b) { return a.idx < b.idx; });
    return std::nullopt;
}

std::optional<VbinError> parse_qdma(xmlNode* qdma_node, const std::string& origin,
                                    QdmaConnection& out) {
    for (xmlNode* child = qdma_node->children; child; child = child->next) {
        if (child->type != XML_ELEMENT_NODE) continue;
        if (is_element(child, "kernel")) {
            out.kernel = node_text(child);
        } else if (is_element(child, "interface")) {
            out.interface = node_text(child);
        } else if (is_element(child, "direction")) {
            out.direction = node_text(child);
        } else if (is_element(child, "qid")) {
            const std::string text = node_text(child);
            auto qid = parse_uint(text);
            if (!qid) {
                return parse_err(origin + ": <Qdma> for kernel '" + out.kernel +
                                 "' has malformed <qid> '" + text + "'");
            }
            out.qid = static_cast<uint32_t>(*qid);
        }
    }
    return std::nullopt;
}

VbinResult<SystemMap> parse_document(xmlDoc* doc, const std::string& origin) {
    xmlNode* root = xmlDocGetRootElement(doc);
    if (!root) {
        return VbinResult<SystemMap>::err(parse_err(origin + ": document has no root element"));
    }
    if (xmlStrcmp(root->name, BAD_CAST "SystemMap") != 0) {
        return VbinResult<SystemMap>::err(parse_err(
            origin + ": unexpected root element '" +
            reinterpret_cast<const char*>(root->name) + "' (expected <SystemMap>)"));
    }

    SystemMap map;
    bool have_platform = false;

    for (xmlNode* node = root->children; node; node = node->next) {
        if (node->type != XML_ELEMENT_NODE) continue;

        if (is_element(node, "Platform")) {
            const std::string text = node_text(node);
            if (text == "Hardware") {
                map.platform = Platform::Hardware;
            } else if (text == "Emulation") {
                map.platform = Platform::Emulation;
            } else if (text == "Simulation") {
                map.platform = Platform::Simulation;
            } else {
                return VbinResult<SystemMap>::err(parse_err(
                    origin + ": unknown <Platform> value '" + text +
                    "' (expected Hardware, Emulation, or Simulation)"));
            }
            have_platform = true;
        } else if (is_element(node, "ClockFrequency")) {
            const std::string text = node_text(node);
            auto hz = parse_uint(text);
            if (!hz) {
                return VbinResult<SystemMap>::err(
                    parse_err(origin + ": malformed <ClockFrequency> '" + text + "'"));
            }
            map.clock_frequency_hz = *hz;
        } else if (is_element(node, "Kernel")) {
            Kernel k;
            if (auto e = parse_kernel(node, origin, k)) {
                return VbinResult<SystemMap>::err(std::move(*e));
            }
            // Reject duplicate kernel names: find_kernel() resolves by name, so a
            // collision would silently shadow the second kernel for addressing.
            if (map.find_kernel(k.name) != nullptr) {
                return VbinResult<SystemMap>::err(parse_err(
                    origin + ": duplicate kernel name '" + k.name + "'"));
            }
            map.kernels.push_back(std::move(k));
        } else if (is_element(node, "Qdma")) {
            QdmaConnection q;
            if (auto e = parse_qdma(node, origin, q)) {
                return VbinResult<SystemMap>::err(std::move(*e));
            }
            map.qdma.push_back(std::move(q));
        }
        // ServiceLayer and any other elements are metadata we currently ignore.
    }

    if (!have_platform) {
        return VbinResult<SystemMap>::err(
            parse_err(origin + ": missing required <Platform> element"));
    }
    return VbinResult<SystemMap>::ok(std::move(map));
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Public entry points
// ─────────────────────────────────────────────────────────────────────────────

VbinResult<SystemMap> parse_system_map_file(const std::string& path) {
    // XML_PARSE_NONET forbids network access; NOENT/NOBLANKS keep the tree lean.
    // Suppress libxml2's default stderr error reporting — we surface our own.
    XmlDocPtr doc{xmlReadFile(path.c_str(), nullptr,
                              XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR |
                                  XML_PARSE_NOWARNING)};
    if (!doc) {
        return VbinResult<SystemMap>::err(
            parse_err("system_map.xml: failed to parse XML file '" + path + "'"));
    }
    return parse_document(doc.get(), path);
}

VbinResult<SystemMap> parse_system_map_buffer(const std::string& xml, const std::string& origin) {
    if (xml.size() > static_cast<size_t>(INT_MAX)) {
        return VbinResult<SystemMap>::err(
            VbinError{VbinErrorKind::Parse, origin + ": system_map.xml is too large to parse"});
    }
    XmlDocPtr doc{xmlReadMemory(xml.data(), static_cast<int>(xml.size()), origin.c_str(), nullptr,
                               XML_PARSE_NONET | XML_PARSE_NOBLANKS | XML_PARSE_NOERROR |
                                   XML_PARSE_NOWARNING)};
    if (!doc) {
        return VbinResult<SystemMap>::err(
            parse_err(origin + ": failed to parse XML document"));
    }
    return parse_document(doc.get(), origin);
}

} // namespace slash_emu
