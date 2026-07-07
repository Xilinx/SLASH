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

#include "hotplug_subsystem.h"

#include "hotplug_ioctls.h"

#include <cerrno>
#include <cstring>
#include <utility>
#include <vector>

#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

namespace slash_emu {

namespace {

TransportError os_error(const std::string& what) {
    return TransportError{ErrorKind::Transport, what + ": " + std::strerror(errno)};
}

slash_emu_socket_header make_response_header(const slash_emu_socket_header& req,
                                             int32_t return_value) {
    slash_emu_socket_header h{};
    h.ioctl_op     = req.ioctl_op;
    h.sequence_id  = req.sequence_id;
    h.return_value = static_cast<uint32_t>(return_value);
    h.pad          = 0;
    return h;
}

Result<void> send_plain_response(int fd, const slash_emu_socket_header& req,
                                 int32_t return_value, std::span<const uint8_t> payload) {
    slash_emu_socket_header h = make_response_header(req, return_value);
    return send_message(fd, h, payload, {});
}

// Parse a BDF that may carry a ".F" function suffix.  On success sets @p board to
// the canonical board part and @p pf to the function's PF (nullopt for a bare
// board BDF).  Returns false if the string is malformed.
bool parse_bdf_with_optional_pf(const std::string& raw, std::string& board,
                                std::optional<Pf>& pf) {
    // Find a trailing ".F" suffix (single function digit after the last dot).
    auto dot = raw.rfind('.');
    if (dot != std::string::npos && dot + 2 == raw.size()) {
        char fn = raw[dot + 1];
        std::string board_part = raw.substr(0, dot);
        if (!is_valid_board_bdf(board_part)) {
            return false;
        }
        switch (fn) {
        case '0': pf = Pf::Pf0; break;
        case '1': pf = Pf::Pf1; break;
        case '2': pf = Pf::Pf2; break;
        default:  return false; // only functions 0/1/2 exist
        }
        board = board_part;
        return true;
    }
    // No function suffix: must be a bare board BDF.
    if (!is_valid_board_bdf(raw)) {
        return false;
    }
    board = raw;
    pf    = std::nullopt;
    return true;
}

} // namespace

// ─────────────────────────────────────────────────────────────────────────────
// Construction / destruction
// ─────────────────────────────────────────────────────────────────────────────

HotplugSubsystem::HotplugSubsystem(DaemonConfig cfg)
    : HotplugSubsystem(std::move(cfg), Options{}) {}

HotplugSubsystem::HotplugSubsystem(DaemonConfig cfg, Options opts)
    : socket_cfg_(std::move(cfg)),
      opts_(opts),
      socket_path_(socket_path_hotplug(socket_cfg_)) {}

HotplugSubsystem::~HotplugSubsystem() { remove(); }

// ─────────────────────────────────────────────────────────────────────────────
// setup — start the lifecycle thread + open the socket
// ─────────────────────────────────────────────────────────────────────────────

Result<void> HotplugSubsystem::setup() {
    if (active_.load()) {
        return Result<void>::ok();
    }

    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    if (socket_path_.size() + 1 > sizeof(addr.sun_path)) {
        return Result<void>::err(TransportError{
            ErrorKind::Transport, "slash_hotplug socket path too long: '" + socket_path_ + "'"});
    }
    std::memcpy(addr.sun_path, socket_path_.c_str(), socket_path_.size());

    UniqueFd sock(::socket(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0));
    if (!sock) {
        return Result<void>::err(os_error("socket(AF_UNIX, SOCK_SEQPACKET)"));
    }

    ::unlink(socket_path_.c_str());

    if (::bind(sock.get(), reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) != 0) {
        return Result<void>::err(os_error("bind(" + socket_path_ + ")"));
    }
    if (::chmod(socket_path_.c_str(), socket_cfg_.mode) != 0) {
        auto e = os_error("chmod(" + socket_path_ + ")");
        ::unlink(socket_path_.c_str());
        return Result<void>::err(std::move(e));
    }
    if (::chown(socket_path_.c_str(), socket_cfg_.uid, socket_cfg_.gid) != 0) {
        if (errno != EPERM) {
            auto e = os_error("chown(" + socket_path_ + ")");
            ::unlink(socket_path_.c_str());
            return Result<void>::err(std::move(e));
        }
    }
    if (::listen(sock.get(), /*backlog=*/16) != 0) {
        auto e = os_error("listen(" + socket_path_ + ")");
        ::unlink(socket_path_.c_str());
        return Result<void>::err(std::move(e));
    }

    // Start the lifecycle worker thread.
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        q_stop_ = false;
    }
    lifecycle_thread_ = std::thread([this] { lifecycle_thread_main(); });

