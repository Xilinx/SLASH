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

#include "transport.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

namespace slash_emu {

// ─────────────────────────────────────────────────────────────────────────────
// BarMemfd — a memfd-backed emulated BAR window
// ─────────────────────────────────────────────────────────────────────────────
//
// The real driver exposes each BAR as a dmabuf that the user mmaps and accesses
// via direct MMIO, bracketing transactions with DMA_BUF_IOCTL_SYNC.  The
// emulation daemon instead backs each BAR with an anonymous memfd (architecture:
// "Model control worker subsystem"):
//
//   * The daemon holds an fd to the memfd and mmaps it for its own (frequent,
//     polling) register access.
//   * Instead of DMA_BUF_IOCTL_SYNC, reads take a SHARED flock (LOCK_SH) and
//     writes take an EXCLUSIVE flock (LOCK_EX), released after each access.
//   * The fd handed to the user (Step 9's GET_BAR_FD) MUST be a DISTINCT open
//     file description from the daemon's, otherwise their flocks would not
//     collide (flock is per-open-file-description, not per-fd/per-inode-per-
//     process).  reopen() provides that distinct description by opening
//     /proc/self/fd/<n> rather than dup()-ing.
//
// Daemon-side data path: this class keeps a persistent RAII mmap of the whole
// window.  Step 8's worker polls control registers in a tight loop, so a
// per-access pread/pwrite syscall would be wasteful; a persistent mmap lets the
// daemon read/write registers as plain memory.  The flock brackets remain the
// advisory synchronisation protocol between the daemon and the user (mmap access
// itself does not interact with flock — flock is advisory).
//
// Thread-safety / flock nuance: flock is per-OPEN-FILE-DESCRIPTION.  A single fd's
// whole-file LOCK_SH/LOCK_EX excludes other DISTINCT descriptions (a user's
// reopen()ed fd) — the daemon-vs-user boundary — but multiple daemon threads
// sharing THIS one BarMemfd hold the SAME description, so the flock is merely
// converted, not contended: one thread's LOCK_UN would drop a lock another thread
// believes it holds (breaking exclusion against the user and risking torn
// same-offset access).  Because the 128 MiB user-region BAR holds every kernel's
// register window and Step 8 runs one worker thread PER KERNEL, multiple daemon
// threads WILL share one BarMemfd.  So this class holds an INTERNAL std::mutex for
// the entire flock-bracketed op: BarMemfd is internally thread-safe for concurrent
// daemon-side access (at most one daemon thread holds the flock at a time, so
// LOCK_UN is correct and the flock still excludes the user for that window), and
// the flock continues to handle the cross-process (daemon-vs-user) boundary.
// Multiple Step 8 workers may therefore safely share one BarMemfd.
//
// All accesses are bounds-checked against the BAR size; out-of-range access is a
// Protocol error.  OS failures (memfd_create/ftruncate/mmap/flock/open) are
// Transport errors.  Nothing throws across the API.

class BarMemfd {
public:
    /**
     * @brief Create a memfd-backed BAR window of @p size bytes.
     *
     * memfd_create(MFD_CLOEXEC) + ftruncate(size) + a persistent mmap of the
     * whole window.
     *
     * @param size  Window size in bytes (e.g. 128 MiB for the user region).
     * @param name  Debug name for the memfd (as shown in /proc/<pid>/fd).
     * @return A ready BarMemfd, or ErrorKind::Transport on any OS failure.
     */
    static Result<BarMemfd> create(std::size_t size, const std::string& name = "slash_bar");

    BarMemfd(const BarMemfd&)            = delete;
    BarMemfd& operator=(const BarMemfd&) = delete;
    BarMemfd(BarMemfd&& o) noexcept;
    BarMemfd& operator=(BarMemfd&& o) noexcept;
    ~BarMemfd();

    /** Window size in bytes. */
    [[nodiscard]] std::size_t size() const noexcept { return size_; }
    /** The daemon-owned fd (borrowed; ownership stays with this object). */
    [[nodiscard]] int fd() const noexcept { return fd_.get(); }

    // ── Register / range access (flock-bracketed) ────────────────────────────

    /** Read a 32-bit little-endian value at @p offset under a SHARED lock. */
    [[nodiscard]] Result<uint32_t> read_u32(std::size_t offset) const;

    /** Write a 32-bit little-endian value at @p offset under an EXCLUSIVE lock. */
    Result<void> write_u32(std::size_t offset, uint32_t value);

    /** Read @p len bytes starting at @p offset under a SHARED lock. */
    [[nodiscard]] Result<std::vector<uint8_t>> read(std::size_t offset, std::size_t len) const;

    /** Write @p bytes starting at @p offset under an EXCLUSIVE lock. */
    Result<void> write(std::size_t offset, std::span<const uint8_t> bytes);

