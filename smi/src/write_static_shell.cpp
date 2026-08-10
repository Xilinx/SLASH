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

/// @file write_static_shell.cpp
/// @brief Implementation of the write-static-shell command.

#include "write_static_shell.hpp"

#include <array>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <exception>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <sys/wait.h>
#include <unistd.h>

#include <vrtd/session.hpp>

#include "bdf.hpp"

#ifndef SMI_VERSAL_FLASH_TCL
#define SMI_VERSAL_FLASH_TCL "/usr/share/v80-smi/versal_flash_pdi.tcl"
#endif

namespace {

constexpr uint8_t StaticShellBootDevice = 0;

bool shellTypeAll(const std::string& shellType) {
    return shellType == "all";
}

std::string effectiveShellType(const WriteStaticShell::Options& options) {
    if (!options.shellType.empty()) {
        return options.shellType;
    }
    if (options.flash && options.pdiPath.empty()) {
        return "all";
    }
    return "service";
}

vrtd::ShellType parseShellType(const std::string& shellType) {
    if (shellType == "service") {
        return vrtd::ShellType::Service;
    }
    if (shellType == "compute") {
        return vrtd::ShellType::Compute;
    }

    throw std::invalid_argument("shell-type must be one of: service, compute");
}

uint32_t shellPartition(vrtd::ShellType shellType) {
    switch (shellType) {
    case vrtd::ShellType::Service:
        return 0;
    case vrtd::ShellType::Compute:
        return 1;
    default:
        throw std::invalid_argument("shell-type must be one of: service, compute");
    }
}

const char* cfgmemPhaseName(vrtd::CfgmemProgramPhase phase) {
    switch (phase) {
    case vrtd::CfgmemProgramPhase::Queued:
        return "Queued";
    case vrtd::CfgmemProgramPhase::OpeningAmi:
        return "Opening AMI device";
    case vrtd::CfgmemProgramPhase::DownloadingPdi:
        return "Downloading PDI";
    case vrtd::CfgmemProgramPhase::SelectingPartition:
        return "Selecting boot partition";
    case vrtd::CfgmemProgramPhase::ResetPreparing:
        return "Preparing reset";
    case vrtd::CfgmemProgramPhase::RemovingPcie:
        return "Removing PCIe functions";
    case vrtd::CfgmemProgramPhase::TogglingSbr:
        return "Toggling secondary bus reset";
    case vrtd::CfgmemProgramPhase::RescanningPcie:
        return "Rescanning PCIe";
    case vrtd::CfgmemProgramPhase::RediscoveringDevice:
        return "Rediscovering device";
    case vrtd::CfgmemProgramPhase::Done:
        return "Done";
    case vrtd::CfgmemProgramPhase::Failed:
        return "Failed";
    }

    return "Unknown";
}

class StatusReporter {
public:
    void stage(const std::string& message) const {
        std::cerr << message << "...\n";
    }

    void progress(const vrtd::CfgmemProgramStatus& status) {
        const uint32_t percent = status.bytesTotal == 0 ? 0 :
            static_cast<uint32_t>((status.bytesWritten * 100ULL) / status.bytesTotal);

        if (status.phase == lastPhase &&
            percent == lastPercent &&
            status.state == lastState) {
            return;
        }

        lastPhase = status.phase;
        lastPercent = percent;
        lastState = status.state;

        /* Only the download percentage animates in place; every other phase
         * transition gets its own line. */
        if (status.phase == vrtd::CfgmemProgramPhase::DownloadingPdi) {
            std::cerr << '\r' << cfgmemPhaseName(status.phase) << ": " << percent << "%";
            if (status.bytesTotal != 0) {
                std::cerr << " (" << status.bytesWritten << "/" << status.bytesTotal << " bytes)";
            }
            /* \r only rewinds; erase to end of line so a shorter update
             * doesn't inherit the tail of a longer one. */
            std::cerr << "\033[K" << std::flush;
            lineOpen = true;
        } else {
            if (lineOpen) {
                std::cerr << '\n';
                lineOpen = false;
            }
            std::cerr << cfgmemPhaseName(status.phase) << '\n';
        }
    }

    void endProgress() {
        if (lineOpen) {
            std::cerr << '\n';
            lineOpen = false;
        }
    }

