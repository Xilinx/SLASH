// SPDX-License-Identifier: MIT
/**
 * @file rdma_sideband.hpp
 * @brief The TCP connection of an RDMA session (docs/rdma-sideband.md):
 *        ASCII lines "VERB [#tag] key=value ...", one per '\n'.
 */

#ifndef VRT_RDMA_SIDEBAND_HPP
#define VRT_RDMA_SIDEBAND_HPP

#include <vrt/rdma.hpp>

#include <chrono>
#include <map>
#include <memory>
#include <string>

namespace vrt {

struct Rdma::Sideband {
    struct Line {
        std::string verb;
        long tag = -1;                              ///< -1: none
        std::map<std::string, std::string> keys;
    };

    Sideband(int fd, std::chrono::milliseconds timeout);
    ~Sideband();
    Sideband(const Sideband&) = delete;
    Sideband& operator=(const Sideband&) = delete;

    /// wait for one connection on @p port
    static std::unique_ptr<Sideband> listen(uint16_t port, std::chrono::milliseconds timeout);
    /// connect to @p host : @p port, retrying while nobody listens there yet
    static std::unique_ptr<Sideband> connect(const std::string& host, uint16_t port,
                                             std::chrono::milliseconds timeout);

    void send(const std::string& line);
    /// the next non-empty line; throws on timeout or when the peer closed
    Line receive();
    static Line parse(const std::string& line);
    /// a numeric key (decimal or 0x hex) of @p l; throws when it is missing
    static uint64_t number(const Line& l, const char* key);
    /// a key of @p l, "" when missing
    static std::string text(const Line& l, const char* key);

    int fd;
    std::chrono::milliseconds timeout;
    std::string in;                                 ///< bytes received, not yet a whole line
    long nextTag = 1;
};

}  // namespace vrt

#endif  // VRT_RDMA_SIDEBAND_HPP
