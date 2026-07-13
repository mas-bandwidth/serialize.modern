# Introduction

[![CI](https://github.com/mas-bandwidth/serialize.modern/actions/workflows/ci.yml/badge.svg)](https://github.com/mas-bandwidth/serialize.modern/actions/workflows/ci.yml)

**serialize.modern** is the modern C++ port of [serialize](https://github.com/mas-bandwidth/serialize), a simple bitpacking serializer for C++.

> **Status: pre-release and experimental.** The wire format is pinned, but the library — especially
> the schema language — is young, still evolving, and has not shipped in production anywhere. If
> you need battle tested today, use [classic serialize](https://github.com/mas-bandwidth/serialize);
> the two are wire compatible, so you can switch later without a protocol change. Feedback is very
> much appreciated — see [below](#feedback).

It requires C++23 and produces **byte-identical wire output to classic serialize**: data written by either library reads back correctly with the other. This is enforced in CI on every pull request — a harness is built against both libraries, the two streams must match byte-for-byte, and each library must decode the stream the other wrote.

It shares the following features with classic serialize:

* Serialize a bool with only one bit
* Serialize any integer value from [1,64] bits writing only that number of bits to the buffer
* Serialize signed integer values with [min,max] writing only the required bits to the buffer
* Serialize floats, doubles, compressed floats, strings, byte arrays, and integers relative to another integer
* Alignment support so you can align your bitstream to a byte boundary whenever you want
* Optional template-based serialization so you can write one function that handles both read and write

And has one new feature:

* A compile time [schema language](SCHEMA.md) that generates zero-overhead serialization code: ~3x faster reads and up to ~7x faster writes than the stream path, byte-identical on the wire

The classic API — bitpacker, streams, the `serialize_*` macros — is carried over unchanged; see the [classic documentation](https://github.com/mas-bandwidth/serialize) and [example.cpp](example.cpp) for usage.

# Requirements

A C++23 compiler: recent clang, GCC 12+, or MSVC 2022 (`-std=c++23` / `/std:c++latest`). If you need to support older toolchains, use [classic serialize](https://github.com/mas-bandwidth/serialize) — the wire formats are identical, so the two can interoperate across a network.

# Compile time schemas

The distinguishing feature of this library is the new compile time schema support. With it you can describe structs and generate code that does all the work entirely at compile time, generating fully optimal read and write machine code for each path:

```c++
struct Vec { float x, y, z; };

struct Body { Vec position; bool atRest; Vec velocity; };

using VecSchema = serialize::schema<
    serialize::float_<&Vec::x>,
    serialize::float_<&Vec::y>,
    serialize::float_<&Vec::z> >;

using BodySchema = serialize::schema<
    serialize::object<&Body::position, VecSchema>,        // the equivalent of serialize_object
    serialize::branch<&Body::atRest,                      // if true do this, else do that -- at compile time
        serialize::fields<>,
        serialize::fields< serialize::object<&Body::velocity, VecSchema> > > >;

int64_t bytesWritten = BodySchema::Write( buffer, BufferSize, body );

bool ok = BodySchema::Read( buffer, bytesWritten, body );
```

The language covers compile time if/else and switch (including back references to previously
serialized fields, statically validated so forward references are compile errors), fixed and
runtime-count arrays, strings and runtime-length byte blocks, relative integers, protocol
constants and reserved bits, enums and compressed floats, and composition equivalent to
serialize_object.

Because a schema sees the packet as a type, mistakes stream code cannot catch are compile errors
here: a float member in a `bits` field, a range the member type cannot hold, the same member
serialized twice, a forward reference. Each is a named, one-line `static_assert` stating the
mistake and the fix — not a template trace.

Full reference in [SCHEMA.md](SCHEMA.md).

The zero-overhead property is enforced in CI on GCC, clang and MSVC: a codegen audit disassembles
the generated schema functions on every pull request and fails on call instructions, loops,
indirect branches or instruction-count blowups.

# Performance

Measured on Apple Silicon (Apple clang, Release, medians of interleaved runs) against classic serialize 1.4.3:

| benchmark | classic | serialize.modern |
|---|---|---|
| bitpacker write | 5,905 MB/s | 5,962 MB/s |
| bitpacker read | 8,184 MB/s | 8,138 MB/s |
| bitpacker write (random widths) | 3,774 MB/s | 3,733 MB/s |
| bitpacker read (random widths) | 3,652 MB/s | 3,648 MB/s |
| stream write | 44.3 M packets/s | ~64 M packets/s |
| stream read | ~135 M packets/s | ~120 M packets/s |
| schema write | — | ~520 M packets/s |
| schema read | — | ~430 M packets/s |

The raw bitpacker is at parity — the classic 64 bit scratch, qword flush writer was measured against a fully branchless store-per-write design and kept, because it won on every benchmark. Stream writes are ~45% faster (force-inlined hot core plus an inline fast path for small byte copies). Stream reads compile to instruction-identical code; the residual difference in the table is benchmark code/data placement sensitivity, not the serializer (the same binaries swing ±15% with 8 byte layout perturbations). Schema read and write are radically faster than classical because the read and write codepaths are completely optimized down at compile time: on the benchmark packet that comes to **~12x classic's stream writes and ~3x its stream reads**, reproduced by the shipped [bench.cpp](bench.cpp), which verifies the schema bytes are identical to the stream path before timing.

# Limitations

* Buffer allocations must extend at least 8 bytes past the end of the data for both reading and writing.
* Wide strings are serialized as 32 bits per character, so streams are compatible between platforms with 2 and 4 byte wchar_t, but code points above 0xFFFF are not translated between UTF-16 and UTF-32 platforms.

# Building

See [BUILDING.md](BUILDING.md) for CMake instructions, consuming the library in your project,
running the wire compatibility gate against classic serialize, benchmarking and fuzzing.

# Feedback

The schema language is the part of this library that needs your eyes: the field vocabulary, the
branch/match/back-reference model, the array and string strategies — anything missing or awkward.
Please open an issue at
[github.com/mas-bandwidth/serialize.modern/issues](https://github.com/mas-bandwidth/serialize.modern/issues).
An honest, maintained list of the library's current weaknesses lives in
[WEAKNESSES.md](WEAKNESSES.md).

# Author

The author of this library is Glenn Fiedler.

Open source libraries by the same author include: [netcode](https://github.com/mas-bandwidth/netcode), [reliable](https://github.com/mas-bandwidth/reliable) and [yojimbo](https://github.com/mas-bandwidth/yojimbo)

If you find this software useful, [please consider sponsoring it](https://github.com/sponsors/mas-bandwidth). Thanks!

# License

[BSD 3-Clause license](https://opensource.org/licenses/BSD-3-Clause).
