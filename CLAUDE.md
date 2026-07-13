# CLAUDE.md

## What this is

**Status: pre-release and experimental.** The wire format is pinned, but the
API — especially the schema language — is young and evolving, and the owner
explicitly does not want it presented as mature or production ready (classic
serialize is the production choice). User feedback on the schema language
design is actively solicited via GitHub issues.

**serialize.modern** is the modern C++ (C++23) port of classic
[serialize](https://github.com/mas-bandwidth/serialize), a single-header C++
bitpacking serializer (~4,000 lines of library code in
[serialize.h](serialize.h), plus ~2,200 lines of embedded tests) aimed at game
networking. The wire protocol is **byte-identical to classic serialize** —
this is the repo's core invariant and is enforced two ways: the golden wire
format test pins the exact bytes (same golden bytes as classic), and the
`wire-compat` CI job builds [wire_compat.cpp](wire_compat.cpp) against both
libraries (classic pinned at v1.4.3), requires the two corpus streams to be
byte-identical, and cross-reads each stream with the other library. Never
change the wire format; if the golden test or the wire gate fails, the change
is wrong.

Header-only is intentional (heavily templated serialize methods), and the
header is self-contained: including it into a translation unit with no prior
includes must compile. Requires C++23 (`<bit>`, `std::bit_cast`,
`std::byteswap`, `std::endian`, `if constexpr` macros). The API is classic's
(same classes, same serialize_* / read_* / write_* macros — the macro
families were briefly removed during development and restored by owner
decision: classic API compatibility matters more than trimming surface;
their internal temporaries use the hygienic serialize_temp_* names), plus
`WriteBits64`/`ReadBits64`/`SerializeBits64` for [1,64] bit values, plus
compile time schemas (`serialize::schema`): fixed-layout packets described as
types, with constant-offset generated code, compile time if/else
(`branch`) and serialize_object-style composition (`object`).
Schema wire output is byte-identical to the equivalent serialize methods,
pinned by the schema tests.

The schema language reference is [SCHEMA.md](SCHEMA.md), linked from the
README; keep it in sync when fields or semantics change.

Build: `cmake -B build && cmake --build build --config Release`, test with
`ctest --test-dir build --build-config Release`. Tests live in serialize.h
behind `SERIALIZE_ENABLE_TESTS`. CI (.github/workflows/ci.yml): Debug +
Release on Linux/macOS/Windows, the wire-compat gate on all three platforms,
ASan+UBSan and libFuzzer jobs on Linux, a big-endian s390x job under QEMU,
and a codegen audit (Release, all three compilers: GCC and clang via
nm+objdump, MSVC via dumpbin /DISASM; s390x is excluded on purpose — that
job proves wire correctness, not codegen): codegen_audit.py disassembles
the schema Read/Write/MeasureBits functions and fails the build on call instructions,
loops, indirect branches or instruction count blowups — the zero-overhead
schema property is a gated invariant, not an aspiration. Its self test runs
the same rules against deliberately bad code, so the gate provably can fail. To run the wire gate locally:
`cmake -B build -DSERIALIZE_WIRE_COMPAT_CLASSIC_DIR=<classic checkout> . && ctest --test-dir build -R wire_compat`.

## Differences from classic (owner-approved, July 2026)

- **Allocation contract**: one rule for both sides — buffer allocations must
  extend at least 8 bytes past the end of the data (writer flushes whole
  qwords; reader loads 64 bit windows at byte granularity). In exchange, the
  classic multiple-of-8 write buffer size rule is gone: any size works.
- **FlushBits is still required** after writing, exactly as classic. (A
  branchless writer that made flush a no-op was built and rejected on
  measurement; see the performance record.)
- Single puts/loads move up to 56 bits (`MaxWriteBits`/`MaxReadBits`), so
  `serialize_int64` ranges that fit 56 bits cost one operation instead of
  two. Wire-identical: the stream is LSB-first, so any split produces the
  same bytes. 56 (not 57) because a spilling put must keep its carry shift
  below 64 — 57 was an undefined-behavior bug caught during development.
- Version 0.1.0 (`SERIALIZE_VERSION`), matching CMake: a 0.x version line,
  because the library is pre-release and the schema language is still
  evolving (owner directive: the first release tag is v0.1.0, not v1.0.0).
  serialize.modern does not continue classic's 1.4.x numbering — the two
  version lines are independent (wire_compat output prints both, e.g.
  classic 1.4.3 vs modern 0.1.0).

## Honest assessment

### Verified state (July 2026)

- All 30 tests pass in Debug and Release, clean under ASan+UBSan, on Apple
  Silicon (Apple clang 21). CI is green on every job: Debug/Release on
  Linux (GCC), macOS (Apple clang) and Windows (MSVC), the wire-compat gate
  on all three platforms, ASan+UBSan, libFuzzer, and big-endian s390x under
  QEMU. (One first-run fix: GCC 13 reports __cplusplus as 202100L in C++23
  mode, so the version guard accepts the partial-support value.)
- The wire-compat gate passes locally against classic v1.4.3: byte-identical
  65 KB corpus (every primitive, widths straddling every internal split
  point) and both cross-reads. A negative test (one flipped bit) fails both
  readers, so the gate can actually fail.
- The golden wire format test passes with classic's exact golden bytes.
- The fuzz harness covers the streams AND the schema path (hostile schema
  reads with invariant checks plus a schema-vs-stream differential that
  requires byte-identical wire). libFuzzer cannot run locally (Apple clang
  ships none), but a 300k-iteration random driver under ASan+UBSan runs
  clean locally; CI runs the real fuzzer 60s per push and nightly.

### Performance engineering record

Every design decision below was measured on Apple Silicon (interleaved
best-of-5 runs, heap-buffered benchmark). Kept:

- **Classic 64 bit scratch, qword flush writer.** A fully branchless
  store-per-write writer was built first and lost everywhere: 3,400 vs 6,000
  MB/s on the fixed-pattern benchmark, 12% behind on random widths — its
  serial OR+shift scratch chain costs more than the well-predicted flush
  branch. The flush writer was restored, generalized to 56 bit puts.
- **`serialize_force_inline` on the bitpack hot core** (PutBits/GetBits,
  WriteBits*/ReadBits*, stream SerializeInteger*/SerializeBits*): the 64 bit
  paths exceed inline budgets, and an out-of-line call mid-decode spills
  state — worth ~20% stream read and ~16% stream write.
- **`small_copy`**: inline overlapping-chunk copies for byte blocks ≤ 64
  bytes (`SmallCopyMaxBytes`), bypassing the libc memcpy call: +20% stream
  write. The threshold is tuned only against 17-byte benchmark blobs.
- **Compile time schemas.** Constant-offset generated code beats the stream
  path by ~3x read / ~7x write on the benchmark packet (450 M packets/s
  both directions), and 3.6x read with write parity on a small float-only
  packet with an unpredictable 50/50 branch. The runtime bit cursor is the
  stream path's fundamental cost: schemas delete it. Wire-identical output
  is pinned by test_schema/test_schema_object/test_schema_array/
  test_schema_dynamic (memcmp against macro twins, cross-reads both
  directions), and the zero-overhead codegen is pinned by the codegen_audit
  CI gate (straight-line call-free code, e.g. the audit packet reads in 57
  instructions and writes in 33 on arm64 clang).
- **Protocol plumbing fields**: const_ (magic/version, read rejects
  mismatch), reserved_ (zeros, read rejects nonzero), enum_ (range
  validated), compressed_float_ (quantization constants folded at compile
  time), wstring_ (wire adds an align before the 32 bit code points, unlike
  serialize_wstring — documented), int_relative_ (wire identical to
  serialize_int_relative; forks the layout tree seven ways). Schema
  MeasureBits/MeasureBytes give exact sizes, unlike the conservative
  MeasureStream. Dynamic and counted fields compose through object and
  fixed arrays (a string inside an array element just hands the remaining
  elements to a runtime base).
- **Back references (branch_on, match/case_)**: decisions from members
  serialized earlier in the schema, zero wire bits, layout tree still forks
  at compile time. Forward references are compile errors: schema<>
  static_asserts valid_references<>, which walks the flattened schema
  accumulating unconditionally-serialized member paths and requires every
  back reference to hit one (members written inside one branch side or
  behind an array count don't qualify). Enforced by every compiler in the
  matrix at every optimization level, and pinned by positive and negative
  static_asserts in test_schema_backref.
- **Loops and variable length in schemas, three strategies, all measured.**
  array/bits_array unroll fixed counts at compile time. array_n (runtime
  count in [min,max], range capped at 16) generates one fully constant path
  per possible count, wire encoded relative to the minimum:
  pure specialization, no runtime cursor. string/bytes_n hand the rest of
  the schema off to a runtime byte base: shifts and masks stay constants,
  only the base pointer is a register, the length-bounded copy is the only
  loop. A packet mixing all three reads at 240 M packets/s vs 61 M on the
  stream path (3.9x). The codegen audit gates these under a 'dynamic'
  profile: loops/copy calls allowed, instruction budgets still enforced,
  fixed-layout schemas stay strict.

Rejected, with measurements — do not re-propose without new evidence:

- `[[likely]]`/`[[unlikely]]` on stream validation branches: **cost** 10% of
  stream read throughput.
- `[[assume]]`/`__builtin_assume` bit-range hints: zero effect, UB risk.
- 128 bit (`__int128`) scratch writer, flushing every 128 bits: exact parity
  — wider ALU ops cancel the halved flush rate (and MSVC has no `__int128`).
  The 32→64 scratch widening was worth +25% historically; 64→128 is worth 0.
  The scratch sweet spot is the native register width.
- Force-inlining the serialize_bytes/align path: parity.
- Grouped-field decode (fusing adjacent small fields into one
  `SerializeBits64`, cutting window loads from ~12 to ~7 per packet): +3%,
  within noise. L1 window loads are nearly free; not worth contorting
  serialize code.
- PGO (Apple clang, instrumented profile of the benchmark itself): −15%
  stream write. Measure before trusting PGO on this kind of code.

Second red/blue round (July 2026, after the schema language landed):

- **Accepted: member type constraints on schema fields.** Before: a float
  member in a `bits` field *compiled* and silently truncated (3.7 wrote as
  3) — member pointers had erased the type check the stream macros do
  naturally. Every field now static_asserts its member type with a message
  naming the correct field; the predicates (`unsigned_bits_member`,
  `integer_member`) are pinned both-polarities in the tests. Zero runtime
  cost: codegen audit and wire gate unchanged. Note `int_relative_` accepts
  any non-bool integral (classic's macro does), and `bits_array`/`object`
  have no `kind` member — they are expanded by flatten, so their asserts
  live directly in the struct body.
- **Measured: compile time.** A typical 7-field packet TU: schema 0.07s vs
  stream method 0.08s — parity for consumers. The full 28-test suite TU is
  ~3.9s: the code-size multipliers (branch x2, match x(cases+1), array_n
  x range, relative x7) govern compile time too. Documented in SCHEMA.md.
- **Rejected without new experiments** (by record or by the gates'
  definition): C++26 reflection to auto-derive schemas (not shipped in any
  targeted toolchain; the natural future front end — revisit when GCC,
  clang and MSVC all ship P2996); constexpr/consteval Read/Write
  (compile-time round trips duplicate the get/put paths under `if
  consteval` for zero runtime asm change; golden bytes already CI-pinned);
  `[[likely]]` on schema validation (streams measured -10%); shared aligned
  window loads (the grouped-decode +3%-in-noise result); binary dispatch
  trees for match/array_n selection (ranges are capped small, forward
  compares predict well); coroutine/incremental decode and std::expected
  returns (runtime frames, indirect calls, error taxonomies — the things
  the codegen gate exists to forbid); DSL/UDL sugar and modules (no asm
  change). The one real gap inside the std::expected proposal — "how many
  bytes did Read consume?" — was already answered exactly by MeasureBits on
  the decoded object, now documented in SCHEMA.md.

Third red/blue round (July 2026, capacity and the compile-time cliff):

- **Accepted: capacity constraints on schema fields.** Round two checked
  the member's type but not its size: `int_<&P::i8, -1000, 1000>` compiled,
  and the read VALIDATED the legal wire value 1000, then assigned -24 to
  the int8_t member — silent corruption that passes validation (classic
  cannot catch this; its macros assign through an int32 temp). Now
  compile errors: `bits`/`bits_array` wider than the member/element type,
  `int_`/`int64` ranges that do not fit the member, `enum_` MaxValue
  above the underlying type, `array_n`/`bytes_n` counts above the count
  member's capacity. Helpers `member_bits<T>`/`range_fits<T>(min,max)`
  (enums resolve to their underlying type; no <limits> dependency), pinned
  both-polarities in the tests. Zero runtime cost; wire and codegen gates
  unchanged. Placement note: `bytes_n`'s assert sits below its `max_length`
  declaration, and `string`/`wstring_`/`bytes_n` all have textually
  identical `length_bits` lines — anchor edits on DataMember.
- **Measured: the compile-time cliff.** Chained maximal `array_n` (17
  paths each): 17 paths = 0.2s / 4 KB text; 289 = 2.1s / 60 KB; 4,913 =
  72.5s / 1.1 MB. Linear-ish per path to ~1k paths, superlinear beyond.
  Two chained large forks is the practical ceiling; documented with
  numbers in SCHEMA.md.
- **Rejected**: a static_assert cap on total path count (the threshold is
  user policy, and a true count requires recursive estimation through
  array_n inners — machinery for a judgment the multiplier table already
  teaches); clamping out-of-range release writes (the trust model, by
  design); std::span overloads (API surface, no asm change); naming the
  offending member in constraint messages (needs C++26 user-generated
  static_assert messages — revisit with reflection).

Fourth red/blue round (July 2026, structural coherence):

- **Accepted: duplicate write detection.** The copy-paste bug compiled:
  `float_<&Vec::x>, float_<&Vec::y>, float_<&Vec::y>` — y serialized
  twice, z never, read validates fine and z silently never round-trips.
  Only the schema can catch this (streams cannot see repetition). A
  `unique` flag now threads through the existing ref_check walk (same
  instantiations as valid_references, near-zero compile cost): each
  field's writes must be disjoint from the accumulated Seen list.
  Exclusive branch/match sides restart from the same Seen, so both sides
  writing one member is allowed — pinned positive and negative by
  static_asserts on `serialize::unique_writes` (public trait) and
  schema<> static_asserts it. Limitation: the check is path-identity
  based — overlapping but differently shaped accesses (bytes over a whole
  array vs its expanded elements) do not collide.
- **Accepted: object type coherence.** Mixing members of two structs in
  one schema failed before, but with "no matching function for call to
  'write'" plus 17 lines of template context. schema<> now static_asserts
  same_object_type over the flattened list: a named one-line error
  ("nest inner objects with object<>").
- **Red claim killed by experiment**: "int_relative_'s back reference is
  never validated — no ref_step specialization exists." False: the
  specialization lives next to relative_dispatch in the runner section
  (line ~3413), far from the other ref_steps — easy to miss reading
  top-down, but `valid_references` over a dangling prev already returns
  false. Do not re-claim without a failing test; the machinery walk is
  split across the file by dependency order, not by topic.

Fifth red/blue round (July 2026, runtime contracts and gate blind spots):

- **Accepted: MeasureBits joined the codegen audit.** It was ungated — it
  could have regressed to a runtime schema walk and no gate would notice.
  Measured first: fixed-layout measure folds to `mov #const; ret` (2
  instructions), the branchy body packet to 6, the dynamic packet to 19
  including a strlen for the string field. Budgets 24/24/150, dynamic
  profile for the strlen.
- **Accepted: rejection parity pinned.** SCHEMA.md claimed schema and
  stream twins "reject the same malformed inputs" — true but untested
  (the fuzz differential cross-reads valid data). test_schema_reject_parity
  now flips every bit of a valid composed packet (arrays, branch, match,
  int_relative_, array_n) and requires identical verdicts from both
  readers, and identical decoded packets (compared by re-encoding) where
  both accept. Parity held on the first run; now it cannot silently break.
- **Rejected: hardening the serialize_* macros with the schema's type
  checks.** serialize_bits on a float member compiles and truncates — in
  classic AND modern, byte-for-byte the same behavior. Rejecting it here
  would make modern refuse code classic accepts, re-breaking the
  compatibility the macro restoration exists to provide. Deliberate
  two-tier design: the macros are classic's surface verbatim; static
  safety lives in the schema. Do not re-propose macro static_asserts.

Extended red/blue campaign, rounds 6-19 (July 2026). The owner asked for
rounds until diminishing returns was definitely reached, plus ten more.
The point was declared after round 9 (rounds 8 and 9 produced no library
defect); rounds 10-19 were the mandated overtime. Compact record:

- R6 (accepted): bench.cpp had NO schema benchmark and example.cpp no
  schema example — the flagship numbers rested on scratch harnesses.
  Both added; bench verifies wire identity vs the stream path before
  timing and reproduces 524 M pkt/s write (7.8x) / 426 M read (3.5x);
  README table updated. Re-baseline: five constraint rounds cost zero
  runtime; consumer TU compile unchanged (0.07s); suite TU 3.9->6.4s.
- R7 (accepted): CMake interface include marked SYSTEM — consumers with
  -Wall -Wextra -Wshadow -Wconversion -Wpedantic -Wold-style-cast now see
  ZERO header warnings (was 101, all benign). ODR-safe, self-contained.
  The unprefixed read_*/write_* macro names are classic's API: kept.
- R8: flat-width ceiling ~500 fields (clang default -fconstexpr-depth
  512; documented with workaround). No SCHEMA.md vocabulary drift. No
  dead helpers.
- R9: unaligned bases (+0..+7), all truncations, slack canary — all
  hold; slack contract pinned as test 30. No defect: DIMINISHING RETURNS
  DECLARED HERE.
- R10-18, the overtime, all nulls: float specials bit-exact,
  compressed_float_(NaN) clamps safely; 48-combo boundary matrix exact;
  TSan-clean 16 threads x 10k ops; -fno-exceptions/-fno-rtti and
  -std=c++2c clean (suite included); 300k fuzz iterations clean in 1.6s;
  measure hierarchy (exact == stream bits <= conservative) over 20k
  variants; high-byte/max-length strings and supplementary-plane
  wstring_ exact; object<> nesting to depth 64 compiles (3.7s) and round
  trips; the schema object's ONLY undefined symbol is strlen — no
  allocation, no libc++, nothing else. Two harness bugs (out-of-range
  write in the slack test, wrong size expectation in the TSan test) and
  zero library bugs: by round 12 the exercise was finding bugs in the
  tester. That is what diminishing returns looks like; the record exists
  so nobody has to rediscover it.

Benchmark epistemology, learned the hard way:

- The classic fixed-width bitpacker benchmark flatters whichever design the
  compiler can constant-fold: classic write drops from 6,006 to 3,935 MB/s
  with a random width sequence. bench.cpp now carries both patterns.
- Stream numbers at ~85 cycles/packet resolve **code/data placement luck**:
  with instruction-identical decode functions, stream read swung between 107
  and 144 M packets/s across 8-byte layout perturbations (member padding,
  stack offsets, function alignment). The packet buffers were moved to the
  heap in bench.cpp to reduce this; residual modern-vs-classic stream read
  deltas are placement artifacts, not code quality (the decode disassembly
  is identical and was verified instruction by instruction).

### Known limits (documented, by design — inherited from classic)

- **The trust model**: debug asserts verify correctness; in release the
  write path is unchecked (pre-measure with MeasureStream or size
  conservatively). The read path validates at runtime in release and drops
  invalid data, because network input is the trust boundary. Do not propose
  hardened/checked write modes.
- **The serialize macros hide `return false` on purpose** — invalid packet
  data must abort the whole serialize function immediately. Serialize
  functions are `template <typename Stream>` returning bool. Do not propose
  exceptions or error-code redesigns.
- `serialize_int_relative` requires strictly increasing values.
- `wstring` wire format is 32 bits per character.
- `MeasureStream` is conservative: every align counts as 7 bits.

### Bottom line

A faithful modern port that is measurably faster where it matters (stream
writes ~45% over classic; bitpacker and reads at parity with identical
decode instruction streams) and simpler to use (one allocation rule, any
buffer size, readable concept errors, constexpr bit math). The wire format
is pinned by a golden test and a cross-library CI gate against real classic
binaries. The performance record above is the repo's institutional memory:
most "obvious" modern optimizations were measured and rejected — check it
before proposing perf changes.

### Open items

- `SmallCopyMaxBytes` (64) is tuned against a single workload.
- Performance numbers are from one machine (Apple M-series). The CI
  benchmark step provides x86 numbers but shared runners are noisy —
  compare trends only.
- Schemas cover fixed layouts, conditionals and composition; variable-length
  structure (runtime array counts, strings) stays on the streams. An
  array_field/switch_field extension is possible if needed.
