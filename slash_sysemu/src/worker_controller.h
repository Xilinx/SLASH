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

#include "system_map.h"
#include "transport.h"

namespace slash_sysemu {

class ModelClient;

// ─────────────────────────────────────────────────────────────────────────────
// WorkerController — pluggable model control worker lifecycle hook
// ─────────────────────────────────────────────────────────────────────────────
//
// The model control workers (Step 8) are set up and torn down together with the
// model process (architecture: "Run for the entire lifetime of the model
// process ... Set up and torn down together with their model process").  Step 6
// (reconfiguration) owns the model process lifecycle but not the worker internals,
// so it drives the workers through this abstract interface: on a successful NEW
// model process, the reconfiguration stops the OLD workers and starts NEW ones
// bound to the new ModelClient and SystemMap.
//
// The interface is intentionally minimal and paired: start() and stop() bracket
// the lifetime of one set of workers.  Step 8 provides the concrete
// implementation; Step 6 tests use a counting stub.  It is passed to the
// reconfiguration as a nullable shared_ptr — a null controller means "no workers
// to manage" (valid for tests and for early bring-up).
//
// IMPORTANT: start() must NOT send the global `start` verb to the model — the
// ModelProcess handle already issues that one-time sim-clock start as its launch
// probe.  A WorkerController::start that re-sent it would double-start the sim.
class WorkerController {
public:
    virtual ~WorkerController() = default;

    /**
     * @brief Start the model control workers against a live model.
     *
     * Called after a new model process is up and its ModelClient has connected
     * and been probed.  @p client and @p map remain valid (owned by the
     * ModelProcess) for as long as the workers run, i.e. until the matching
     * stop().
     *
     * @return ok on success; an error propagates as a reconfiguration failure.
     */
    virtual Result<void> start(ModelClient& client, const SystemMap& map) = 0;

    /**
     * @brief Stop and join all model control workers.
     *
     * Must be idempotent and safe to call even if start() was never called or
     * already failed.  Called before the old model process is torn down, and
     * before starting a new set of workers.
     */
    virtual void stop() = 0;
};

} // namespace slash_sysemu
