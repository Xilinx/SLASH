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

#include "bar_memfd.h"
#include "config.h"
#include "ctl_subsystem.h"
#include "model_control_workers.h"
#include "model_process.h"
#include "qdma_subsystem.h"
#include "reconfigure.h"
#include "transport.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <sys/types.h>

namespace slash_sysemu {

// ─────────────────────────────────────────────────────────────────────────────
// Accelerator — one emulated card (per board BDF): the six-component state machine
// ─────────────────────────────────────────────────────────────────────────────
//
// Owns the six tracked components for a single board BDF:
//   (1) the main+staging VBIN files      — via ModelInstance's VbinStore
//   (2) the model process                — via ModelInstance's ModelProcess
//   (3) the model control worker threads — via a ModelControlWorkers
//   (4) the PF0 stub                     — a single presence flag
//   (5) the QDMA subsystem (PF1)         — a QdmaSubsystem
//   (6) the BAR/device-info subsystem (PF2) — a CtlSubsystem
//
// The PF0 stub models board management: in the real system PF0 is owned by the
// AMI driver, but its REMOVE/RESCAN still goes through the slash driver, and an
// accelerator is only fully torn down once PF0 has also been removed.  The daemon
// therefore needs nothing more than a single presence flag to track it.
//
// State (derived from the component flags):
//   * Absent   — never instantiated during this run and no main.vbin on disk.
//   * Inactive — no model process running (VBIN files may exist on disk).
//   * Active   — model running AND all of PF0/PF1/PF2 present.
//   * Partial  — model running AND at least one PF absent (REMOVE of some, not all).
//
// Instantiation order: launch the model process + workers FIRST,
// then set up the QDMA (PF1) and BAR (PF2) subsystems, then mark PF0 present.
// Teardown order: tear down each PF on REMOVE; the model process + workers follow
// once the LAST PF (incl. PF0) is gone.  A full teardown PRESERVES the VBIN files
// (only the daemon's cold-reboot cleanup removes them).
//
// Threading / ownership:
//   * The BarSet and the ModelControlWorkers PERSIST across model restarts (the
//     memfds outlive any single model process); they are created once, on the
//     first instantiate(), and reused.
//   * The QdmaSubsystem borrows the current model's ModelClient by REFERENCE.  A
//     PF2-restore reconfiguration that adopts a NEW process would leave that
//     reference dangling, so PF1 is QUIESCED FIRST: qdma_->remove() joins every
//     transfer-session thread while the old client is still alive, THEN the
//     reconfigure runs (it may destroy the old process), THEN a FRESH QdmaSubsystem
//     is reconstructed against the new client.  Stale qpair state is intentionally
//     dropped — device memory does not persist across reconfiguration anyway
//     (an accepted emulation inaccuracy for now).
//   * The QdmaSubsystem is always torn down BEFORE the ModelInstance whose client
//     it borrows, so the borrowed reference never dangles under a live transfer.
//   * The model-death callback (fired on the ModelProcess monitor thread) is
//     forwarded via post_model_death (a caller-supplied poster) to the daemon's
//     lifecycle work queue — it NEVER tears down synchronously (see model_process.h
//     DeathCallback contract).
//
// All lifecycle methods below must be called with the daemon's single lifecycle
// lock held (the HotplugSubsystem owns that lock); Accelerator itself carries no
// lock.  Nothing throws across the API.

/** The high-level lifecycle state of an accelerator. */
enum class AccelState { Absent, Inactive, Active, Partial };

/** Human-readable name for an AccelState value. */
const char* accel_state_name(AccelState s) noexcept;

/** Which physical function a targeted operation addresses. */
enum class Pf { Pf0, Pf1, Pf2 };

/** The immutable per-accelerator parameters captured at first instantiation. */
struct AcceleratorParams {
    std::filesystem::path base_dir;         /**< Daemon base dir (VBINs under <base>/<bdf>/). */
    BoardBdf              bdf;               /**< Validated board BDF. */
    std::filesystem::path default_vbin;     /**< Default VBIN to bootstrap a fresh card. */
    std::string          ctl_socket_path;   /**< slash_ctl<N> path (PF2). */
    std::string          qdma_socket_path;  /**< slash_qdma_ctl<N> path (PF1). */
    std::chrono::milliseconds worker_poll{ModelControlWorkers::kDefaultPollInterval};
    ModelProcessTimeouts timeouts{};
};

class Accelerator {
public:
    /**
     * @brief Construct an (Absent) accelerator for @p params.bdf.
     *
     * No filesystem or process side effects occur here; call instantiate().
     *
     * @param params            Immutable per-accelerator parameters (the ORIGINAL
     *                          config, reused for partial-restore per the spec).
     * @param post_model_death  Poster invoked (on the ModelProcess monitor thread)
     *                          when the model dies unexpectedly.  Its argument is
     *                          the model GENERATION that died; the poster forwards
     *                          it to on_model_died() so a stale death task for an
     *                          already-replaced process is a no-op.  The poster
     *                          MUST only enqueue work onto the daemon lifecycle
     *                          queue and return promptly — never tear down inline.
     * @param gen_counter       The model-generation counter to draw launch
     *                          generations from.  MUST be shared across every
     *                          Accelerator the daemon constructs for the SAME board
     *                          over its lifetime (RESCAN replaces the Accelerator
     *                          object when a board goes Inactive; a fresh per-object
     *                          counter would restart at 0 and let a re-instantiated
     *                          process reuse a dead process's generation, defeating
     *                          the staleness guard).  Defaults to a private counter
     *                          (fine for a stand-alone Accelerator that is never
     *                          replaced, e.g. in unit tests).
     */
    Accelerator(AcceleratorParams params, std::function<void(uint64_t)> post_model_death,
                std::shared_ptr<std::atomic<uint64_t>> gen_counter = nullptr);