    void done(const std::string& message) const {
        const auto elapsed = std::chrono::steady_clock::now() - start;
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(elapsed).count();
        std::cerr << message << " (" << seconds << "s)\n";
    }

private:
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    vrtd::CfgmemProgramPhase lastPhase = vrtd::CfgmemProgramPhase::Queued;
    uint32_t lastPercent = UINT32_MAX;
    vrtd::CfgmemProgramState lastState = vrtd::CfgmemProgramState::Queued;
    bool lineOpen = false;  ///< True while an in-place download line awaits its newline.
};

bool fileExists(const std::string& path) {
    std::ifstream f(path);
    return f.good();
}

std::string trimTrailingNewline(std::string text) {
    const auto end = text.find_last_not_of("\r\n");
    if (end == std::string::npos) {
        return "";
    }
    text.erase(end + 1);
    return text;
}

std::string shellQuote(const std::string& text) {
    std::string quoted = "'";
    for (char c : text) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += "'";
    return quoted;
}

std::string resolveStaticShellPdi(bool nofpt, const std::string& shellType) {
    std::string command = "python3 -m slashkit static-shell-path";
    command += " --shell-type " + shellQuote(shellType);
    if (nofpt) {
        command += " --nofpt";
    }

    FILE *pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        throw std::runtime_error(
            "Failed to run 'python3 -m slashkit static-shell-path': " +
            std::string(std::strerror(errno)));
    }

    std::string output;
    std::array<char, 256> buffer{};
    while (fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }

    const int status = pclose(pipe);
    if (status == -1) {
        throw std::runtime_error(
            "Failed to wait for 'python3 -m slashkit static-shell-path': " +
            std::string(std::strerror(errno)));
    }
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        throw std::runtime_error("'python3 -m slashkit static-shell-path' failed");
    }

    std::string path = trimTrailingNewline(output);
    if (path.empty()) {
        throw std::runtime_error("'python3 -m slashkit static-shell-path' printed no path");
    }
    if (!fileExists(path)) {
        throw std::runtime_error("Static shell PDI path does not exist: " + path);
    }

    return path;
}

std::string resolvePdiPath(const WriteStaticShell::Options& options,
                           bool nofpt,
                           const std::string& shellType) {
    if (!options.pdiPath.empty()) {
        if (!fileExists(options.pdiPath)) {
            throw std::runtime_error("PDI path does not exist: " + options.pdiPath);
        }
        return options.pdiPath;
    }

    return resolveStaticShellPdi(nofpt, shellType);
}

std::string resolveVersalFlashTcl() {
    if (const char *overridePath = std::getenv("SMI_VERSAL_FLASH_TCL")) {
        if (overridePath[0] != '\0') {
            return overridePath;
        }
    }

    return SMI_VERSAL_FLASH_TCL;
}

std::string buildXsdbCommand(const std::vector<std::string>& bashSources,
                             const std::string& tclPath) {
    std::string command;
    for (const auto& source : bashSources) {
        command += "source " + shellQuote(source) + "; ";
    }
    command += "xsdb " + shellQuote(tclPath);
    return command;
}

void runBashCommandWithPdiPath(const std::string& command,
                               const std::string& pdiPath,
                               const std::string& xsdbTargetId) {
    const pid_t pid = fork();
    if (pid == -1) {
        throw std::runtime_error("fork failed: " + std::string(std::strerror(errno)));
    }

    if (pid == 0) {
        if (setenv("PDI_PATH", pdiPath.c_str(), 1) != 0) {
            std::perror("setenv PDI_PATH");
            _exit(127);
        }
        if (!xsdbTargetId.empty() &&
            setenv("V80_TARGET_ID", xsdbTargetId.c_str(), 1) != 0) {
            std::perror("setenv V80_TARGET_ID");
            _exit(127);
        }
        execl("/bin/bash", "/bin/bash", "-c", command.c_str(),
              static_cast<char *>(nullptr));
        std::perror("execl /bin/bash");
        _exit(127);
    }

    int status = 0;
    if (waitpid(pid, &status, 0) == -1) {
        throw std::runtime_error("waitpid failed: " + std::string(std::strerror(errno)));
    }

    if (!WIFEXITED(status)) {
        throw std::runtime_error("xsdb command did not exit normally");
    }
    if (WEXITSTATUS(status) != 0) {
        throw std::runtime_error(
            "xsdb command failed with exit code " + std::to_string(WEXITSTATUS(status)));
    }
}

void validateOptions(const WriteStaticShell::Options& options) {
    if (!options.flash && !options.jtag) {
        throw std::invalid_argument("either --flash or --jtag must be specified");
    }

    const std::string shellType = effectiveShellType(options);

    if (options.flash) {
        if (options.bdf.empty()) {
            throw std::invalid_argument("-d/--device is required for --flash");
        }
        if (options.noRemoveDevice) {
            throw std::invalid_argument("--no-remove-device is only valid with --jtag");
        }
        if (!options.bashSources.empty()) {
            throw std::invalid_argument("--bash-source is only valid with --jtag");
        }
        if (!options.xsdbTargetId.empty()) {
            throw std::invalid_argument("--xsdb-target-id is only valid with --jtag");
        }
        if (shellTypeAll(shellType) && !options.pdiPath.empty()) {
            throw std::invalid_argument("--shell-type all cannot be used with --pdi");
        }
        return;
    }

    if (shellTypeAll(shellType)) {
        throw std::invalid_argument("--shell-type all is only valid with --flash without --pdi");
    }
    if (options.noRemoveDevice && !options.bdf.empty()) {
        throw std::invalid_argument("--no-remove-device cannot be used with -d/--device");
    }
    if (!options.noRemoveDevice && options.bdf.empty()) {
        throw std::invalid_argument(
            "-d/--device is required for --jtag unless --no-remove-device is used");
    }
}

