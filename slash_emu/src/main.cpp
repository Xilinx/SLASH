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

#include "config.h"
#include "daemon.h"

#include <cstdio>

int main(int argc, char* argv[]) {
    slash_emu::CliResult cli = slash_emu::parse_cli(argc, argv);
    if (!cli.ok) {
        std::fprintf(stderr, "%s\n", cli.error.c_str());
        return 2;
    }
    if (cli.exit_code != 0) {
        return cli.exit_code;
    }
    // --help / --version return ok with an empty config (CLI11 already printed);
    // there is nothing to run, so exit cleanly.
    if (cli.config.accelerators.empty()) {
        return 0;
    }
    return slash_emu::run_daemon(cli.config);
}
