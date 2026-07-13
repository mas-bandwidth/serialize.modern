# Introduction

[![CI](https://github.com/mas-bandwidth/serialize.modern/actions/workflows/ci.yml/badge.svg)](https://github.com/mas-bandwidth/serialize.modern/actions/workflows/ci.yml)

**serialize.modern** is the modern C++ port of [serialize](https://github.com/mas-bandwidth/serialize), a simple bitpacking serializer for C++.

It requires C++23 and produces **byte-identical wire output to classic serialize**: data written by either library reads back correctly with the other. This is enforced in CI on every pull request — a harness is built against both libraries, the two streams must match byte-for-byte, and each library must decode the stream the other wrote.

It has the following features:

* Serialize a bool with only one bit
* Serialize any integer value from [1,64] bits writing only that number of bits to the buffer
* Serialize signed integer values with [min,max] writing only the required bits to the buffer
* Serialize floats, doubles, compressed floats, strings, byte arrays, and integers relative to another integer
* Alignment support so you can align your bitstream to a byte boundary whenever you want
* Optional template-based serialization so you can write one function that handles both read and write

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

For packets whose layout is fixed at compile time, serialize.modern can go much faster than the
stream path: describe the packet as a schema type and every field's offset, shift and mask becomes
a compile time constant — reads compile to independent constant-offset loads, writes to ORs into a
handful of registers. Measured on Apple Silicon: ~3x faster reads and up to ~7x faster writes,
with byte-identical wire output to the equivalent serialize methods (so schema and stream code
interoperate freely).

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

Schemas compose (`object` splices inner schemas in place at compile time) and support
conditional structure (`branch` serializes a bool then one of two field lists; every branch
outcome gets its own fully constant layout, so each conditional roughly doubles the generated code
for what follows it).

Loops and variable-length data are covered by three strategies:

* `array` / `bits_array` — fixed-count arrays: the compile time for loop, fully unrolled, every
  element at constant offsets.
* `array_n` — a runtime count up to a small maximum (the items array extent, capped at 16): the
  generated code contains one fully constant path per possible count, selected by forward compares,
  so zero overhead survives a runtime count. Cost is code size: keep the maximum small and put
  counted arrays near the end of the schema.
* `string` / `bytes_n` — runtime-length sections: after the content, the rest of the schema
  continues at a runtime byte offset as a fresh compile time run, so every shift and mask stays a
  constant and only the base pointer is a register. The length-bounded copy is the only loop.

Measured on a packet mixing a string, a runtime byte block and a runtime-count array of vectors
(lengths and counts varying unpredictably): schema reads 240 M packets/s vs 61 M for the stream
path — 3.9x. Unbounded collections stay on the streams.

Decisions can also back-reference values serialized earlier in the schema, consuming no wire
bits: `branch_on<&P::flag, Then, Else>` branches on a previously serialized bool, and
`match<&P::type, case_<V, ...>...>` switches on a previously serialized integer (a value matching
no case serializes nothing, identically on write and read). By the time the runner reaches the
decision point the referenced member has already been decoded, so the layout tree still forks
entirely at compile time — the member must be serialized by an earlier field, the same ordering
discipline a hand written serialize method needs.

Full field vocabulary: `bits`, `int_`, `int64`, `bool_`, `float_`, `double_`, `align`, `bytes`,
`branch`, `branch_on`, `match`/`case_`, `object`, `array`, `bits_array`, `array_n`, `string`,
`bytes_n`.

The zero-overhead property is enforced in CI: a codegen audit disassembles the generated schema
Read/Write functions on every pull request and fails if calls, loops or instruction-count blowups
appear. Fixed-layout schemas are held to straight-line, call-free code; schemas with runtime-length
sections are audited under a documented profile that allows the length-bounded copies but still
enforces instruction budgets.

# Differences from classic serialize

The wire format is identical. What changed:

* **One buffer rule instead of two.** For both writing and reading, the buffer allocation must extend at least 8 bytes past the end of the data. In exchange, write buffer *sizes* no longer need to be a multiple of 8 — any size works. (Classic requires multiple-of-8 write sizes and the +8 allocation only for reads.)
* **Up to 56 bits move in a single operation.** New `WriteBits64`/`ReadBits64` on the bitpacker and `SerializeBits64` on the streams handle [1,64] bits per call, with widths up to 56 taking a single store or window load. `serialize_int64` with a range that fits 56 bits costs one operation instead of two. The bit stream is LSB-first, so the bytes are identical to the classic 32+32 split.
* **C++23 internals.** `std::bit_width`, `std::bit_cast`, `std::byteswap` and `std::endian` replace the platform macros, compiler builtins and memcpy punning; the serialize macros use `if constexpr`; the bit math is constexpr and usable in `static_assert`.
* **The hot core is explicitly force-inlined**, which measures significantly faster on the stream read and write paths.
* `FlushBits` is still required after writing, exactly as in classic serialize.
* Classic's `read_*`/`write_*` macro families are removed. For separate read and write functions,
  alias the stream type — `using Stream = serialize::ReadStream;` — and the unified `serialize_*`
  macros work unchanged.
* The serialize macros' internal temporaries use reserved-style names (`serialize_temp_*`), so a
  variable named e.g. `uint64_value` can no longer be silently shadowed into serializing zero.

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

# Author

The author of this library is Glenn Fiedler.

Open source libraries by the same author include: [netcode](https://github.com/mas-bandwidth/netcode), [reliable](https://github.com/mas-bandwidth/netcode) and [yojimbo](https://github.com/mas-bandwidth/yojimbo)

If you find this software useful, [please consider sponsoring it](https://github.com/sponsors/mas-bandwidth). Thanks!

# License

[BSD 3-Clause license](https://opensource.org/licenses/BSD-3-Clause).
