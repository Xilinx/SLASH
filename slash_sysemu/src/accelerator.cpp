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

#include "accelerator.h"

#include <utility>

namespace slash_sysemu {

namespace {

TransportError err(const std::string& what) {
    return TransportError{ErrorKind::Transport, what};
}

} // namespace

const char* accel_state_name(AccelState s) noexcept {
    switch (s) {
    case AccelState::Absent:   return "absent";
    case AccelState::Inactive: return "inactive";
    case AccelState::Active:   return "active";
    case AccelState::Partial:  return "partial";
    }
    return "unknown";
}

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

Accelerator::Accelerator(AcceleratorParams params, std::function<void(uint64_t)> post_model_death,
                         std::shared_ptr<std::atomic<uint64_t>> gen_counter)
    : params_(std::move(params)),
      post_model_death_(std::move(post_model_death)),
      gen_counter_(gen_counter ? std::move(gen_counter)
                               : std::make_shared<std::atomic<uint64_t>>(0)) {}

Accelerator::~Accelerator() { teardown(); }

// ─────────────────────────────────────────────────────────────────────────────
// State
// ─────────────────────────────────────────────────────────────────────────────

bool Accelerator::model_running() const noexcept {
    return model_ != nullptr && model_->has_process();
}

bool Accelerator::pf_present(Pf pf) const noexcept {
    switch (pf) {
    case Pf::Pf0: return pf0_present_;
    case Pf::Pf1: return qdma_ != nullptr && qdma_->is_active();
    case Pf::Pf2: return ctl_ != nullptr && ctl_->is_active();
    }
    return false;
}

AccelState Accelerator::state() const {
    if (model_running()) {
        const bool all_pfs = pf_present(Pf::Pf0) && pf_present(Pf::Pf1) && pf_present(Pf::Pf2);
        return all_pfs ? AccelState::Active : AccelState::Partial;
    }
    // No model running: Absent if this BDF was never instantiated and has no
    // on-disk main VBIN; otherwise Inactive (files persist across teardown).
    if (model_ != nullptr && model_->store().has_main()) {
        return AccelState::Inactive;
    }
    // No ModelInstance yet: check the on-disk main.vbin directly.
    std::error_code ec;
    std::filesystem::path main = params_.base_dir / params_.bdf.str() / "main.vbin";
    if (std::filesystem::is_regular_file(main, ec)) {
        return AccelState::Inactive;
    }
    return AccelState::Absent;
}

// ─────────────────────────────────────────────────────────────────────────────
// Persistent state (BARs, workers, ModelInstance) — created once, reused
// ─────────────────────────────────────────────────────────────────────────────

Result<void> Accelerator::ensure_persistent_state() {
    if (bars_ == nullptr) {
        auto b = make_standard_bars();
        if (!b) {
            return Result<void>::err(b.error());
        }
        bars_ = std::make_unique<BarSet>(std::move(b.value()));
    }
    if (workers_ == nullptr) {
        workers_ = std::make_shared<ModelControlWorkers>(
            bars_->user_region, bars_->clock_wizard, params_.worker_poll);
    }
    if (model_ == nullptr) {
        // The death callback only POSTS to the daemon lifecycle queue; it never
        // tears down inline (see model_process.h DeathCallback contract).  The
        // GENERATION is threaded through ModelInstance's per-launch wrapper (each
        // process carries its own generation), so this callback just forwards it.
        std::function<void(uint64_t)> on_death_gen = [this](uint64_t gen) {
            if (post_model_death_) {
                post_model_death_(gen);
            }
        };
        model_ = std::make_unique<ModelInstance>(
            params_.base_dir, params_.bdf.str(), params_.default_vbin,
            workers_, std::move(on_death_gen), gen_counter_, params_.timeouts);
    }
    return Result<void>::ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// ensure_model — reconfigure/launch the model process
// ─────────────────────────────────────────────────────────────────────────────
//
// IMPORTANT (QDMA-vs-reconfigure safety): reconfigure() may DESTROY the current
// ModelProcess (and its ModelClient) before adopting a new one.  A QdmaSubsystem
// borrows that ModelClient by reference, so the caller MUST have quiesced PF1
// (qdma_->remove() + reset, which joins all transfer-session threads while the
// old client is still alive) BEFORE calling ensure_model() whenever PF1 might be
// up.  instantiate() satisfies this trivially (PF1 doesn't exist yet);
// restore_pf(Pf2) quiesces PF1 explicitly.  Stale qpair state is intentionally
// dropped — device memory does not persist across reconfiguration anyway
// (an accepted emulation inaccuracy for now).

Result<void> Accelerator::ensure_model() {
    if (auto r = ensure_persistent_state(); !r) {
        return r;
    }
    ReconfigureResult rr = model_->reconfigure();
    switch (rr.status) {
    case ReconfigureStatus::Failed:
        return Result<void>::err(err("reconfigure failed: " + rr.message));
    case ReconfigureStatus::NewProcess:
        // The generation of the newly-adopted process was recorded by
        // ModelInstance (current_generation()); nothing to do here.
        return Result<void>::ok();
    case ReconfigureStatus::Unchanged:
        return Result<void>::ok();
    }
    return Result<void>::ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// setup_ctl / setup_qdma
// ─────────────────────────────────────────────────────────────────────────────

Result<void> Accelerator::setup_ctl() {
    if (ctl_ != nullptr && ctl_->is_active()) {
        return Result<void>::ok();
    }
    if (bars_ == nullptr) {
        return Result<void>::err(err("setup_ctl: no BAR set"));
    }
    if (ctl_ == nullptr) {
        ctl_ = std::make_unique<CtlSubsystem>(params_.ctl_socket_path,
                                              params_.bdf.str(), *bars_);
    }
    return ctl_->setup();
}

Result<void> Accelerator::setup_qdma() {
    if (qdma_ != nullptr && qdma_->is_active()) {
        return Result<void>::ok();
    }
    if (model_ == nullptr || !model_->has_process()) {
        return Result<void>::err(err("setup_qdma: no running model"));
    }
    // Always (re)construct a fresh QdmaSubsystem bound to the CURRENT model
    // client.  A QdmaSubsystem is never rebound to a different client: it is torn
    // down and reset() before any model swap, so qdma_ is null here whenever the
    // model may have changed, and this construction always binds the live client.
    if (qdma_ == nullptr) {
        qdma_ = std::make_unique<QdmaSubsystem>(params_.qdma_socket_path,
                                                params_.bdf.str(),
                                                model_->process()->client(),
                                                model_->store());
    }
    return qdma_->setup();
}

// ─────────────────────────────────────────────────────────────────────────────
// instantiate — model first, then PF1 + PF2, then PF0
// ─────────────────────────────────────────────────────────────────────────────

Result<void> Accelerator::instantiate() {
    if (state() == AccelState::Active) {
        return Result<void>::ok(); // idempotent
    }

    // 1. Launch (or confirm) the model process + workers.
    if (auto r = ensure_model(); !r) {
        teardown();
        return r;
    }

    // 2. Set up the subsystems: QDMA (PF1) then BAR/device-info (PF2).
    if (auto r = setup_qdma(); !r) {
        teardown();
        return r;
    }
    if (auto r = setup_ctl(); !r) {
        teardown();
        return r;
    }

    // 3. Mark PF0 present.
    pf0_present_ = true;
    return Result<void>::ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// restore_pf — bring one PF back without touching the others
// ─────────────────────────────────────────────────────────────────────────────

Result<void> Accelerator::restore_pf(Pf pf) {
    if (!model_running()) {
        return Result<void>::err(err("restore_pf: no running model"));
    }
    if (pf_present(pf)) {
        return Result<void>::ok(); // already up
    }
    switch (pf) {
    case Pf::Pf0:
        pf0_present_ = true;
        return Result<void>::ok();
    case Pf::Pf1:
        // PF1 restore does NOT reconfigure; just bring the QDMA socket back up
        // bound to the current model client.
        return setup_qdma();
    case Pf::Pf2: {
        // PF2 restore triggers reconfiguration on the running model, which may
        // promote a staged VBIN and adopt a NEW model process (destroying the old
        // ModelClient).  If PF1 is currently up it borrows that soon-to-be-dead
        // client, so QUIESCE PF1 FIRST: qdma_->remove() joins every transfer
        // session thread while the old client is still alive, so no in-flight
        // transfer can outlive the swap (provably no use-after-free).  We then
        // reconfigure, bring PF2 up, and finally reconstruct PF1 bound to the
        // (possibly new) live client.  Stale qpair state is intentionally dropped
        // — device memory does not survive reconfiguration anyway.
        const bool pf1_was_up = pf_present(Pf::Pf1);
        if (pf1_was_up) {
            qdma_->remove();
            qdma_.reset();
        }
        if (auto r = ensure_model(); !r) {
            // Reconfiguration failed on a running model: the accelerator
            // disappears.  Tear it down (→ Inactive).
            teardown();
            return r;
        }
        if (auto r = setup_ctl(); !r) {
            return r;
        }
        if (pf1_was_up) {
            // Reconstruct PF1 against the current live client (see setup_qdma).
            if (auto r = setup_qdma(); !r) {
                return r;
            }
        }
        return Result<void>::ok();
    }
    }
    return Result<void>::ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// remove_pf — remove one PF; drop model+workers once the last PF is gone
// ─────────────────────────────────────────────────────────────────────────────

void Accelerator::remove_pf(Pf pf) {
    switch (pf) {
    case Pf::Pf0:
        pf0_present_ = false;
        break;
    case Pf::Pf1:
        if (qdma_ != nullptr) {
            qdma_->remove();
        }
        break;
    case Pf::Pf2:
        if (ctl_ != nullptr) {
            ctl_->remove();
        }
        break;
    }

    // If no PF remains present, tear down the model process + workers (the last-PF
    // rule).  The VBIN files are preserved.
    if (!pf_present(Pf::Pf0) && !pf_present(Pf::Pf1) && !pf_present(Pf::Pf2)) {
        teardown_model_and_workers();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// teardown — full teardown in order (PFs, then model + workers)
// ─────────────────────────────────────────────────────────────────────────────

void Accelerator::teardown() {
    // PF2 first, then PF1 (reverse of instantiation).  Destroy the QdmaSubsystem
    // (which joins its transfer workers) BEFORE the ModelInstance whose client it
    // borrows, so the borrowed pointer never dangles under a live transfer.
    if (ctl_ != nullptr) {
        ctl_->remove();
    }
    if (qdma_ != nullptr) {
        qdma_->remove();
    }
    qdma_.reset(); // release the borrowed model-client pointer before the model dies
    ctl_.reset();

    pf0_present_ = false;

    teardown_model_and_workers();
}

void Accelerator::teardown_model_and_workers() {
    if (model_ != nullptr) {
        // ModelInstance::teardown() stops the workers, then tears down the model
        // process (joining its monitor thread).  This is safe here because it runs
        // on the lifecycle thread, never the monitor thread.
        model_->teardown();
        model_.reset();
    }
    // bars_ and workers_ are retained: the memfds persist across model restarts,
    // and a fresh ModelInstance will reuse the same workers_ controller.  A new
    // ModelInstance is created on the next instantiate() (ensure_persistent_state).
}

void Accelerator::on_model_died(uint64_t death_generation) {
    // Staleness guard: only the CURRENT process's death tears the accelerator down.
    // A stale death task — one whose process was already replaced by a newer one via
    // HOTPLUG/RESCAN — carries a mismatched generation and is a no-op.  generation()
    // reads the ModelInstance's current generation (0 if no process is adopted).
    // The caller holds the lifecycle lock, so the generation()/teardown pair is
    // atomic with respect to other lifecycle operations.
    if (death_generation != generation()) {
        return;
    }
    teardown();
}

} // namespace slash_sysemu