    listen_fd_ = std::move(sock);
    stop_.store(false);
    active_.store(true);
    listener_ = std::thread([this] { listener_loop(); });

    return Result<void>::ok();
}

// ─────────────────────────────────────────────────────────────────────────────
// remove — stop socket, drain + stop lifecycle thread, tear down all accelerators
// ─────────────────────────────────────────────────────────────────────────────

void HotplugSubsystem::remove() {
    if (!active_.exchange(false)) {
        return;
    }

    // 1. Stop the socket first so no new ops arrive.
    stop_.store(true);
    ::unlink(socket_path_.c_str());
    if (listen_fd_) {
        ::shutdown(listen_fd_.get(), SHUT_RDWR);
    }
    if (listener_.joinable()) {
        listener_.join();
    }
    listen_fd_.reset();

    std::unordered_map<uint64_t, std::unique_ptr<Conn>> conns;
    {
        std::lock_guard<std::mutex> lk(conns_mtx_);
        conns = std::move(conns_);
        conns_.clear();
    }
    for (auto& [key, c] : conns) {
        (void)key;
        if (c->fd >= 0) {
            ::shutdown(c->fd, SHUT_RDWR);
        }
    }
    for (auto& [key, c] : conns) {
        (void)key;
        if (c->thread.joinable()) {
            c->thread.join();
        }
    }

    // 2. Stop + join the lifecycle thread WITHOUT holding lifecycle_mu_ (a queued
    //    task needs that lock).  Any already-queued death-teardown task runs to
    //    completion first.  Also wake a possible in-flight SBR sleep.
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        q_stop_ = true;
    }
    q_cv_.notify_all();
    sbr_cv_.notify_all();
    if (lifecycle_thread_.joinable()) {
        lifecycle_thread_.join();
    }

    // 3. Tear down every accelerator under the lifecycle lock.
    {
        std::lock_guard<std::mutex> lk(lifecycle_mu_);
        for (auto& [bdf, acc] : accels_) {
            (void)bdf;
            acc->teardown();
        }
        accels_.clear();
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Lifecycle work queue
// ─────────────────────────────────────────────────────────────────────────────

void HotplugSubsystem::post_lifecycle(std::function<void()> task) {
    {
        std::lock_guard<std::mutex> lk(q_mu_);
        if (q_stop_) {
            return; // shutting down: drop the task (accelerators are being torn down)
        }
        q_.push_back(std::move(task));
    }
    q_cv_.notify_one();
}

void HotplugSubsystem::lifecycle_thread_main() {
    for (;;) {
        std::function<void()> task;
        {
            std::unique_lock<std::mutex> lk(q_mu_);
            q_cv_.wait(lk, [this] { return q_stop_ || !q_.empty(); });
            if (q_.empty()) {
                // q_stop_ with an empty queue: exit.
                return;
            }
            task = std::move(q_.front());
            q_.pop_front();
        }
        task(); // takes lifecycle_mu_ internally
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Socket listener / connection workers
// ─────────────────────────────────────────────────────────────────────────────

void HotplugSubsystem::listener_loop() {
    while (!stop_.load()) {
        int conn = ::accept4(listen_fd_.get(), nullptr, nullptr, SOCK_CLOEXEC);
        if (conn < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        reap_finished();
        auto c = std::make_unique<Conn>();
        c->fd  = conn;
        {
            std::lock_guard<std::mutex> lk(conns_mtx_);
            if (stop_.load()) {
                ::close(conn);
                break;
            }
            uint64_t key = next_conn_key_++;
            c->thread    = std::thread([this, conn] { connection_loop(conn); });
            conns_.emplace(key, std::move(c));
        }
    }
}

void HotplugSubsystem::reap_finished() {
    std::vector<std::unique_ptr<Conn>> finished;
    {
        std::lock_guard<std::mutex> lk(conns_mtx_);
        for (auto it = conns_.begin(); it != conns_.end();) {
            if (it->second->done.load()) {
                finished.push_back(std::move(it->second));
                it = conns_.erase(it);
            } else {
                ++it;
            }
        }
    }
    for (auto& c : finished) {
        if (c->thread.joinable()) {
            c->thread.join();
        }
    }
}

void HotplugSubsystem::connection_loop(int conn_fd) {
    UniqueFd fd(conn_fd);
    while (!stop_.load()) {
        auto req = recv_message(fd.get());
        if (!req) {
            break;
        }
        ReceivedMessage& msg = req.value();
        Result<void> sent = dispatch(fd.get(), msg);
        if (!sent) {
            break;
        }
    }
    {
        std::lock_guard<std::mutex> lk(conns_mtx_);
        for (auto& [key, c] : conns_) {
            (void)key;
            if (c->fd == fd.get()) {
                c->done.store(true);
                break;
            }
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// dispatch — parse op + bdf, run the op, respond
// ─────────────────────────────────────────────────────────────────────────────

Result<void> HotplugSubsystem::dispatch(int fd, ReceivedMessage& msg) {
    switch (msg.header.ioctl_op) {
    case kSlashHotplugIoctlRescan: {
        // No argument.  RESCAN never fails structurally; returns 0.
        int rv = op_rescan();
        return send_plain_response(fd, msg.header, rv, {});
    }
    case kSlashHotplugIoctlRemove:
    case kSlashHotplugIoctlToggleSbr:
    case kSlashHotplugIoctlHotplug: {
        if (msg.payload.size() < sizeof(slash_hotplug_device_request)) {
            return send_plain_response(fd, msg.header, -EINVAL, {});
        }
        slash_hotplug_device_request req{};
        std::memcpy(&req, msg.payload.data(), sizeof(req));
        // Ensure NUL termination before treating bdf as a C string.
        req.bdf[sizeof(req.bdf) - 1] = '\0';
        std::string bdf(req.bdf);

        int rv = 0;
        if (msg.header.ioctl_op == kSlashHotplugIoctlRemove) {
            rv = op_remove(bdf);
        } else if (msg.header.ioctl_op == kSlashHotplugIoctlToggleSbr) {
            rv = op_toggle_sbr(bdf);
        } else {
            rv = op_hotplug(bdf);
        }
        std::span<const uint8_t> echo(reinterpret_cast<const uint8_t*>(&req), sizeof(req));
        return send_plain_response(fd, msg.header, rv, echo);
    }
    default:
        return send_plain_response(fd, msg.header, -ENOSYS,
                                   std::span<const uint8_t>(msg.payload));
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// Programmatic ops (take the lifecycle lock once)
// ─────────────────────────────────────────────────────────────────────────────

int HotplugSubsystem::op_rescan() {
    std::lock_guard<std::mutex> lk(lifecycle_mu_);
    return rescan_locked();
}

int HotplugSubsystem::op_remove(const std::string& bdf) {
    std::lock_guard<std::mutex> lk(lifecycle_mu_);
    return remove_locked(bdf);
}

int HotplugSubsystem::op_hotplug(const std::string& bdf) {
    std::lock_guard<std::mutex> lk(lifecycle_mu_);
    return hotplug_locked(bdf);
}

int HotplugSubsystem::op_toggle_sbr(const std::string& bdf) {
    std::unique_lock<std::mutex> lk(lifecycle_mu_);
    return toggle_sbr_locked(bdf, lk);
}

// ─────────────────────────────────────────────────────────────────────────────
// RESCAN
// ─────────────────────────────────────────────────────────────────────────────

int HotplugSubsystem::rescan_locked() {
    // 1. Reload the config file (if configured).  On parse failure keep the
    //    existing accelerators running (spec: active accelerators keep running).
    if (!socket_cfg_.config_file.empty()) {
        ConfigFileResult r = parse_config_file(socket_cfg_.config_file);
        if (r.ok) {
            socket_cfg_.accelerators = std::move(r.accelerators);
        }
    }

    // 2. Restore removed PFs of partial accelerators using their ORIGINAL config.
    for (auto& [bdf, acc] : accels_) {
        (void)bdf;
        if (acc->state() != AccelState::Partial) {
            continue;
        }
        // Restore in the instantiation subsystem order: PF1 then PF2, plus PF0.
        if (!acc->pf_present(Pf::Pf0)) {
            (void)acc->restore_pf(Pf::Pf0);
        }
        if (!acc->pf_present(Pf::Pf1)) {
            (void)acc->restore_pf(Pf::Pf1);
        }
        if (!acc->pf_present(Pf::Pf2)) {
            (void)acc->restore_pf(Pf::Pf2); // may reconfigure the running model
        }
    }

    // 3. (Re)instantiate configured accelerators that don't conflict with a
    //    (partially) active one.  Active-without-config accelerators are simply
    //    never iterated here, so they keep running.
    for (std::size_t n = 0; n < socket_cfg_.accelerators.size(); ++n) {
        const AcceleratorConfig& cfg_e = socket_cfg_.accelerators[n];
        const std::string board = cfg_e.board_bdf();

        auto it = accels_.find(board);
        if (it != accels_.end()) {
            AccelState st = it->second->state();
            if (st == AccelState::Active || st == AccelState::Partial) {
                continue; // conflict: leave the running accelerator alone
            }
        }

        // Build params from the config entry + daemon config.
        AcceleratorParams params{
            .base_dir         = socket_cfg_.base_dir,
            .bdf              = cfg_e.bdf,
            .default_vbin     = {},
            .ctl_socket_path  = socket_path_ctl(socket_cfg_, n),
            .qdma_socket_path = socket_path_qdma_ctl(socket_cfg_, n),
            .uid              = socket_cfg_.uid,
            .gid              = socket_cfg_.gid,
            .mode             = socket_cfg_.mode,
        };
        params.timeouts = opts_.model_timeouts;
        if (auto dv = socket_cfg_.resolve_default_vbin(cfg_e); dv.has_value()) {
            params.default_vbin = *dv;
        }

        std::string board_key = board;
        auto poster = [this, board_key](uint64_t death_generation) {
            // Runs on the ModelProcess monitor thread; only enqueue (never tear
            // down inline).  Forward the dying generation so a stale task is a
            // no-op after the process was replaced.
            post_lifecycle([this, board_key, death_generation] {
                std::lock_guard<std::mutex> lk(lifecycle_mu_);
                auto found = accels_.find(board_key);
                if (found == accels_.end()) {
                    return;
                }
                // on_model_died() carries the staleness guard under this lifecycle
                // lock: it tears the accelerator down only if death_generation still
                // matches its current generation(), so a stale task whose process was
                // already replaced by a newer one (HOTPLUG/RESCAN) is a no-op.
                found->second->on_model_died(death_generation);
            });
        };

        if (it == accels_.end()) {
            auto acc = std::make_unique<Accelerator>(std::move(params), std::move(poster));
            auto [ins, ok] = accels_.emplace(board, std::move(acc));
            (void)ok;
            it = ins;
        } else {
            // Replace an Inactive/Absent accelerator with a fresh one using the
            // reloaded config (the running-conflict case was skipped above).
            it->second = std::make_unique<Accelerator>(std::move(params), std::move(poster));
        }

        (void)it->second->instantiate(); // Failed → left Inactive; continue RESCAN
    }

    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// REMOVE
// ─────────────────────────────────────────────────────────────────────────────

void HotplugSubsystem::remove_all_pfs(Accelerator& acc) {
    // Removing PF0 last matches "the model + workers follow once the last PF is
    // gone" — remove the socket-backed PFs first, then the PF0 flag.
    if (acc.pf_present(Pf::Pf2)) {
        acc.remove_pf(Pf::Pf2);
    }
    if (acc.pf_present(Pf::Pf1)) {
        acc.remove_pf(Pf::Pf1);
    }
    if (acc.pf_present(Pf::Pf0)) {
        acc.remove_pf(Pf::Pf0);
    }
}

int HotplugSubsystem::remove_locked(const std::string& bdf) {
    Target t;
    if (int rc = resolve_target(bdf, t); rc != 0) {
        return rc;
    }
    auto it = accels_.find(t.board_bdf);
    if (it == accels_.end()) {
        return -ENODEV;
    }
    if (t.pf.has_value()) {
        it->second->remove_pf(*t.pf);
    } else {
        remove_all_pfs(*it->second);
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// HOTPLUG = REMOVE(target) + RESCAN
// ─────────────────────────────────────────────────────────────────────────────

int HotplugSubsystem::hotplug_locked(const std::string& bdf) {
    if (int rc = remove_locked(bdf); rc != 0) {
        return rc;
    }
    return rescan_locked();
}

// ─────────────────────────────────────────────────────────────────────────────
// TOGGLE_SBR = REMOVE all PFs of all accelerators on the bus + RESCAN + sleep
// ─────────────────────────────────────────────────────────────────────────────

int HotplugSubsystem::toggle_sbr_locked(const std::string& bdf,
                                        std::unique_lock<std::mutex>& lk) {
    Target t;
    if (int rc = resolve_target(bdf, t); rc != 0) {
        return rc;
    }
    const std::string bus = bus_of(t.board_bdf);

    // REMOVE all PFs of every accelerator on the same bus.
    for (auto& [board, acc] : accels_) {
        if (bus_of(board) == bus) {
            remove_all_pfs(*acc);
        }
    }

    // RESCAN.
    (void)rescan_locked();

    // Emulate the link-training delay (injectable).  Sleep under the lifecycle
    // lock so this remains "one operation on the lock"; wake early on shutdown.
    if (opts_.sbr_delay.count() > 0) {
        sbr_cv_.wait_for(lk, opts_.sbr_delay, [this] { return stop_.load(); });
    }
    return 0;
}

// ─────────────────────────────────────────────────────────────────────────────
// BDF targeting
// ─────────────────────────────────────────────────────────────────────────────

int HotplugSubsystem::resolve_target(const std::string& raw, Target& out) const {
    if (raw.empty()) {
        // Empty bdf: target the only tracked device (slash_hotplug.h convention).
        if (accels_.empty()) {
            return -ENODEV;
        }
        if (accels_.size() > 1) {
            return -EOPNOTSUPP;
        }
        out.board_bdf = accels_.begin()->first;
        out.pf        = std::nullopt;
        return 0;
    }
    std::string       board;
    std::optional<Pf> pf;
    if (!parse_bdf_with_optional_pf(raw, board, pf)) {
        return -EINVAL;
    }
    out.board_bdf = board;
    out.pf        = pf;
    return 0;
}

std::string HotplugSubsystem::bus_of(const std::string& board_bdf) {
    // Canonical board BDF is "DDDD:BB:DD"; the bus is the middle "BB" field.
    auto first = board_bdf.find(':');
    if (first == std::string::npos) {
        return {};
    }
    auto second = board_bdf.find(':', first + 1);
    if (second == std::string::npos) {
        return {};
    }
    return board_bdf.substr(first + 1, second - first - 1);
}

// ─────────────────────────────────────────────────────────────────────────────
// Introspection
// ─────────────────────────────────────────────────────────────────────────────

std::optional<AccelState> HotplugSubsystem::state_of(const std::string& board_bdf) const {
    std::lock_guard<std::mutex> lk(lifecycle_mu_);
    auto it = accels_.find(board_bdf);
    if (it == accels_.end()) {
        return std::nullopt;
    }
    return it->second->state();
}

Accelerator* HotplugSubsystem::accelerator(const std::string& board_bdf) const {
    std::lock_guard<std::mutex> lk(lifecycle_mu_);
    auto it = accels_.find(board_bdf);
    return it == accels_.end() ? nullptr : it->second.get();
}

std::size_t HotplugSubsystem::accelerator_count() const {
    std::lock_guard<std::mutex> lk(lifecycle_mu_);
    return accels_.size();
}

} // namespace slash_emu
