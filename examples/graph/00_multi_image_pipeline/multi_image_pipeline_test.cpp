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

#include <cassert>
#include <initializer_list>
#include <string>
#include <vector>

#define MULTI_IMAGE_PIPELINE_TESTING
#include "multi_image_pipeline.cpp"

namespace {

Cli parse(std::initializer_list<const char*> values) {
    std::vector<std::string> storage;
    storage.reserve(values.size());
    for (const char* value : values) storage.emplace_back(value);

    std::vector<char*> argv;
    argv.reserve(storage.size());
    for (std::string& value : storage) argv.push_back(value.data());
    return parseArgs(static_cast<int>(argv.size()), argv.data());
}

void expectParseFailure(std::initializer_list<const char*> values,
                        const std::string& messagePart) {
    try {
        (void)parse(values);
        assert(false && "parseArgs unexpectedly succeeded");
    } catch (const std::runtime_error& error) {
        assert(std::string(error.what()).find(messagePart) != std::string::npos);
    }
}

}  // namespace

int main() {
    const Cli defaults = parse({"multi_image_pipeline", "--bdf", "0000:01:00.0"});
    assert(defaults.iterations == 2);
    assert(defaults.elementCount == 16);
    assert(defaults.inputOffset == 0);

    const Cli variant = parse({
        "multi_image_pipeline", "--bdf", "0000:01:00.0",
        "--iterations", "3", "--elements", "7", "--input-offset", "-19",
    });
    assert(variant.iterations == 3);
    assert(variant.elementCount == 7);
    assert(variant.inputOffset == -19);

    expectParseFailure(
        {"multi_image_pipeline", "--bdf", "0000:01:00.0", "--iterations", "2x"},
        "invalid value for --iterations");
    expectParseFailure(
        {"multi_image_pipeline", "--bdf", "0000:01:00.0", "--input-offset", "2147483648"},
        "invalid value for --input-offset");
    expectParseFailure(
        {"multi_image_pipeline", "--bdf", "0000:01:00.0", "--input-offset", "2147483647",
         "--elements", "2"},
        "exceeds int32 range");

    assert((generateInput(4, 0) == std::vector<int32_t>{0, 1, 2, 3}));
    assert((generateInput(3, -4) == std::vector<int32_t>{-4, -3, -2}));
    assert((expectedOutput(generateInput(3, 0), 1) ==
            std::vector<int32_t>{122, 122, 124}));
    assert((expectedOutput(generateInput(3, 5), 2) ==
            std::vector<int32_t>{166, 164, 168}));

    return 0;
}
