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

/**
 * @file bridge_op.hpp
 * @brief IBridgeOp — opaque bridge-owned cross-device sync/transfer primitive.
 *
 * Bridges define their own concrete subclasses of IBridgeOp to hold whatever
 * state they need (semaphore handles, ring indices, HSA signals, CUDA events,
 * bounce buffer storage, …). The compiler treats each instance as opaque and
 * pairs the producer- and consumer-side CompiledBridgeOpNodes through pointer
 * identity of the shared `shared_ptr<IBridgeOp>` they hold.
 *
 * Subclasses may override `label()` to provide a short human-readable
 * description used by visualisation tooling.
 */

#ifndef VRT_GRAPH_CROSSDEVICE_BRIDGE_OP_HPP
#define VRT_GRAPH_CROSSDEVICE_BRIDGE_OP_HPP

#include <memory>
#include <string>

namespace vrt::graph {

class IBridgeOp {
   public:
    virtual ~IBridgeOp() = default;

    /**
     * @brief Short human-readable name used by visualisation tooling.
     *
     * Default is `"bridge_op"`. Subclasses are encouraged to return
     * something more descriptive, e.g. `"cpu_gpu_xfer"`.
     */
    virtual std::string label() const { return "bridge_op"; }
};

}  // namespace vrt::graph

#endif  // VRT_GRAPH_CROSSDEVICE_BRIDGE_OP_HPP
