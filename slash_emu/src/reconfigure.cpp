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

#include "reconfigure.h"

#include <utility>

namespace slash_emu {

ModelInstance::ModelInstance(std::filesystem::path                  base_dir,
                             std::string                            bdf,
                             std::filesystem::path                  default_vbin,
                             std::shared_ptr<WorkerController>      workers,
                             std::function<void(uint64_t)>          on_death_gen,
                             std::shared_ptr<std::atomic<uint64_t>> gen_counter,
                             const ModelProcessTimeouts&            timeouts)
    : store_(std::move(base_dir), std::move(bdf)),
      default_vbin_(std::move(default_vbin)),
      workers_(std::move(workers)),
      on_death_gen_(std::move(on_death_gen)),
      gen_counter_(std::move(gen_counter)),
      timeouts_(timeouts) {}

ModelInstance::~ModelInstance() { teardown(); }

bool ModelInstance::adopt(std::unique_ptr<ModelProcess> proc, std::string& err) {
    // Stop the OLD workers (if any) before swapping the process they reference.
    if (workers_) {
        workers_->stop();
    }
    // Tear down the OLD process.
    if (process_) {
        process_->teardown();
        process_.reset();
    }

    // Swap in the new process, then start NEW workers bound to it.
    process_ = std::move(proc);

    if (workers_) {
        Result<void> started = workers_->start(process_->client(), process_->system_map());
        if (!started) {
            err = "worker start failed: " + started.error().message;
            // Roll back: tear the just-adopted process down again.
            process_->teardown();
            process_.reset();
            return false;
        }
    }
    return true;
}

ReconfigureResult ModelInstance::reconfigure() {
    // 1. Bootstrap: ensure main + staging exist (copy default→main if no main).
    if (VbinResult<void> b = store_.bootstrap(default_vbin_); !b) {
        return {ReconfigureStatus::Failed, "bootstrap failed: " + b.error().message};
    }

    // 2. If staging is non-empty, try to launch it.  On success we adopt it and
    //    promote staging → main and return.  On ANY failure we clear staging and
    //    fall through (per spec: staging is cleared in either case).
    if (store_.staging_nonempty()) {
        // Mint a globally-unique generation for THIS launch and bind it into a
        // per-launch void() death wrapper, so the death callback carries the exact
        // identity of the process it monitors (see the death-path generation guard).
        const uint64_t g = gen_counter_ ? (gen_counter_->fetch_add(1) + 1) : 0;
        DeathCallback per_launch = [cb = on_death_gen_, g] {
            if (cb) cb(g);
        };
        VbinResult<std::unique_ptr<ModelProcess>> launched =
            ModelProcess::launch(store_.staging_path().string(), per_launch, timeouts_);

        if (launched) {
            // Adopt the staged process BEFORE the rename; the process holds its
            // own extracted copy (its TempDir), so the staging file is free to
            // be renamed into main.
            std::string err;
            if (adopt(std::move(launched.value()), err)) {
                current_gen_ = g; // record the adopted process's generation
                // Success: promote staging → main, recreate empty staging.  The
                // promotion (rename + recreate empty staging) also satisfies the
                // "clear staging on success" requirement.
                VbinResult<void> replaced = store_.replace_main_with_staging();
                if (!replaced) {
                    // The new process is already running and adopted; a failure
                    // to promote the file is reported but does not tear it down.
                    // Per spec ("clear staging in either case"), still clear the
                    // staging buffer so a subsequent no-op reconfigure() does not
                    // see it non-empty and needlessly relaunch the same VBIN
                    // (killing and respawning the just-adopted process).
                    (void)store_.clear_staging();
                    return {ReconfigureStatus::NewProcess,
                            "new staging process adopted but main promotion failed: " +
                                replaced.error().message};
                }
                return {ReconfigureStatus::NewProcess, "launched staging VBIN"};
            }
            // Worker start failed for the staged process: adopt() already tore
            // the old + new processes down.  Fall through to try main.
        }
        // Staging launch (or its worker start) failed.  Clear staging; the old
        // process (if any) is still running unless adopt() rolled it back.
        (void)store_.clear_staging();
    }

    // If we already adopted a staging process, we returned above.  Reaching here
    // means staging was empty or its launch failed.

    // 3. If a process is already running, it keeps running: no-op.
    if (process_) {
        return {ReconfigureStatus::Unchanged,
                "existing model process retained (staging empty or failed)"};
    }

    // 4. No process running: try to launch the main VBIN.
    if (!store_.has_main()) {
        return {ReconfigureStatus::Failed, "no main VBIN to launch"};
    }
    const uint64_t gm = gen_counter_ ? (gen_counter_->fetch_add(1) + 1) : 0;
    DeathCallback main_per_launch = [cb = on_death_gen_, gm] {
        if (cb) cb(gm);
    };
    VbinResult<std::unique_ptr<ModelProcess>> main_launched =
        ModelProcess::launch(store_.main_path().string(), main_per_launch, timeouts_);
    if (!main_launched) {
        return {ReconfigureStatus::Failed,
                "main VBIN launch failed: " + main_launched.error().message};
    }
    std::string err;
    if (!adopt(std::move(main_launched.value()), err)) {
        return {ReconfigureStatus::Failed, "main VBIN " + err};
    }
    current_gen_ = gm; // record the adopted process's generation
    return {ReconfigureStatus::NewProcess, "launched main VBIN"};
}

void ModelInstance::teardown() {
    if (workers_) {
        workers_->stop();
    }
    if (process_) {
        process_->teardown();
        process_.reset();
    }
}

} // namespace slash_emu
