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

#include "model_process.h"
#include "system_map.h"
#include "vbin_store.h"
#include "worker_controller.h"

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace slash_emu {

// ─────────────────────────────────────────────────────────────────────────────
// ModelInstance — owns one accelerator's model process + VBIN store, and
//                 implements the reconfiguration procedure.
// ─────────────────────────────────────────────────────────────────────────────
//
// This is the Step 6 realisation of the architecture's "Model process and
// reconfiguration" section.  A ModelInstance holds:
//   * a VbinStore (per-BDF main + staging VBIN files),
//   * the currently-running ModelProcess (if any),
//   * a pluggable WorkerController (Step 8 fills in the real one; may be null),
//   * the default-VBIN source and the launch timeouts / death callback.
//
// reconfigure() runs the exact algorithm from "Launching the model process and
// reconfiguration".  Step 11 (accelerator lifecycle/hotplug) will own instances
// of this class and call reconfigure()/teardown() under its lifecycle lock; the
// model process death callback is wired by Step 11 to tear the accelerator down.

/** Outcome of a reconfigure() call, for the caller (Step 11) to act on. */
enum class ReconfigureStatus {
    NewProcess,   /**< A new model process was launched and adopted. */
    Unchanged,    /**< A process was already running and staging was empty/failed:
                       reconfiguration was a no-op, the old process keeps running. */
    Failed,       /**< No process is running and neither staging nor main could be
                       launched: the caller must tear the accelerator down. */
};

struct ReconfigureResult {
    ReconfigureStatus status;
    std::string       message; /**< Human-readable detail, esp. on Failed. */

    [[nodiscard]] bool ok() const noexcept { return status != ReconfigureStatus::Failed; }
};

class ModelInstance {
public:
    /**
     * @brief Construct an instance for @p bdf under @p base_dir.
     *
     * @param base_dir      Daemon base directory (VBIN files live under
     *                      <base_dir>/<bdf>/).
     * @param bdf           Validated board BDF string.
     * @param default_vbin  Default VBIN used to bootstrap a fresh accelerator.
     * @param workers       Worker lifecycle hook (may be null → no workers).
     * @param on_death_gen  Model-process death callback (may be null).  Its
     *                      argument is the GENERATION of the process that died —
     *                      a globally-unique id (per @p gen_counter) assigned at
     *                      that process's launch.  The caller uses it to ignore a
     *                      STALE death (a process already replaced by a newer one).
     * @param gen_counter   Shared, never-reset generation counter.  Each launch
     *                      draws a fresh generation from it; a successful adopt
     *                      records it as current_generation().  Shared (not owned)
     *                      so it SURVIVES this ModelInstance being reset()/recreated
     *                      by the owning Accelerator across model restarts.
     * @param timeouts      Launch/teardown timeouts.
     */
    ModelInstance(std::filesystem::path        base_dir,
                  std::string                  bdf,
                  std::filesystem::path        default_vbin,
                  std::shared_ptr<WorkerController>            workers = nullptr,
                  std::function<void(uint64_t)>               on_death_gen = {},
                  std::shared_ptr<std::atomic<uint64_t>>      gen_counter = nullptr,
                  const ModelProcessTimeouts&  timeouts = {});

    ModelInstance(const ModelInstance&)            = delete;
    ModelInstance& operator=(const ModelInstance&) = delete;

    ~ModelInstance();

    /**
     * @brief Run the reconfiguration procedure.
     *
     * Algorithm (verbatim from the architecture):
     *   1. If no main VBIN exists yet: bootstrap from the default (copy
     *      default→main, create empty staging).
     *   2. If staging is non-empty: try to unpack+launch the staging VBIN.
     *      On success, adopt the new process AND replace main with staging.
     *      Clear staging in EITHER case (success or failure).
     *   3. If no process is running AND (staging was empty OR its launch failed):
     *      try to unpack+launch the main VBIN.  On success adopt it; on failure
     *      reconfiguration FAILS (caller tears the accelerator down).
     *   4. If a process is already running and staging was empty/failed: the old
     *      process keeps running — reconfiguration is a no-op (Unchanged).
     *
     * On adopting a NEW process, the old workers are stopped and new workers are
     * started (via the WorkerController) bound to the new client + system map.
     */
    ReconfigureResult reconfigure();

    /**
     * @brief Tear down the running model process and its workers, if any.
     *
     * Stops the workers, then tears down the model process.  The VBIN files are
     * PRESERVED (per spec: only cold_reboot_cleanup removes them).  Idempotent.
     */
    void teardown();

    // ── Introspection ────────────────────────────────────────────────────────

    /** True if a model process is currently running. */
    [[nodiscard]] bool has_process() const noexcept { return process_ != nullptr; }
    /** The running model process (nullptr if none). */
    [[nodiscard]] ModelProcess* process() const noexcept { return process_.get(); }
    /** The underlying VBIN store. */
    [[nodiscard]] VbinStore&       store() noexcept { return store_; }
    [[nodiscard]] const VbinStore& store() const noexcept { return store_; }

    /**
     * @brief The generation of the currently-adopted process (0 if none).
     *
     * A globally-unique id assigned at the running process's launch.  Used by the
     * death path to distinguish a stale death (old generation) from the current
     * process.  Set only on a successful adopt, under the caller's lifecycle lock.
     */
    [[nodiscard]] uint64_t current_generation() const noexcept { return current_gen_; }

private:
    // Adopt @p proc as the new running process: stop old workers, swap process,
    // start new workers.  On worker-start failure, the new process is torn down
    // and false is returned.
    bool adopt(std::unique_ptr<ModelProcess> proc, std::string& err);

    VbinStore                              store_;
    std::filesystem::path                  default_vbin_;
    std::shared_ptr<WorkerController>      workers_;
    std::function<void(uint64_t)>          on_death_gen_;
    std::shared_ptr<std::atomic<uint64_t>> gen_counter_;
    ModelProcessTimeouts                   timeouts_;

    std::unique_ptr<ModelProcess>          process_;
    uint64_t                               current_gen_{0};
};

} // namespace slash_emu
