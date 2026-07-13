# Upcoming C++ and the future design of serialize.modern

*Last revised July 2026. Compiler support notes reflect that date and will rot; the design
analysis should not.*

This library sits deliberately on the newest C++ that all three of its compilers (GCC, clang,
MSVC) actually ship — C++23 today. The next standard changes what a compile time schema language
can be. This document records which upcoming features matter here, what each one enables or
threatens, and the conditions under which we adopt it.

## Ground rules that no future feature changes

* **The wire format is pinned forever.** Any feature adoption produces byte-identical output or
  it does not land. The golden wire test and the classic cross-library CI gate outlast every
  language revision.
* **The classic API surface is immutable.** The streams and the `serialize_*` / `read_*` /
  `write_*` macros stay source-compatible with classic serialize, whatever the internals become.
* **The zero-overhead property is gated, not aspirational.** Internal rewrites enabled by new
  language features must pass the codegen audit unchanged — same straight-line code, same budgets.
* **The language floor moves only when the whole matrix ships.** We bump past C++23 when GCC,
  clang *and* MSVC all ship the feature in a released compiler, not when the first one does.
  Historically that lags the standard by two to three years.
* **Everything is measured first.** The performance record in CLAUDE.md is full of plausible
  ideas that lost to measurement. Future features get the same treatment.

## C++26 static reflection (P2996) — the headline

Today a schema names members by member pointer, and the schema is a parallel description the
author must keep in sync with the struct. Reflection collapses that gap. What it enables, in
order of value:

**1. The missing-member check — the last structural hole.** The schema constraints added in the
red/blue rounds catch wrong types, wrong sizes, duplicate writes and forward references, but they
*cannot* catch omission: add a member to the struct, forget to add a field, and the schema
compiles happily while the member silently never serializes. No amount of template machinery can
enumerate the members of a struct in C++23. With reflection it is a static_assert: every
non-static data member either appears in the schema's write paths or is explicitly marked
skipped. This closes the one bug class the schema language currently shares with hand-written
serialize functions, and it is the first thing to build when reflection lands.

**2. Schemas derived from annotated structs (with P3394 annotations).** The end state is the
schema written next to the data instead of parallel to it:

```c++
struct Packet
{
    [[=serialize::width( 16 )]]        uint32_t sequence;
    [[=serialize::range( -100, 100 )]] int32_t  velocity;
                                       float    position;    // natural width: 32 raw bits
};

using PacketSchema = serialize::auto_schema<Packet>;
```

The existing field vocabulary (SCHEMA.md) does not go away — it becomes the *backend* that
`auto_schema` lowers onto, and stays the explicit form for anything derivation cannot express
(branches, matches, back references). Two hard rules for derivation: it must be opt-in per
member for anything narrower than the member's natural width (silent wire-width guessing is how
formats break), and derived schemas pin their bytes with the same twin tests as hand-written
ones.

**3. Member names in error messages.** Combined with user-generated `static_assert` messages
(below), `schema int_ field: the range does not fit the member type` becomes `Packet::velocity:
range [-1000, 1000] does not fit int8_t`. The constraint machinery already computes everything
except the name.

**4. Generated wire documentation.** A consteval walk over a schema plus `identifier_of` emits
the field-by-field wire layout table that SCHEMA.md maintains by hand today.

*Status and posture:* approved for C++26; experimental in at most one of our three compilers as
of mid 2026, shipping in none. The design above can be prototyped behind a feature-test macro
(`__cpp_reflection`) without moving the language floor, but nothing in the library may *require*
it until the whole matrix ships. Revisit per compiler release.

## User-generated static_assert messages (P2741, C++26)

`static_assert` messages become constant expressions, so the schema constraints can format the
actual numbers into the diagnostic — the offending bit width, the member's capacity, the
duplicate path index. Recorded as the explicit fix for the round-three/four constraint messages,
which currently name the *rule* but cannot name the *values*. Cheap, purely additive, adopt at
floor bump.

## Expansion statements (P1306, C++26)

`template for` over the field list replaces the recursive continuation-passing `runner` and the
recursive consteval offset computation. Two concrete effects:

* **The ~500 field ceiling lifts.** Flat schemas today recurse once per field through consteval
  `prefix_end`, hitting default `-fconstexpr-depth` around 500 fields (measured, SCHEMA.md). An
  expansion-statement runner iterates instead of recursing.
* **Compile time constant factors drop.** Path *multiplication* from forking constructs is
  inherent to zero-overhead specialization and no loop syntax removes it, but the per-path
  instantiation overhead (the 72 s / 4,913-path measurement) should shrink substantially.

This is the largest internal rewrite on the horizon and touches nothing observable: byte-identical
wire, same generated code, gated by the golden test, the twin tests and the codegen audit. Worth
prototyping early precisely because the gates make it safe.

## Pack indexing (P2662, C++26)

`Fields...[I]` simplifies `flatten`, `expand_array` and the dispatch machinery that currently
peel packs recursively. Pure internals cleanup with possible compile-time gains; adopt
opportunistically during the P1306 rewrite, measure compile time before/after.