    /**
     * @brief Atomically read-modify-write the 32-bit value at @p offset.
     *
     * Reads the current value, calls @p fn on it, and writes the result back —
     * all under a SINGLE held internal-mutex + EXCLUSIVE-flock bracket, so
     * concurrent daemon-side RMWs on this object do not lose updates and the user
     * cannot observe or interleave a half-completed modification.  Step 8's kernel
     * workers use this for control-register handshakes (e.g. clear ap_start after
     * reading it) where a plain read_u32-then-write_u32 pair would race.
     *
     * @return the NEW value on success, or an error (out-of-range / OS failure).
     */
    Result<uint32_t> update_u32(std::size_t offset, const std::function<uint32_t(uint32_t)>& fn);

    /**
     * @brief Return a NEW open file description on the same memfd.
     *
     * Opens /proc/self/fd/<n> (O_RDWR|O_CLOEXEC), yielding a fresh open file
     * description that shares the underlying memfd inode but NOT the file
     * description — so an flock held on this BarMemfd's fd and an flock held on
     * the returned fd genuinely conflict (as the daemon-vs-user model requires).
     * This is the fd Step 9 sends to the user; the daemon closes its copy after
     * handing it over (the returned UniqueFd owns it until then).
     *
     * NOTE: dup() would NOT work here — a dup shares the same open file
     * description, so its flock would never conflict with the original's.
     */
    [[nodiscard]] Result<UniqueFd> reopen() const;

    // A default-constructed BarMemfd owns no memfd or mapping (size()==0,
    // fd()==-1).  It exists so Result<BarMemfd>/BarSet can hold a moved-into slot;
    // use create() / make_standard_bars() to obtain a usable window.
    BarMemfd() = default;

private:
    void close_mapping() noexcept;

    UniqueFd    fd_;
    void*       map_ = nullptr; // persistent mmap of the whole window (MAP_SHARED)
    std::size_t size_ = 0;
    // Serialises the ENTIRE flock-bracketed access (lock → flock → mmap access →
    // LOCK_UN → unlock) so that concurrent daemon-side callers sharing one
    // BarMemfd (e.g. multiple Step 8 workers on the shared 128 MiB user-region
    // BAR) do not race on the single fd's whole-file flock — one thread's LOCK_UN
    // would otherwise drop the lock another thread believes it holds, breaking
    // exclusion against the user and risking torn same-offset access.  Behind a
    // unique_ptr so BarMemfd stays movable (std::mutex is not movable).  Moves
    // happen at construction/handoff, before any worker touches the object, so a
    // move never races an access.
    std::unique_ptr<std::mutex> mutex_ = std::make_unique<std::mutex>();
};

// ─────────────────────────────────────────────────────────────────────────────
// Standard BAR set
// ─────────────────────────────────────────────────────────────────────────────
//
// The three BARs present on PF2 (architecture: BARs 0, 2, 4 are present/usable),
// with the sizes from the "Model control worker subsystem" section.

enum class BarKind {
    UserRegion,  /**< BAR 0: compute-kernel control window, 128 MiB. */
    ServiceLayer,/**< BAR 2: service layer window, 128 MiB. */
    ClockWizard, /**< BAR 4: clock-wizard register windows, 512 KiB. */
};

/** Number of standard BARs. */
inline constexpr std::size_t kNumStandardBars = 3;

/** Byte size of each standard BAR. */
inline constexpr std::size_t kUserRegionSize  = 128u * 1024u * 1024u; // 128 MiB
inline constexpr std::size_t kServiceLayerSize = 128u * 1024u * 1024u; // 128 MiB
inline constexpr std::size_t kClockWizardSize  = 512u * 1024u;         // 512 KiB

/** Size in bytes for a given BAR kind. */
[[nodiscard]] std::size_t bar_kind_size(BarKind kind) noexcept;

/** PF2 BAR index (0/2/4) for a given BAR kind. */
[[nodiscard]] int bar_kind_index(BarKind kind) noexcept;

/**
 * @brief The standard trio of BAR memfds for one accelerator.
 *
 * Indexed by BarKind; also queryable by the PF2 BAR index (0/2/4) that Step 9
 * hands to the user in GET_BAR_INFO/GET_BAR_FD.
 */
struct BarSet {
    BarMemfd user_region;
    BarMemfd service_layer;
    BarMemfd clock_wizard;

    /** Access a BAR by kind. */
    [[nodiscard]] BarMemfd&       by_kind(BarKind kind) noexcept;
    [[nodiscard]] const BarMemfd& by_kind(BarKind kind) const noexcept;

    /**
     * @brief Look up a BAR by its PF2 index (0, 2, or 4).
     * @return pointer to the BAR, or nullptr if @p bar_index is not present.
     */
    [[nodiscard]] BarMemfd*       by_index(int bar_index) noexcept;
    [[nodiscard]] const BarMemfd* by_index(int bar_index) const noexcept;
};

/**
 * @brief Create the standard trio (user region, service layer, clock wizard).
 * @return a populated BarSet, or ErrorKind::Transport on any OS failure.
 */
Result<BarSet> make_standard_bars();

} // namespace slash_emu
