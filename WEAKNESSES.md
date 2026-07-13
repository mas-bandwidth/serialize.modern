# Known weaknesses

*Last revised July 2026. This is the maintained, honest list of what is weak about this library
today — ranked by how much it should worry you, with what mitigates each item now and what fixes
it later. Companion documents: [SCHEMA.md](SCHEMA.md) for the schema language,
[FUTURE.md](FUTURE.md) for the upcoming-C++ roadmap.*

## 1. No external validation

Everything in this library — including its safety net of tests, gates and adversarial review —
was built and reviewed by the same small set of eyes. The schema language has never been used by
someone who didn't design it, and API design flaws are exactly the class of defect an author
cannot see. This is the dominant weakness and the reason the library is marked pre-release:
nothing but real users fixes it. If you are one, feedback is genuinely wanted — especially on the
schema language design ([open an issue](https://github.com/mas-bandwidth/serialize.modern/issues)).

## 2. Schemas cannot detect an omitted member

Add a member to a struct, forget to add a field for it, and the schema compiles while the member
silently never serializes. The schema constraints catch wrong types, wrong sizes, duplicate
writes, forward references and cross-type field lists — omission is the one structural bug class
left open, because enumerating a struct's members is impossible in C++23. C++26 reflection closes
it with a completeness check (the first item in [FUTURE.md](FUTURE.md)); until then, schemas share
this failure mode with hand-written serialize functions. Mitigation today: the exactness of
`MeasureBits` makes a missing field show up as a size discrepancy in any test that compares
against a known wire size.

## 3. Error cascades after the first diagnostic line

Schema misuse is reported by named static_assert messages that state the mistake and the fix —
including deep cases: wrong object types inside branch sides and match cases, wrong inner types
in `object`/`array`/`array_n`, bare fields where a `fields<...>` list is needed, non-fields in a
schema, non-member-pointers in a field (an eight-case misuse matrix pins this, and composite
fields validate their own children, so the property holds at any nesting depth by induction).
What remains: after the named first line, compilers usually append the underlying instantiation
trace anyway — the first line is always the answer, but it is not always alone. Fully
suppressing the cascade would require conditional-instantiation surgery that is not worth its
complexity. And misuse patterns outside the matrix may still produce raw traces; report them.

## 4. Fuzzing compile time specialized code has a ceiling

The streams get true coverage-guided fuzzing. Schemas cannot: a fuzzer mutates data at runtime,
but schema *shapes* are compile time types, so hostile coverage is bounded by the instantiations
someone thought to write (the fuzz harness schemas, plus a reject-parity test that flips every
bit of a composed packet against a stream twin). Those are rich but fixed. A schema shape nobody
instantiated in a test has never been fuzzed and never will be until someone writes it down.

## 5. Performance numbers are one machine deep

Every published number — the ~45% stream write win over classic, the schema speedups, the
bitpacker parity — was measured on a single Apple M-series machine. The codegen audit *proves*
GCC, clang and MSVC all generate call-free straight-line schema code on x86-64 and arm64, but
proving clean codegen is not the same as benchmarking it: no trustworthy x86 desktop numbers
exist (CI runners are shared and noisy — trends only). Related: the `small_copy` threshold
(`SmallCopyMaxBytes = 64`) is tuned against exactly one workload, the benchmark's 17-byte blobs.

## 6. The buffer contract is unverifiable

Allocations must extend at least 8 bytes past the end of the data, for both reading and writing.
Violate it and nothing fails at compile time or in a release run — the failure surfaces under
ASan or as memory corruption in production. This is one rule where classic had two, it is
documented everywhere, and a canary test pins that the writer touches nothing beyond the slack —
but no language feature on any horizon can check an allocation's extent through a pointer
([FUTURE.md](FUTURE.md), contracts section). Expect this to be the first crash every new user
has.

## 7. Compile time in schema-heavy translation units

A translation unit with an ordinary fixed-layout schema compiles as fast as the equivalent stream
code (measured at parity, ~0.07s). But the constraint machinery and specialization are not free
at scale: the library's own 30-test suite TU takes ~6.4s, forking constructs multiply compile
time along with code size (two chained maximal `array_n` ≈ 2s; three ≈ 72s — measured, see
SCHEMA.md), and flat schemas hit default constexpr depth limits around 500 fields. Nobody has
measured a real game's worth of packet schemas (say, 200 types) in one TU. C++26 expansion
statements should cut the constant factors and lift the depth ceiling ([FUTURE.md](FUTURE.md)).

## 8. Deliberate limits you will feel anyway

Documented design decisions, not defects — listed because a newcomer hits them as if they were
weaknesses:

* **Bounded collections only.** `array_n` count ranges are capped at 16; unbounded collections
  and wide count ranges stay on the streams. The zero-overhead strategy is specialization, and
  specialization is paid for in code size.
* **`wstring_` is the single schema/stream wire asymmetry.** It aligns before its code points;
  `serialize_wstring` does not. Documented in SCHEMA.md; still an exception you must know.
* **No protocol versioning primitives.** Evolving a live protocol is a hand pattern — `const_`
  version field, `reserved_` bits, `branch_on` gates — not a feature. This is the most likely
  next addition if users ask for it.
* **Back references must be unconditional.** `branch_on`/`match`/`int_relative_` can only
  reference members serialized unconditionally earlier. Decisions on conditionally-present
  members are inexpressible by design (the validator would otherwise be unsound).

## What is *not* on this list

Unchecked release-mode writes. That is the trust model, not a weakness, and the failure-mode
analysis is recorded in the repo: clamping would write silently wrong data that passes read
validation; returning false at the write site is unactionable and turns value bugs into phantom
packet drops; fatal errors would crash shipped games over a value the peer would simply reject.
The reader guards the trust boundary and validates everything; the writer sits inside it, and its
correctness is the program's responsibility — supported by debug asserts and the schema's compile
time constraints, both free at runtime. The same reasoning keeps the classic `serialize_*` macros
byte-for-byte compatible rather than hardened: code that compiles against classic must compile
here.
