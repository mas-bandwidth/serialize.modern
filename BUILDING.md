How to build serialize.modern
=============================

serialize.modern builds with [CMake](https://cmake.org) (3.16 or newer) on Windows, MacOS and Linux.
It requires a C++23 compiler: recent clang, GCC 12 or newer, or MSVC 2022 (the CMake targets request
`cxx_std_23` automatically). For older toolchains use
[classic serialize](https://github.com/mas-bandwidth/serialize) — the wire formats are identical.

## Building

Go to the command line under the serialize directory and enter:

    cmake -B build
    cmake --build build --config Release

Then run the tests:

    ctest --test-dir build --build-config Release --output-on-failure

Or run the binaries directly:

    ./build/bin/test
    ./build/bin/example

On Windows the binaries are under `build\bin\Release`, and you can open the generated `build\serialize.sln` in Visual Studio if you prefer to work there.

## Using serialize.modern in your project

serialize is a single header. The simplest thing is to copy `serialize.h` into your project.

If you use CMake, you can consume it as a target instead — via FetchContent:

    include(FetchContent)
    FetchContent_Declare(serialize GIT_REPOSITORY https://github.com/mas-bandwidth/serialize.modern.git GIT_TAG main)
    FetchContent_MakeAvailable(serialize)
    target_link_libraries(your_target PRIVATE serialize::serialize)

or via `add_subdirectory`, or install it (`cmake --install build`) and use `find_package(serialize CONFIG REQUIRED)`. In all cases the `serialize::serialize` target carries only the include path: none of this repo's warning or fast-math flags leak into your build, and the test/example/benchmark targets are only built when serialize is the top level project.

The library version is available as `SERIALIZE_VERSION` (and `SERIALIZE_VERSION_MAJOR/MINOR/PATCH`) after including the header.

## Debug builds

    cmake -B build-debug -DCMAKE_BUILD_TYPE=Debug
    cmake --build build-debug --config Debug
    ctest --test-dir build-debug --build-config Debug --output-on-failure

Debug builds define `SERIALIZE_DEBUG`, which enables asserts and extra bounds checking. Release builds define `SERIALIZE_RELEASE`.

## Benchmarking

A throughput benchmark for the bitpacker and the stream/macro path builds as part of the normal build:

    cmake -B build && cmake --build build --config Release
    ./build/bin/bench

Only release build numbers are meaningful (the binary warns if you run a debug build). CI runs the benchmark on the release jobs for each platform — treat those numbers as indicative only, since shared runners are noisy.

## Wire compatibility tests

serialize.modern promises byte-identical wire output to classic serialize, gated in CI on every
pull request. To run the gate locally, point the build at a classic checkout:

    git clone --branch v1.4.3 https://github.com/mas-bandwidth/serialize.git classic
    cmake -B build -DSERIALIZE_WIRE_COMPAT_CLASSIC_DIR=$(pwd)/classic
    cmake --build build --config Release
    ctest --test-dir build --build-config Release -R wire_compat --output-on-failure

This builds `wire_compat.cpp` against both libraries: the two corpus streams must be byte-identical
and each library must decode the stream the other wrote.

## Fuzzing

A libFuzzer harness lives in `fuzz.cpp`. Each input is run two ways: as hostile bytes fed to every `ReadStream` primitive, and as a source of values for a write→read round trip that traps on any mismatch. It needs clang (Apple clang doesn't ship the libFuzzer runtime, so use Linux or Homebrew LLVM on MacOS):

    cmake -B build-fuzz -DCMAKE_BUILD_TYPE=Debug -DSERIALIZE_FUZZ=ON -DCMAKE_CXX_COMPILER=clang++
    cmake --build build-fuzz
    ./build-fuzz/bin/fuzz

Build it in Debug so asserts stay enabled: reads from a `ReadStream` must fail by returning false, never by tripping an assert, and the fuzzer treats an assert as a crash. CI runs this harness for 60 seconds on every push, and for an hour nightly with a corpus that accumulates across runs. If a nightly run finds a crash, the reproducer input is uploaded as a workflow artifact.

If you have questions please create an issue at https://github.com/mas-bandwidth/serialize.modern and I'll do my best to help you out.

cheers

 - Glenn
