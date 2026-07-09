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

#include "model_control_workers.h"

#include "model_client.h"

#include <utility>

namespace slash_sysemu {

const char* kernel_state_name(KernelState s) noexcept {
    switch (s) {
        case KernelState::Idle: return "Idle";
        case KernelState::Busy: return "Busy";
        case KernelState::Dead: return "Dead";
    }
    return "?";
}

ModelControlWorkers::ModelControlWorkers(BarMemfd&                 user_region,
                                         BarMemfd&                 clock_wizard,
                                         std::chrono::milliseconds poll_interval)
    : user_region_(user_region),
      clock_wizard_(clock_wizard),
      poll_interval_(poll_interval) {}

ModelControlWorkers::~ModelControlWorkers() { stop(); }

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle
// ─────────────────────────────────────────────────────────────────────────────

Result<void> ModelControlWorkers::start(ModelClient& client, const SystemMap& map) {
    if (running_.load()) {
        return Result<void>::err(
            {ErrorKind::Protocol, "model control workers already started"});
    }

    stop_requested_.store(false);

    // Build the worker vector under the unique lock so no introspection reader can
    // observe it mid-construction or race the mutation.  Locate each control
    // register at base_address + 0.  system_map warns register_at(0) may be null:
    // such a kernel simply has no ap_start/ap_done handshake and will never start —
    // we still spawn its worker (so kernel_count() matches the map) but it stays
    // Idle forever.
    {
        std::unique_lock<std::shared_mutex> g(kernels_mu_);
        kernels_.clear();
        kernels_.reserve(map.kernels.size());
        for (const Kernel& k : map.kernels) {
            auto kw        = std::make_unique<KernelWorker>();
            kw->kernel     = k;
            // The BAR memfd is addressed by the kernel's offset WITHIN the user
            // region (base_address % kUserRegionSize), exactly as VRT resolves an
            // absolute kernel address to a BAR offset.  The model, in contrast, is
            // addressed by the absolute base_address.  See KernelWorker's fields.
            kw->bar_base    = static_cast<std::size_t>(k.base_address % kUserRegionSize);
            kw->model_base  = k.base_address;
            kw->has_control = (k.register_at(0) != nullptr);
            kernels_.push_back(std::move(kw));
        }

        running_.store(true);

        // Spawn the threads only after the vector is fully populated so a worker
        // never observes a half-built container.  The workers hold raw
        // KernelWorker* captured from kw.get(); those stay valid because stop()
        // joins every thread BEFORE the vector is cleared.
        for (auto& kw : kernels_) {
            kw->thread =
                std::thread([this, kwp = kw.get(), &client] { kernel_loop(*kwp, client); });
        }
    }
    clock_thread_ = std::thread([this] { clock_loop(); });

    return Result<void>::ok();
}

void ModelControlWorkers::stop() {
    // Idempotent: if never started (or already stopped) there is nothing to join.
    // Signal a stop and wake every worker out of its interruptible sleep.
    {
        std::lock_guard<std::mutex> g(sleep_mu_);
        stop_requested_.store(true);
    }
    sleep_cv_.notify_all();

    // Join every worker BEFORE freeing the vector.  Joining does not mutate the
    // vector, and start()/stop() are not called concurrently (a single controller
    // owner drives the paired lifecycle), so this iteration needs no lock; the
    // KernelWorker objects are still alive here.
    for (auto& kw : kernels_) {
        if (kw->thread.joinable()) {
            kw->thread.join();
        }
    }
    if (clock_thread_.joinable()) {
        clock_thread_.join();
    }

    // Now free the worker objects under the unique lock so a concurrent
    // kernel_state()/kernel_count() reader (shared lock) can never dereference
    // freed storage.
    {
        std::unique_lock<std::shared_mutex> g(kernels_mu_);
        kernels_.clear();
    }
    running_.store(false);
}

KernelState ModelControlWorkers::kernel_state(std::size_t i) const {
    std::shared_lock<std::shared_mutex> g(kernels_mu_);
    if (i >= kernels_.size()) {
        return KernelState::Dead;
    }
    return kernels_[i]->state.load();
}

std::size_t ModelControlWorkers::kernel_count() const {
    std::shared_lock<std::shared_mutex> g(kernels_mu_);
    return kernels_.size();
}

// ─────────────────────────────────────────────────────────────────────────────
// Interruptible sleep
// ─────────────────────────────────────────────────────────────────────────────

bool ModelControlWorkers::wait_or_stop() {
    std::unique_lock<std::mutex> lk(sleep_mu_);
    sleep_cv_.wait_for(lk, poll_interval_, [this] { return stop_requested_.load(); });
    return !stop_requested_.load();
}

// ─────────────────────────────────────────────────────────────────────────────
// Per-kernel worker
// ─────────────────────────────────────────────────────────────────────────────
//
// TOCTOU / atomicity resolution
// -----------------------------
// The architecture wants "check the control register + fetch the parameters +
// reset the control register" to be a single CPU transaction.  BarMemfd's public
// API only brackets ONE flock'd op at a time (read_u32 / write_u32 / update_u32);
// there is no scoped multi-op lock, and Step 7 deliberately kept that API narrow.
//
// We resolve this WITHOUT extending BarMemfd, in two steps:
//
//   1. update_u32(control_offset, ...) atomically reads the control register and,
//      if ap_start is set, clears it to 0 — all under a SINGLE exclusive-flock +
//      internal-mutex bracket.  This is the check-and-reset half of the
//      transaction and is genuinely atomic against both the user (cross-process
//      flock) and other daemon workers (BarMemfd's internal mutex).  So the user
//      can never observe a half-completed handshake (control neither still-set
//      nor yet-cleared): it flips atomically from "ap_start=1" to "0".
//
//   2. The parameter registers are then read with separate read_u32 brackets,
//      AFTER ap_start was captured-and-cleared.  This leaves a residual window
//      (params read a few instructions after the control reset), but it is benign
//      per the architecture's "Accepted inaccuracies": VRT's hardware path writes
//      ALL parameter registers FIRST and only THEN writes ap_start (see
//      vrt/src/kernel.cpp — write() of each arg register precedes the ap_start
//      write to the control register).  Therefore, by the time this worker
//      observes ap_start=1, every parameter is already committed in the memfd.
//      A user that violates that ordering (sets ap_start before writing params)
//      is out of contract; the architecture explicitly does not model
//      write-ordering-dependent behaviour and only promises "suffices for most
//      compute kernels".
//
// Bundling the parameter reads into the update_u32 callback was rejected: the
// callback runs under the exclusive flock, so issuing more mmap reads there is
// possible, but update_u32 only returns a single u32 and cannot surface the
// captured parameter vector without an API change — and the residual-window
// argument above makes that change unnecessary for the MVP.
//
//   3. The busy→idle publish of the finished (ap_done) control word is ALSO done
//      via update_u32, not a blind write: if the user re-armed (set ap_start
//      again) while the kernel was Busy, the publish preserves that ap_start
//      instead of clobbering it, so a concurrent re-arm is not silently dropped.
//      This too is a single exclusive-flock bracket, so the whole control-register
//      handshake — capture/reset on the way in AND publish on the way out — is
//      atomic against the user, not just the reset half.

void ModelControlWorkers::kernel_loop(KernelWorker& kw, ModelClient& client) {
    while (true) {
        if (!wait_or_stop()) {
            return;
        }

        // A kernel without an offset-0 control register can never be started; it
        // just idles until teardown (never crashes).
        if (!kw.has_control) {
            continue;
        }

        // Control register lives at offset 0 of the kernel; bar_base/model_base are
        // the memfd offset and the absolute model address of that register.
        const std::size_t control_bar_offset = kw.bar_base;
        const uint64_t    control_model_addr = kw.model_base;

        // ── IDLE: poll the control register for ap_start ─────────────────────
        uint32_t captured_control = 0;
        bool     started          = false;
        auto     upd = user_region_.update_u32(control_bar_offset, [&](uint32_t cur) -> uint32_t {
            if ((cur & kApStart) != 0) {
                captured_control = cur;
                started          = true;
                return 0; // clear the whole control register (reset the handshake)
            }
            return cur; // leave unchanged
        });
        if (!upd) {
            // An OS failure reading the memfd is fatal for this worker (should not
            // happen for a live mapping); treat as dead so we do not spin.
            kw.state.store(KernelState::Dead);
            return;
        }
        if (!started) {
            continue; // no ap_start yet; keep polling
        }

        // Replay what VRT wrote to the BAR: forward the parameter (input)
        // registers to the model, in declaration order, THEN the control value to
        // trigger the FSM.  Only WRITABLE registers are forwarded: VRT only ever
        // writes input/argument registers (see vrt/src/kernel.cpp — it writes the
        // functional-arg registers, never the read-only output/return/ap_vld
        // registers).  Forwarding a read-only ("R") output register here would push
        // its STALE memfd value (whatever the user last saw, or 0) to the model and
        // clobber the result the model is about to produce — so read-only registers
        // are skipped on the way in and only fetched back on the way out (busy
        // loop).  The control register (offset 0) is always sent last.
        bool transport_dead = false;
        for (const Register& reg : kw.kernel.registers) {
            if (reg.offset == 0) {
                continue; // the control register is sent last
            }
            if (reg.access.find('W') == std::string::npos) {
                continue; // read-only output register: not a parameter to forward
            }
            // Read the staged value from the BAR memfd (BAR-window offset) and
            // forward it to the model at the absolute AXI address.
            std::size_t bar_off   = kw.bar_base + static_cast<std::size_t>(reg.offset);
            uint64_t    model_addr = kw.model_base + static_cast<uint64_t>(reg.offset);
            auto v = user_region_.read_u32(bar_off);
            if (!v) {
                // Out-of-range/OS failure on a declared register: skip it rather
                // than abort the whole cycle (a malformed map should not wedge the
                // worker).  This is defensive; a well-formed map stays in range.
                continue;
            }
            auto w = client.reg_write(model_addr, v.value());
            if (!w) {
                if (w.error().kind == ErrorKind::Transport) {
                    transport_dead = true;
                    break;
                }
                // Protocol error: the model rejected the write.  Log-worthy, but
                // we press on — the model FSM decides what to do with the control.
            }
        }
        if (!transport_dead) {
            auto w = client.reg_write(control_model_addr, captured_control);
            if (!w && w.error().kind == ErrorKind::Transport) {
                transport_dead = true;
            }
        }
        if (transport_dead) {
            kw.state.store(KernelState::Dead);
            return;
        }

        kw.state.store(KernelState::Busy);

        // ── BUSY: poll the model's control register for ap_done ──────────────
        while (true) {
            if (!wait_or_stop()) {
                return;
            }
            auto ctrl = client.fetch_scalar(control_model_addr);
            if (!ctrl) {
                if (ctrl.error().kind == ErrorKind::Transport) {
                    kw.state.store(KernelState::Dead);
                    return;
                }
                continue; // Protocol error: retry the poll
            }
            if ((ctrl.value() & kApDone) == 0) {
                continue; // not done yet
            }

            // ap_done: fetch every readable (output/return/ap_vld) register from
            // the model and write it back into the memfd so the user/VRT observes
            // the results.  "R" in the access string marks a readable register.
            bool dead = false;
            for (const Register& reg : kw.kernel.registers) {
                if (reg.offset == 0) {
                    continue; // control handled separately below
                }
                if (reg.access.find('R') == std::string::npos) {
                    continue; // not readable: nothing to fetch back
                }
                // Fetch the result from the model (absolute address) and write it
                // back into the BAR memfd (BAR-window offset) so the user sees it.
                std::size_t bar_off    = kw.bar_base + static_cast<std::size_t>(reg.offset);
                uint64_t    model_addr = kw.model_base + static_cast<uint64_t>(reg.offset);
                auto val = client.fetch_scalar(model_addr);
                if (!val) {
                    if (val.error().kind == ErrorKind::Transport) {
                        dead = true;
                        break;
                    }
                    continue; // Protocol error: skip this register
                }
                auto wb = user_region_.write_u32(bar_off, val.value());
                (void)wb; // a memfd write failure is non-fatal to the FSM state
            }
            if (dead) {
                kw.state.store(KernelState::Dead);
                return;
            }

            // Publish the model's control value (with ap_done set) so the user
            // sees the finished handshake — but via update_u32, NOT a blind
            // overwrite, and preserving a fresh ap_start.  Rationale: the busy→idle
            // publish targets the same control cell the user writes ap_start to.  A
            // blind write would clobber an ap_start the user set WHILE we were Busy
            // (a re-arm), silently dropping that launch.  Strict serial VRT usage
            // (startKernel → wait-on-ap_done → next startKernel) never re-arms
            // before reading ap_done, so this cannot happen under the documented
            // contract; but honouring a concurrent re-arm rather than eating it is
            // strictly safer and keeps the whole handshake atomic against the user
            // (update_u32 is a single exclusive-flock bracket).  If the user has
            // re-armed (ap_start set), we leave the memfd as-is so the idle loop
            // picks the new launch up next; otherwise we publish ap_done.
            const uint32_t done_value = ctrl.value();
            auto           wb = user_region_.update_u32(
                control_bar_offset, [done_value](uint32_t cur) -> uint32_t {
                    if ((cur & kApStart) != 0) {
                        return cur; // user re-armed while Busy: honour it, don't clobber
                    }
                    return done_value; // no re-arm: publish the finished (ap_done) word
                });
            (void)wb; // a memfd failure is non-fatal to the FSM state

            kw.state.store(KernelState::Idle);
            break; // back to the IDLE poll
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Clock-wizard worker
// ─────────────────────────────────────────────────────────────────────────────
//
// The vrtd clock driver polls REG4 bit0 (offset 0x33C within each wizard window)
// for the lock signal after a reconfig.  In the memory-backed BAR that cell
// aliases a divider data register, so it frequently reads back 0.  We pin bit0 of
// both windows (user 0x0033C, service 0x1033C) to 1 on every poll cycle so the
// driver's 200 ms / 100 µs lock poll always sees a 1 (architecture: "Clock
// wizard").  No model interaction.

void ModelControlWorkers::clock_loop() {
    auto pin = [](uint32_t cur) -> uint32_t { return cur | kClockLockBit; };
    while (wait_or_stop()) {
        auto a = clock_wizard_.update_u32(kClockLockUserOffset, pin);
        (void)a;
        auto b = clock_wizard_.update_u32(kClockLockServiceOffset, pin);
        (void)b;
    }
}

} // namespace slash_sysemu
