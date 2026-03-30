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

/// @file smi.cpp
///
/// Entry point for the SMI (System Management Interface) CLI tool.
///
/// Parses command-line arguments using CLI11 and dispatches to the
/// appropriate command handler (version, inspect, query, list, program).

#include <iostream>
#include <string_view>

#include <CLI/CLI.hpp>

#include "inspect.hpp"
#include "list.hpp"
#include "program.hpp"
#include "reset.hpp"
#include "validate.hpp"
#include "version.hpp"

// Forward declarations
static int smiMain(int argc, char **argv);
static int version(bool plain);

/// Top-level entry point. Wraps smiMain() in a catch-all so that
/// unhandled exceptions produce a readable error instead of a crash.
int main(int argc, char** argv) {
    try {
        return smiMain(argc, argv);
    } catch (std::exception& e) {
        std::cerr << "SMI execution failed: " << e.what() << std::endl;
        return 1;
    } catch (...) {
        std::cerr << "SMI execution failed with unknown error" << std::endl;
        return 1;
    }
}

/// The real main function — sets up CLI11 subcommands, parses argv,
/// and routes to the matching command handler.
static int smiMain(int argc, char **argv) {
    CLI::App app{std::string("SMI v") + VERSION};
    // Require [0, 1] subcommands.
    // Without this positional arguments can get interpreted as commands.
    app.require_subcommand(0, 1);

    // -- version --
    auto* versionCommand = app.add_subcommand("version", "Print version information and exit");
    bool versionPlain{};
    versionCommand->add_flag("-p,--plain", versionPlain, "Print only the version in x.y.z format and nothing else (useful in scripting)");

    // -- inspect (file on disk) --
    auto* inspectCommand = app.add_subcommand("inspect", "Inspect vbin file");
    Inspect::Options inspectOptions;
    inspectCommand->add_option("vbin", inspectOptions.vbinPath, "Path to vbin file")->required();
    inspectCommand->add_flag("-j,--json", inspectOptions.jsonOutput, "Print information as compact json (default is human-readable)");
    inspectCommand->add_flag("-J,--pretty-json", inspectOptions.prettyJsonOutput, "Print information as json with indentation (default is human-readable)");

    // -- query (inspect what's loaded on a device) --
    auto* queryCommand = app.add_subcommand("query", "Query vbin file last loaded on device");
    Query::Options queryOptions{.isBdfQuery=true};
    queryCommand->add_option("-d,--device", queryOptions.bdf, "Board address (e.g. 03:00 or 0000:03:00)")->required();
    queryCommand->add_flag("-j,--json", queryOptions.jsonOutput, "Print information as compact json (default is human-readable)");
    queryCommand->add_flag("-J,--pretty-json", queryOptions.prettyJsonOutput, "Print information as json with indentation (default is human-readable)");

    // -- list (enumerate devices) --
    auto* listCommand = app.add_subcommand("list", "List V80 devices");
    List::Options listOptions;
    listCommand->add_flag("-j,--json", listOptions.jsonOutput, "Print information as compact json (default is human-readable)");
    listCommand->add_flag("-J,--pretty-json", listOptions.prettyJsonOutput, "Print information as json with indentation (default is human-readable)");
    listCommand->add_flag("-l,--long", listOptions.longOutput, "Print additional information");
    listCommand->add_flag("-s,--sensors", listOptions.sensorsOutput, "Include sensor readings (requires VRTD)");

    // -- program (load vbin onto device) --
    auto* programCommand = app.add_subcommand("program", "Program a hardware device");
    Program::Options programOptions;
    programCommand->add_option("vbin", programOptions.vbinPath, "Path to vbin file")->required();
    programCommand->add_option("-d,--device", programOptions.bdf, "Board address (e.g. 03:00 or 0000:03:00)")->required();

    // -- reset (hardware reset of board) --
    auto* resetCommand = app.add_subcommand("reset", "Hardware reset a V80 board");
    Reset::Options resetOptions;
    resetCommand->add_option("-d,--device", resetOptions.bdf, "Board address (e.g. 03:00 or 0000:03:00)")->required();

    // -- validate (memory integrity + bandwidth) --
    auto* validateCommand = app.add_subcommand("validate", "Validate board memory (integrity + bandwidth)");
    Validate::Options validateOptions;
    validateCommand->add_option("-d,--device", validateOptions.bdf, "Board address (e.g. 03:00 or 0000:03:00)")->required();
    validateCommand->add_option("-j,--threads", validateOptions.threads,
        "Number of parallel buffers/threads (1-64)")->default_val(8)->check(CLI::Range(1u, 64u));

    CLI11_PARSE(app, argc, argv);

    // Route commands
    if (versionCommand->parsed()) {
        return version(versionPlain);
    } else if (inspectCommand->parsed()) {
        return Inspect::run(inspectOptions);
    } else if (queryCommand->parsed()) {
        return Query::run(queryOptions);
    } else if (listCommand->parsed()) {
        return List::run(listOptions);
    } else if (programCommand->parsed()) {
        return Program::run(programOptions);
    } else if (resetCommand->parsed()) {
        return Reset::run(resetOptions);
    } else if (validateCommand->parsed()) {
        return Validate::run(validateOptions);
    } else {
        // No subcommand given - print help and exit with error.
        std::cerr << app.help() << std::endl;
        return 1;
    }
}

/// Print version information and exit.
static int version(bool plain) {
    if (!plain) {
        std::cout << "SMI v";
    }

    std::cout << VERSION << std::endl;

    return 0;
}