## Contracts (P2900, C++26)

The library's trust model maps cleanly onto contracts: debug asserts on the write path become
`pre()` conditions (`bits >= 1 && bits <= 64`, value fits width, strictly increasing relative
integers), and the consumer picks the enforcement profile instead of the library picking
`SERIALIZE_DEBUG`. Three cautions, all recorded so they are not relearned:

* **Contracts never replace read-path validation.** Network input is the trust boundary; rejecting
  malformed packets is program logic, not a programmer-error check, and stays as runtime code
  returning false.
* **The most important contract here is unexpressible.** The buffer rule — allocations extend 8
  bytes past the end of the data — cannot be stated as a precondition on a `uint8_t *`; no
  language feature on any horizon verifies allocation extents. It stays documentation plus ASan.
* **Enforced contracts cost branches.** The release write path is unchecked by design and
  measured; any default that enforces in release violates the trust model's performance promise.
  Posture: annotate, default to ignore in release builds, measure observe mode before
  recommending it.

## std::inplace_vector (P0843, C++26)

The first standard container whose capacity is a compile time constant — exactly the shape
`array_n` serializes today as a separate items-array plus count member. A future `vector_n<&T::m>`
field over an `inplace_vector<Element, N>` member would serialize size-then-elements with the
same wire encoding as the equivalent `array_n` (count relative to a min bound, one specialized
path per count, same range-16 cap). No classic macro equivalent exists, so the wire spec is
defined by equivalence to `array_n`, pinned by a twin test against it. Add on user demand once
the matrix ships the container.

## Erroneous behavior (P2795, C++26)

Reading an uninitialized object becomes *erroneous* rather than undefined: serializing an
uninitialized member yields a stable-but-arbitrary value instead of UB. No library change — but
it slightly reshapes the fuzz and sanitizer story (such bugs in user code become deterministic
and diagnosable rather than optimizer-dependent), and packet nondeterminism from uninitialized
members remains a user bug the missing-member check (reflection, above) will catch structurally.

## constexpr cmath (P1383, C++26)

`std::floor` in the compressed float quantization becomes usable in constant evaluation, enabling
consteval unit tests of quantization edge cases (the golden test currently pins one carefully
chosen value that quantizes identically everywhere). Testing improvement only; the runtime code
is already optimal.

## std::simd (P1928, C++26)

Noted to pre-empt the proposal rather than to plan adoption. The bitpacker is a serially
dependent scratch chain — the performance record shows widening it does not pay (the 128-bit
scratch experiment measured exact parity; the scratch sweet spot is the native register width).
SIMD pays on independent lanes, which would mean batch APIs serializing multiple packets at once
— out of scope for this library's design. Expected verdict on measurement: null. Do not propose
without a workload where packet-level parallelism cannot be had more simply by threads.

## Structured binding packs (P1061, C++26)

`auto & [...members] = object;` enumerates an aggregate's members without reflection. Bindings
cannot produce the member *pointers* schemas need as template arguments, so this is not a schema
front end — but it can count members, which enables a weak completeness warning for aggregates
(member count vs distinct members referenced by the schema) as a bridge before P2996 ships
everywhere. Marginal; implement only if the reflection timeline slips badly.

## Modules and `import std`

Not a design change — a distribution question. The single self-contained header remains the
canonical form (game engine build systems consume it with zero friction, and the
include-with-no-prerequisites property is tested). A named module wrapper can ship *alongside*
the header when module support in the matrix stops being the least reliable feature in every
toolchain; nothing in the library's design blocks it, and nothing in it is urgent.

## Summary

| feature | paper | what it changes here | posture |
|---|---|---|---|
| static reflection | P2996 | missing-member check, `auto_schema`, named diagnostics, generated docs | prototype behind feature test; adopt at floor bump |
| annotations | P3394 | wire metadata on members, enabling `auto_schema` | with reflection |
| generated assert messages | P2741 | constraint errors name members and values | adopt at floor bump |
| expansion statements | P1306 | runner rewrite: lifts field ceiling, cuts compile time | prototype early; gates make it safe |
| pack indexing | P2662 | TMP internals cleanup | during P1306 rewrite |
| contracts | P2900 | write-path asserts become consumer-profiled contracts | annotate; never on the read path; measure |
| inplace_vector | P0843 | `vector_n` field, wire-equivalent to `array_n` | on user demand |
| erroneous behavior | P2795 | uninitialized-member bugs become diagnosable | no change; note for fuzzing |
| constexpr cmath | P1383 | consteval quantization tests | testing only |
| std::simd | P1928 | nothing, per the performance record | expected null; measure if proposed |
| binding packs | P1061 | weak aggregate completeness warning | only if reflection slips |
| modules | — | optional distribution wrapper | header stays canonical |

The through-line: everything valuable on the horizon makes the schema language *safer to hold*
or *cheaper to compile* — none of it changes a byte on the wire, and none of it will be allowed
to.
