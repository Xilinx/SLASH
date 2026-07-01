# slash_emu — SLASH System Emulation Daemon

`slash_emu` is a user-space daemon that emulates the SLASH kernel driver via
UNIX domain sockets, enabling application testing without physical hardware.
See `/SLASH/architecture.md` for the full design document.

## Building

### Normal build (no sanitisers)

```sh
cmake -S . -B build/normal -DSLASH_EMU_BUILD_TESTS=ON
cmake --build build/normal -j
```

### AddressSanitizer build

```sh
cmake -S . -B build/asan -DSLASH_EMU_BUILD_TESTS=ON -DENABLE_ASAN=ON
cmake --build build/asan -j
```

### UndefinedBehaviourSanitizer build

```sh
cmake -S . -B build/ubsan -DSLASH_EMU_BUILD_TESTS=ON -DENABLE_UBSAN=ON
cmake --build build/ubsan -j
```

### Combined ASan + UBSan build

```sh
cmake -S . -B build/aubsan -DSLASH_EMU_BUILD_TESTS=ON -DENABLE_ASAN=ON -DENABLE_UBSAN=ON
cmake --build build/aubsan -j
```

## Running the tests

```sh
ctest --test-dir build/normal --output-on-failure
ctest --test-dir build/asan   --output-on-failure
ctest --test-dir build/ubsan  --output-on-failure
ctest --test-dir build/aubsan --output-on-failure
```

## Coverage report

Build with coverage instrumentation, then invoke the `coverage` target:

```sh
cmake -S . -B build/coverage -DSLASH_EMU_BUILD_TESTS=ON -DENABLE_COVERAGE=ON
cmake --build build/coverage -j
cmake --build build/coverage --target coverage
```

The HTML report is written to `build/coverage/coverage_html/index.html`.

Note: `ENABLE_COVERAGE` is mutually exclusive with `ENABLE_ASAN` and
`ENABLE_UBSAN`; CMake will error if both are set simultaneously.

## Running the daemon

```sh
./build/normal/src/slash_emu_daemon
```

Send `SIGINT` or `SIGTERM` to shut it down cleanly.
