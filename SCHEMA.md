# The serialize.modern schema language

> **Status: experimental.** The schema language is young and evolving. Feedback on the design —
> the field vocabulary, the branch/match/back-reference model, the array and string strategies,
> and anything missing or awkward — is very much appreciated:
> [open an issue](https://github.com/mas-bandwidth/serialize.modern/issues).

## Why schemas

The classic serialize streams thread a runtime bit cursor through every field: each read depends
on the cursor the previous field just advanced, which serializes the whole decode, and every field
pays its own bounds check. When the packet layout is known at compile time, none of that is
necessary. A schema describes the packet as a type; the library then generates read and write code
in which **every field offset, shift, mask, byte count and copy length is a compile time
constant**. Reads become independent constant-offset loads the CPU executes in parallel; writes
become ORs into a handful of registers followed by a few stores.

Measured on Apple Silicon against the stream path: **~3x faster reads and up to ~7x faster
writes** on a fixed-layout packet (~450 M packets/s in both directions), and **3.9x faster reads**
on a packet mixing a string, a runtime byte block and a runtime-count array.

Two properties are enforced in CI, not just claimed:

* **Wire compatibility**: a schema produces byte-identical output to the equivalent `serialize_*`
  calls, so schema and stream code interoperate freely (and with classic serialize). Every schema
  test pins this with a memcmp against a macro-written twin, in both directions.
* **Zero overhead**: a codegen audit disassembles generated schema functions on GCC, clang and
  MSVC and fails the build on call instructions, loops, indirect branches or instruction-count
  blowups. Fixed-layout schemas must compile to straight-line, call-free code; schemas with
  runtime-length sections are held to a documented profile (the length-bounded copies may loop,
  budgets still apply).

## Getting started

```c++
struct Vec { float x, y, z; };

struct Body { Vec position; bool atRest; Vec velocity; };

using VecSchema = serialize::schema<
    serialize::float_<&Vec::x>,
    serialize::float_<&Vec::y>,
    serialize::float_<&Vec::z> >;

using BodySchema = serialize::schema<
    serialize::object<&Body::position, VecSchema>,
    serialize::branch<&Body::atRest,
        serialize::fields<>,
        serialize::fields< serialize::object<&Body::velocity, VecSchema> > > >;

uint8_t buffer[64 + 8];                                         // + 8: allocations extend 8 bytes past the end

int64_t bytesWritten = BodySchema::Write( buffer, 64, body );   // no flush needed

Body decoded;
if ( !BodySchema::Read( buffer, bytesWritten, decoded ) )
{
    // bad packet: rejected, exactly like the read stream
}
```

A `serialize::schema< Field, Fields... >` provides:

| member | meaning |
|---|---|
| `Write( data, bytes, object )` | serialize the object; returns bytes written. No flush step. |
| `Read( data, bytes, object )` | deserialize and validate; returns false for malformed packets. |
| `MeasureBits( object )` / `MeasureBytes( object )` | the **exact** serialized size for this object (follows its branches, matches, counts and lengths). Unlike `MeasureStream`, alignment is computed from real offsets, so this is exact, not conservative. Also how to learn the size a successful `Read` consumed — e.g. to frame several packets in one buffer, measure the decoded object. |
| `MaxBits` / `MaxBytes` | compile time constants: the longest path through the schema. Size write buffers from `MaxBytes`. |
| `object_type` | the struct being serialized, deduced from the fields. |

**Buffer contract**: same as everywhere in this library — the allocation must extend at least 8
bytes past the end of the data, for both reading and writing.

**Trust model**: same as the streams. Reads validate everything (bounds per path segment, integer
ranges, alignment padding, protocol constants) and return false on any violation, because network
input is the trust boundary. Writes are unchecked in release and assert in debug.

**Members must be addressable**: fields name members by member pointer, so bit-field members
cannot be used (widen them, or keep those packets on the streams).

## Field reference

Member types are checked at compile time: a float member in a `bits` field, a non-bool `branch`
condition, a `string` over the wrong character type — each is a compile error whose message names
the field to use instead, never a silent numeric conversion on the wire.

Capacity is checked too: the declared width or range must fit the member. `bits` wider than the
member type, an `int_` range an `int8_t` member cannot hold, an `array_n` count above the count
member's capacity — each is a compile error, because a legal wire value that truncates on
assignment would be silent corruption that *passes* read validation.

The schema's shape is checked as well. Serializing the same member twice — the copy-paste bug
that writes `y` two times and `z` never — is a compile error (`serialize::unique_writes` is the
trait); members on exclusive `branch`/`match` sides do not collide, since only one side executes.
And all fields must serialize members of a single object type — inner structs compose through
`object<>` — so mixing two packet types in one schema is a named error, not a template trace.


### Integers and scalars

| field | wire | notes |
|---|---|---|
| `bits<&T::m, Bits>` | `Bits` raw bits, `Bits` in [1,64] | unsigned integral or enum member. Wire = `serialize_bits`. |
| `int_<&T::m, Min, Max>` | minimal bits for [Min,Max], value − Min | `int32_t` semantics. Read rejects values above the range. Wire = `serialize_int`. |
| `int64<&T::m, Min, Max>` | minimal bits for the 64 bit range | Wire = `serialize_int64`. |
| `bool_<&T::m>` | 1 bit | Wire = `serialize_bool`. |
| `float_<&T::m>` | 32 raw bits | Wire = `serialize_float`. |
| `double_<&T::m>` | 64 raw bits | Wire = `serialize_double`. |
| `compressed_float_<&T::m, Min, Max, Res>` | quantized to `Res` sized steps | Min/Max/Res are `float` template parameters: the step count and bit width fold at compile time. Read rejects quantized values above the step count. Wire = `serialize_compressed_float`. |
| `enum_<&T::m, MaxValue>` | minimal bits for [0,MaxValue] | scoped or plain enums. Read rejects values above MaxValue. Wire = `serialize_int` over [0,MaxValue]. |

### Protocol plumbing

| field | wire | notes |
|---|---|---|
| `const_<Value, Bits>` | the constant | read **rejects** any other value. Magic numbers, protocol versions, mid-stream sanity markers: corrupt or desynchronized packets fail at the first constant, for the price of a compare against an immediate. |
| `reserved_<Bits>` | zeros | read rejects nonzero. Reserve bits for protocol evolution, then claim them behind a version gate. |
| `align` | zero pad to the next byte | read rejects nonzero padding. Wire = `serialize_align`. |

### Byte blocks and strings

| field | wire | notes |
|---|---|---|
| `bytes<&T::m, NumBytes>` | align, then the bytes | `uint8_t[N]` member, fixed size. Wire = `serialize_bytes`. |
| `bytes_n<&T::data, &T::count>` | count in [0,extent], align, then the bytes | runtime length. Wire = `serialize_int` of the count + `serialize_bytes`. |
| `string<&T::m>` | length in [0,extent−1], align, then the characters | `char[N]` member, null-terminated on read. Wire = `serialize_string`. |
| `wstring_<&T::m>` | length, **align**, then 32 bit code points | `wchar_t[N]` member. **The one wire deviation in the schema language**: `serialize_wstring` does not align before the code points; the schema needs a byte aligned handoff. The stream equivalent is `serialize_int` + `serialize_align` + 32 bits per character. On 2 byte `wchar_t` platforms, reads reject code points above 0xFFFF. |

After a runtime-length section the rest of the schema continues at a runtime byte offset as a
fresh compile time run: shifts and masks stay constants, only the base pointer occupies a
register, and the length-bounded copy is the only loop in the generated code.

### Arrays

| field | wire | notes |
|---|---|---|
| `array<&T::m, Inner, Count = extent>` | the elements, back to back | the compile time for loop: `Inner` (a schema or `fields<...>` over the element type) is spliced once per element, fully unrolled, every element at constant offsets. |
| `bits_array<&T::m, Bits, Count = extent>` | `Count × Bits` bits | the scalar version: an array of unsigned integrals, `Bits` each. |
| `array_n<&T::items, &T::count, Inner, MinCount = 0, MaxCount = extent>` | count in [MinCount,MaxCount] encoded relative to MinCount, then that many elements | bounded runtime count: one **fully constant path per possible count**, selected by forward compares, so zero overhead survives a runtime count. A nonzero MinCount tightens the wire and makes smaller counts inexpressible. The count **range** is capped at 16 (the array extent may be larger). Wire = `serialize_int( count, min, max )` + the element loop. |

### Relative integers

`int_relative_<&T::prev, &T::curr>` — the classic bucket encoding: a difference of 1 costs one
bit, then 2/4/8/12/16 bit buckets, then a 32 bit absolute fallback. Values must be strictly
increasing, and reads reject violations. `prev` is a back reference (see below). Wire =
`serialize_int_relative`.

## Decisions

```c++
serialize::branch<&T::flag, ThenFields, ElseFields>        // serializes the bool, then one side
serialize::branch_on<&T::flag, ThenFields, ElseFields>     // back reference: no wire bits
serialize::match<&T::type,                                 // back referenced switch: no wire bits
    serialize::case_<1, ...fields...>,
    serialize::case_<2, ...fields...>>
```

* `branch` serializes a bool member, then one of two field lists. Wire = `serialize_bool` followed
  by the taken side.
* `branch_on` branches on a bool member **serialized earlier**, consuming no wire bits.
* `match` switches on an integral member serialized earlier, consuming no wire bits. A value that
  matches no case serializes nothing — identically on write and read, so the wire stays symmetric.

In every case the layout tree forks **at compile time**: each outcome gets its own fully constant
layout, and runtime data merely selects which precompiled path executes.

### Back references are validated at compile time

The referenced member must be serialized **unconditionally earlier** in the schema. The schema
walks its flattened field list at compile time, accumulating the member paths that are guaranteed
to be decoded, and every `branch_on`/`match`/`int_relative_` must reference one of them. Forward
references, references to members that are never serialized, and references to members written
only inside one branch side or behind an array count are **compile errors** — on every compiler,
at every optimization level. The trait `serialize::valid_references<Fields...>` exposes the check.

## Composition

`object<&T::member, Inner>` is the schema equivalent of `serialize_object`: it splices `Inner` (a
schema or a `fields<...>` list over the member's type) in place, composing every inner accessor
with the outer member pointer at compile time. Nesting costs nothing at runtime, and every field
kind composes — through object members and through fixed array elements — including branches,
back references (paths compose: a `branch_on` inside `items[2]` references `items[2]`'s member),
counted arrays and strings. A string inside an array element simply hands the remaining elements
off to a runtime base through the same machinery.

## The code size model

Zero overhead is bought with specialized code paths. Each construct multiplies the generated code
for **what follows it** in the schema:

| construct | paths |
|---|---|
| `branch` / `branch_on` | × 2 |
| `match` | × (cases + 1) |
| `array_n` | × (MaxCount − MinCount + 1) |
| `int_relative_` | × 7 |

Keep forking constructs near the end of the schema, keep `array_n` ranges small, and prefer
`branch_on`/`match` over chains of `branch` where a single earlier field drives several decisions.
Fixed-layout fields and runtime-length sections (`string`, `bytes_n`, `wstring_`) do not multiply
paths.

The same multipliers govern compile time: a translation unit with an ordinary fixed-layout schema
compiles as fast as the equivalent stream serialize method (measured at parity), while a TU that
instantiates many schemas full of forking constructs pays proportionally. Measured on the chained
worst case: one maximal `array_n` (17 paths) compiles in 0.2s to ~4 KB of code; two chained (289
paths) in ~2s to ~60 KB; three chained (4,913 paths) in over a minute to more than a megabyte.
Chaining two large forking constructs is the practical ceiling — past that, move the collection to
the streams.

## What stays on the streams

Unbounded collections, count ranges wider than 16, and structure too dynamic to describe as a
type. The streams and `serialize_*` macros remain fully supported — they are the general path, the
`MeasureStream` conservative estimator, and the correctness oracle the schema tests compare
against. Schema and stream code can be mixed freely: the bytes are identical.

## Wire compatibility, precisely

For every field the wire format is identical to the equivalent `serialize_*` calls, field for
field — with the single documented exception of `wstring_`'s alignment, above. This is pinned by
tests that write each schema and its hand-written stream twin and require identical bytes, decode
each with the other, and reject the same malformed inputs. The library-level wire format is
additionally pinned against classic serialize by a byte-identity CI gate on every pull request.
