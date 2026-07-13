# Introduction

[![CI](https://github.com/mas-bandwidth/serialize.modern/actions/workflows/ci.yml/badge.svg)](https://github.com/mas-bandwidth/serialize.modern/actions/workflows/ci.yml)

**serialize.modern** is the modern C++ port of [serialize](https://github.com/mas-bandwidth/serialize), a simple bitpacking serializer for C++.

> **Status: pre-release.** serialize.modern is new and experimental. The wire format is pinned and
> gated against classic serialize in CI, but the library itself — especially the compile time
> schema language — is young, still evolving, and has not shipped in production anywhere. It is
> **not** mature and production ready the way the other ports are: if you need battle tested today,
> use [classic serialize](https://github.com/mas-bandwidth/serialize), which is wire compatible
> with this library.
>
> **Feedback is very much appreciated — especially on the schema language design**: the field
> vocabulary, the branch/match/back-reference model, the array and string strategies, and anything
> you find missing or awkward. Please open an issue at
> [github.com/mas-bandwidth/serialize.modern/issues](https://github.com/mas-bandwidth/serialize.modern/issues).

It requires C++23 and produces **byte-identical wire output to classic serialize**: data written by either library reads back correctly with the other. This is enforced in CI on every pull request — a harness is built against both libraries, the two streams must match byte-for-byte, and each library must decode the stream the other wrote.

It has the following features:

* Serialize a bool with only one bit
* Serialize any integer value from [1,64] bits writing only that number of bits to the buffer
* Serialize signed integer values with [min,max] writing only the required bits to the buffer
* Serialize floats, doubles, compressed floats, strings, byte arrays, and integers relative to another integer
* Alignment support so you can align your bitstream to a byte boundary whenever you want
* Optional template-based serialization so you can write one function that handles both read and write
* A compile time [schema language](SCHEMA.md) that generates zero-overhead serialization code: ~3x faster reads and up to ~7x faster writes than the stream path, byte-identical on the wire

# Requirements

A C++23 compiler: recent clang, GCC 12+, or MSVC 2022 (`-std=c++23` / `/std:c++latest`). If you need to support older toolchains, use [classic serialize](https://github.com/mas-bandwidth/serialize) — the wire formats are identical, so the two can interoperate across a network.

# Usage

The API is the same as classic serialize. You can use the bitpacker directly:

```c++
const int BufferSize = 256;

uint8_t buffer[BufferSize + 8];         // + 8: buffer allocations extend 8 bytes past the end (see below)

serialize::BitWriter writer( buffer, BufferSize );

writer.WriteBits( 0, 1 );
writer.WriteBits( 1, 1 );
writer.WriteBits( 10, 8 );
writer.WriteBits( 255, 8 );
writer.WriteBits( 1000, 10 );
writer.WriteBits( 50000, 16 );
writer.WriteBits( 9999999, 32 );
writer.FlushBits();

const int bytesWritten = writer.GetBytesWritten();

serialize::BitReader reader( buffer, bytesWritten );

uint32_t a = reader.ReadBits( 1 );
uint32_t b = reader.ReadBits( 1 );
uint32_t c = reader.ReadBits( 8 );
uint32_t d = reader.ReadBits( 8 );
uint32_t e = reader.ReadBits( 10 );
uint32_t f = reader.ReadBits( 16 );
uint32_t g = reader.ReadBits( 32 );
```

Or you can write serialize methods for your types:

```c++
struct Vector
{
    float x,y,z;

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_float( stream, x );
        serialize_float( stream, y );
        serialize_float( stream, z );
        return true;
    }
};

struct Quaternion
{
    float x,y,z,w;

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_float( stream, x );
        serialize_float( stream, y );
        serialize_float( stream, z );
        serialize_float( stream, w );
        return true;
    }
};

struct RigidBody
{
    Vector position;
    Quaternion orientation;
    Vector linearVelocity;
    Vector angularVelocity;
    bool atRest;

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_object( stream, position );
        serialize_object( stream, orientation );
        serialize_bool( stream, atRest );
        if ( !atRest )
        {
            serialize_object( stream, linearVelocity );
            serialize_object( stream, angularVelocity );
        }
        else if ( Stream::IsReading )
        {
            linearVelocity.x = linearVelocity.y = linearVelocity.z = 0.0;
            angularVelocity.x = angularVelocity.y = angularVelocity.z = 0.0;
        }
        return true;
    }
};
```

See [example.cpp](example.cpp) for more.

# Compile time schemas

Describe a packet as a schema type and every field offset, shift and mask becomes a compile time
constant: reads compile to independent constant-offset loads, writes to ORs into a handful of
registers. Measured on Apple Silicon: **~3x faster reads and up to ~7x faster writes** than the
stream path, with byte-identical wire output to the equivalent serialize methods — schema and
stream code interoperate freely.

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
bool ok = BodySchema::Read( buffer, bytesWritten, body );       // validates and rejects bad packets, like ReadStream
```

The language covers compile time if/else and switch (including back references to previously
serialized fields, statically validated so forward references are compile errors), fixed and
runtime-count arrays, strings and runtime-length byte blocks, relative integers, protocol
constants and reserved bits, enums and compressed floats, and composition equivalent to
serialize_object. `Schema::MeasureBits( object )` returns the exact serialized size for an object.
Reads validate everything and reject malformed packets, exactly like the read stream.

**The full reference is [SCHEMA.md](SCHEMA.md)** — field vocabulary, wire formats, the code size
model, and what should stay on the streams.

The zero-overhead property is enforced in CI on GCC, clang and MSVC: a codegen audit disassembles
the generated schema functions on every pull request and fails on call instructions, loops,
indirect branches or instruction-count blowups.

# Differences from classic serialize

The wire format is identical. What changed:

* **One buffer rule instead of two.** For both writing and reading, the buffer allocation must extend at least 8 bytes past the end of the data. In exchange, write buffer *sizes* no longer need to be a multiple of 8 — any size works. (Classic requires multiple-of-8 write sizes and the +8 allocation only for reads.)
* **Up to 56 bits move in a single operation.** New `WriteBits64`/`ReadBits64` on the bitpacker and `SerializeBits64` on the streams handle [1,64] bits per call, with widths up to 56 taking a single store or window load. `serialize_int64` with a range that fits 56 bits costs one operation instead of two. The bit stream is LSB-first, so the bytes are identical to the classic 32+32 split.
* **C++23 internals.** `std::bit_width`, `std::bit_cast`, `std::byteswap` and `std::endian` replace the platform macros, compiler builtins and memcpy punning; the serialize macros use `if constexpr`; the bit math is constexpr and usable in `static_assert`.
* **The hot core is explicitly force-inlined**, which measures significantly faster on the stream read and write paths.
* `FlushBits` is still required after writing, exactly as in classic serialize.
* The serialize macros' internal temporaries use reserved-style names (`serialize_temp_*`), so a
  variable named e.g. `uint64_value` can no longer be silently shadowed into serializing zero.
  (This applies to the `read_*`/`write_*` macro families too, which are otherwise identical to
  classic.)

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

The raw bitpacker is at parity — the classic 64 bit scratch, qword flush writer was measured against a fully branchless store-per-write design and kept, because it won on every benchmark. Stream writes are ~45% faster (force-inlined hot core plus an inline fast path for small byte copies). Stream reads compile to instruction-identical code; the residual difference in the table is benchmark code/data placement sensitivity, not the serializer (the same binaries swing ±15% with 8 byte layout perturbations).

# Limitations

* Buffer allocations must extend at least 8 bytes past the end of the data, for both writing and reading: the writer flushes whole qwords and the reader loads 64 bit windows at byte granularity. Bytes past the end of written data are only ever written as zeros; bytes past the end of read data are loaded but never interpreted. Buffers do not need any particular alignment: all memory access goes through memcpy.
* Buffer sizes are effectively unlimited, because bit counts are stored in 64 bit signed integers.
* Wide strings are serialized as 32 bits per character, so streams are compatible between platforms with 2 and 4 byte wchar_t, but code points above 0xFFFF are not translated between UTF-16 and UTF-32 platforms.

# Building

See [BUILDING.md](BUILDING.md) for CMake instructions, consuming the library in your project,
running the wire compatibility gate against classic serialize, benchmarking and fuzzing.

# Author

The author of this library is Glenn Fiedler.

Open source libraries by the same author include: [netcode](https://github.com/mas-bandwidth/netcode), [reliable](https://github.com/mas-bandwidth/reliable) and [yojimbo](https://github.com/mas-bandwidth/yojimbo)

If you find this software useful, [please consider sponsoring it](https://github.com/sponsors/mas-bandwidth). Thanks!

# License

[BSD 3-Clause license](https://opensource.org/licenses/BSD-3-Clause).