int runFlashMode(const WriteStaticShell::Options& options) {
    StatusReporter reporter;
    const std::string shellType = effectiveShellType(options);
    reporter.stage("Resolving device address");
    const std::string bdf = resolveBoardBdf(options.bdf, "write-static-shell");

    reporter.stage("Connecting to VRTD");
    vrtd::Session session;

    const std::vector<std::string> shells =
        shellTypeAll(shellType) ? std::vector<std::string>{"service", "compute"}
                                : std::vector<std::string>{shellType};

    for (const auto& shell : shells) {
        const vrtd::ShellType parsedShell = parseShellType(shell);
        const uint32_t partition = shellPartition(parsedShell);
        reporter.stage("Resolving " + shell + " PDI path");
        const std::string pdiPath = resolvePdiPath(options, false, shell);

        reporter.stage("Resolving VRTD device");
        auto device = session.getDeviceByBdf(bdf);
        reporter.stage("Submitting " + shell + " flash program");
        try {
            session.cfgmemProgramFileProgress(
                device,
                pdiPath,
                StaticShellBootDevice,
                partition,
                [&](const vrtd::CfgmemProgramStatus& status) {
                    reporter.progress(status);
                }
            );
        } catch (...) {
            /* Close the open progress line so the error message that unwinds
             * to main() starts on its own line instead of after "Failed". */
            reporter.endProgress();
            throw;
        }
        reporter.endProgress();
    }

    reporter.done("Flash programming complete");

    return 0;
}

void rethrowXsdbAndRescanErrors(std::exception_ptr xsdbError,
                                const std::exception& rescanError) {
    try {
        if (xsdbError) {
            std::rethrow_exception(xsdbError);
        }
    } catch (const std::exception& xsdbException) {
        throw std::runtime_error(
            std::string("xsdb command failed: ") + xsdbException.what() +
            "; hotplug rescan also failed: " + rescanError.what());
    } catch (...) {
        throw std::runtime_error(
            std::string("xsdb command failed with an unknown error; "
                        "hotplug rescan also failed: ") + rescanError.what());
    }

    throw std::runtime_error("hotplug rescan failed: " + std::string(rescanError.what()));
}

int runJtagMode(const WriteStaticShell::Options& options) {
    StatusReporter reporter;
    const std::string effectiveShell = effectiveShellType(options);
    const vrtd::ShellType shellType = parseShellType(effectiveShell);
    reporter.stage("Resolving PDI path");
    const std::string pdiPath = resolvePdiPath(options, true, effectiveShell);
    reporter.stage("Resolving xsdb Tcl script");
    const std::string tclPath = resolveVersalFlashTcl();
    if (!fileExists(tclPath)) {
        throw std::runtime_error("versal_flash_pdi.tcl path does not exist: " + tclPath);
    }

    reporter.stage("Connecting to VRTD");
    vrtd::Session session;
    std::string bdf;
    if (!options.noRemoveDevice) {
        reporter.stage("Resolving device address");
        bdf = resolveBoardBdf(options.bdf, "write-static-shell");
        reporter.stage("Resolving VRTD device");
        auto device = session.getDeviceByBdf(bdf);
        reporter.stage("Removing PCIe functions");
        device.hotplugOp(vrtd::HotplugOp::Remove, vrtd::HotplugFunctionAll);
    }

    std::exception_ptr xsdbError;
    try {
        reporter.stage("Running xsdb");
        runBashCommandWithPdiPath(
            buildXsdbCommand(options.bashSources, tclPath), pdiPath, options.xsdbTargetId);
    } catch (...) {
        xsdbError = std::current_exception();
    }

    try {
        reporter.stage("Rescanning PCIe");
        session.hotplugRescan();
    } catch (const std::exception& e) {
        rethrowXsdbAndRescanErrors(xsdbError, e);
    }

    if (xsdbError) {
        std::rethrow_exception(xsdbError);
    }

    if (!options.noRemoveDevice) {
        reporter.stage("Recording JTAG shell state");
        auto device = session.getDeviceByBdf(bdf);
        device.setShellState(shellType, true);
    }

    reporter.done("JTAG programming complete");

    std::cerr
        << "The board is now booted from the JTAG-loaded image.\n"
        << "The next 'v80-smi reset' will boot the board from flash.\n"
        << "To reset the board in JTAG mode, re-run 'v80-smi write-static-shell --jtag'.\n";
    if (options.noRemoveDevice) {
        std::cerr
            << "VRTD shell/JTAG state was not updated because --no-remove-device was used.\n";
    }

    return 0;
}

} // namespace

int WriteStaticShell::run(const Options& options) {
    validateOptions(options);

    if (options.flash) {
        return runFlashMode(options);
    }

    return runJtagMode(options);
}