    Accelerator(const Accelerator&)            = delete;
    Accelerator& operator=(const Accelerator&) = delete;
    Accelerator(Accelerator&&)                 = delete;
    Accelerator& operator=(Accelerator&&)      = delete;

    /** Tears everything down in teardown order (VBIN files preserved). */
    ~Accelerator();

    // ── State (caller holds the lifecycle lock) ──────────────────────────────
    [[nodiscard]] AccelState             state() const;
    [[nodiscard]] const BoardBdf&        bdf() const noexcept { return params_.bdf; }
    [[nodiscard]] const AcceleratorParams& params() const noexcept { return params_; }
    [[nodiscard]] bool                   pf_present(Pf pf) const noexcept;
    [[nodiscard]] bool                   model_running() const noexcept;
    /** Generation of the currently-adopted model process (0 if none).  Single
     *  source of truth is the ModelInstance, set at its adopt. */
    [[nodiscard]] uint64_t generation() const noexcept {
        return model_ ? model_->current_generation() : 0;
    }

    // ── Whole-accelerator instantiation (RESCAN of a config entry) ────────────
    /**
     * @brief Launch the model + workers, then set up PF1 + PF2, then mark PF0.
     *
     * On any failure the accelerator is fully torn down (→ Inactive) and the
     * error is returned.  Idempotent-ish: instantiating an already-Active
     * accelerator is a no-op success.
     */
    Result<void> instantiate();

    // ── Per-PF restore (RESCAN of a partial accelerator) ──────────────────────
    /**
     * @brief Restore one previously-removed PF without touching the others.
     *
     * Restoring PF2 triggers reconfigure() on the running model (which may promote
     * a staged VBIN → a new process; the QDMA subsystem is then rebound).  PF0/PF1
     * restore do NOT reconfigure.  Requires a running model process.
     */
    Result<void> restore_pf(Pf pf);

    // ── Per-PF removal (REMOVE) ───────────────────────────────────────────────
    /**
     * @brief Remove one PF.  If it was the last present PF, also tear down the
     *        model process + workers (→ Inactive).  VBIN files preserved.
     */
    void remove_pf(Pf pf);

    // ── Full teardown ─────────────────────────────────────────────────────────
    /** Tear down PF2, PF1, the PF0 flag, then the model + workers.  Idempotent. */
    void teardown();

    /**
     * @brief React to an unexpected model-process death (runs on the lifecycle
     *        thread, NOT the monitor thread).
     *
     * @param death_generation  The generation of the process whose death triggered
     *        this.  STALENESS GUARD: the accelerator is torn down ONLY if
     *        @p death_generation still equals the current generation() — a stale
     *        death task (its process already replaced by a newer one via
     *        HOTPLUG/RESCAN) is a no-op.  Must be called under the lifecycle lock so
     *        the generation()/teardown pair is atomic w.r.t. other lifecycle ops.
     */
    void on_model_died(uint64_t death_generation);

    // ── Introspection (for tests) ────────────────────────────────────────────
    [[nodiscard]] CtlSubsystem*  ctl()   const noexcept { return ctl_.get(); }
    [[nodiscard]] QdmaSubsystem* qdma()  const noexcept { return qdma_.get(); }
    [[nodiscard]] ModelInstance* model() const noexcept { return model_.get(); }

private:
    // Ensure a model process is running (reconfigure).  Caller must have quiesced
    // PF1 first if it might be up (reconfigure may destroy the borrowed client).
    Result<void> ensure_model();
    Result<void> setup_ctl();   // PF2
    Result<void> setup_qdma();  // PF1 (always (re)constructs against the live client)
    void         teardown_model_and_workers();
    // Create the persistent BarSet / workers / ModelInstance on first use.
    Result<void> ensure_persistent_state();

    AcceleratorParams             params_;
    std::function<void(uint64_t)> post_model_death_; // arg = dying generation

    // (5)/(6) persistent BARs — outlive model restarts (created once).
    std::unique_ptr<BarSet>              bars_;
    // (3) worker controller — borrows bars_ by reference; reused across restarts.
    std::shared_ptr<ModelControlWorkers> workers_;
    // (1)+(2)+(3) model instance — owns the running process; recreated per teardown.
    std::unique_ptr<ModelInstance>       model_;

    // (6) PF2 and (5) PF1.  Constructed lazily (QdmaSubsystem borrows the model
    // client, which only exists once a model is running).
    std::unique_ptr<CtlSubsystem>  ctl_;
    std::unique_ptr<QdmaSubsystem> qdma_;

    // Presence flags (the state-machine inputs).
    bool pf0_present_ = false;

    // Model-generation counter.  Held as a shared_ptr so it SURVIVES model_ being
    // reset()/recreated across model restarts within this Accelerator (the actual
    // per-process generation and current_generation() live in ModelInstance, which
    // draws from this counter at each launch).  CRUCIALLY it is INJECTED by the
    // owner (HotplugSubsystem) and shared across every Accelerator constructed for
    // the same board, so a re-instantiation after RESCAN replaces the Accelerator
    // object keeps counting UP rather than restarting at 0.  Without that, a
    // re-instantiated process could reuse a just-dead process's generation and a
    // stale death task would wrongly match current generation() and tear the
    // healthy re-adopted model down.  A private counter is created only when the
    // caller injects none (a stand-alone, never-replaced Accelerator).
    std::shared_ptr<std::atomic<uint64_t>> gen_counter_;
};

} // namespace slash_sysemu
