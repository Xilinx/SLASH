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
/// appropriate command handler (version, inspect, query, list, program,
/// reset, validate, debug).

#include <iostream>
#include <string_view>

#include <CLI/CLI.hpp>

#include "debug/bar_poke.hpp"
#include "debug/clockwiz.hpp"
#include "debug/mem_poke.hpp"
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
    validateCommand->add_flag("-R,--no-reset", validateOptions.noReset,
        "Skip the device reset step before running memory tests");

    // -- debug (low-level debug utilities) --
    auto* debugCommand = app.add_subcommand("debug", "Low-level debug utilities");
    debugCommand->require_subcommand(1, 1);

    auto* barPokeCommand = debugCommand->add_subcommand("bar-poke", "Read or write BAR words");
    BarPoke::Options barPokeOptions;
    barPokeCommand->add_option("-d,--device", barPokeOptions.bdf, "Board address (e.g. 03:00 or 0000:03:00)")->required();
    barPokeCommand->add_option("-b,--bar", barPokeOptions.bar, "BAR number (0-5)")->required()->check(CLI::Range(0u, 5u));
    barPokeCommand->add_flag("-r,--read", barPokeOptions.readMode, "Read words from BAR");
    barPokeCommand->add_flag("-w,--write", barPokeOptions.writeMode, "Write one word to BAR");
    barPokeCommand->add_flag("-x,--hex", barPokeOptions.hexMode, "Print read output in hexadecimal");
    barPokeCommand->add_option("-W,--word-size", barPokeOptions.wordSize, "Word size in bytes (1, 2, 4, 8)")
        ->default_val(4)->check(CLI::IsMember({1u, 2u, 4u, 8u}));
    barPokeCommand->add_option("-c,--count", barPokeOptions.count, "Number of words to read (must be 1 for write)")
        ->default_val(1);
    barPokeCommand->add_option("address", barPokeOptions.addressText,
        "BAR-relative address (0x... for hex, decimal otherwise)")->required();
    barPokeCommand->add_option("value", barPokeOptions.valueText,
        "Value for --write (0x... for hex, decimal otherwise)");

    auto* clockwizCommand = debugCommand->add_subcommand("clockwiz", "Read or set clock rates via vrtd clock-op");
    Clockwiz::Options clockwizOptions;
    clockwizCommand->add_option("-d,--device", clockwizOptions.bdf, "Board address (e.g. 03:00 or 0000:03:00)")->required();
    clockwizCommand->add_flag("--get", clockwizOptions.getMode, "Read clock rate for selected region");
    clockwizCommand->add_option("--set", clockwizOptions.setRateText, "Set requested clock rate in Hz for selected region");
    clockwizCommand->add_option("--region", clockwizOptions.regionText, "Clock region: user or service")
        ->default_val("user");
    clockwizCommand->add_flag("-x,--hex", clockwizOptions.hexMode, "Print --get output in hexadecimal");

    auto* memPokeCommand = debugCommand->add_subcommand("mem-poke",
        "Read or write device memory at a raw physical address (bypasses allocator; requires raw-mem-access permission). "
        "Use --region to declare the target memory space and validate address bounds.");
    MemPoke::Options memPokeOptions;
    memPokeCommand->add_option("-d,--device", memPokeOptions.bdf, "Board address (e.g. 03:00 or 0000:03:00)")->required();
    memPokeCommand->add_option("--region,-r", memPokeOptions.regionText,
        "Memory region: DDR, HBM, HBM0..HBM63, or RAW (no bounds check)")->required();
    memPokeCommand->add_flag("--read", memPokeOptions.readMode, "Read words from device memory");
    memPokeCommand->add_flag("--write,-w", memPokeOptions.writeMode, "Write one word to device memory");
    memPokeCommand->add_flag("-x,--hex", memPokeOptions.hexMode, "Print read output in hexadecimal");
    memPokeCommand->add_flag("--relative", memPokeOptions.relativeAddress,
        "Interpret address as relative to the region base address");
    memPokeCommand->add_flag("--print-base-address", memPokeOptions.printBaseAddress,
        "Print the region base address in hex and exit (mutually exclusive with I/O flags)");
    memPokeCommand->add_flag("--print-size", memPokeOptions.printSize,
        "Print the region size in bytes in hex and exit (mutually exclusive with I/O flags)");
    memPokeCommand->add_option("-W,--word-size", memPokeOptions.wordSize, "Word size in bytes (1, 2, 4, 8)")
        ->default_val(4)->check(CLI::IsMember({1u, 2u, 4u, 8u}));
    memPokeCommand->add_option("-c,--count", memPokeOptions.count, "Number of words to read (must be 1 for write)")
        ->default_val(1);
    memPokeCommand->add_option("address", memPokeOptions.addressText,
        "Device physical address (0x... for hex, decimal otherwise); relative to region base if --relative");
    memPokeCommand->add_option("value", memPokeOptions.valueText,
        "Value for --write (0x... for hex, decimal otherwise)");
    memPokeCommand->add_option("-f,--file", memPokeOptions.filePath,
        "File path: source for --write, destination for --read. "
        "With -x: hexdump format (no 0x prefix); without -x: raw binary. "
        "In file mode -W and -c determine the byte count (-W * -c), not word alignment.");

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
    } else if (barPokeCommand->parsed()) {
        return BarPoke::run(barPokeOptions);
    } else if (clockwizCommand->parsed()) {
        return Clockwiz::run(clockwizOptions);
    } else if (memPokeCommand->parsed()) {
        return MemPoke::run(memPokeOptions);
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

