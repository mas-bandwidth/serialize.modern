/*
    serialize.modern codegen audit

    Copyright © 2016 - 2026, Más Bandwidth LLC.

    Redistribution and use in source and binary forms, with or without modification, are permitted provided that the following conditions are met:

        1. Redistributions of source code must retain the above copyright notice, this list of conditions and the following disclaimer.

        2. Redistributions in binary form must reproduce the above copyright notice, this list of conditions and the following disclaimer
           in the documentation and/or other materials provided with the distribution.

        3. Neither the name of the copyright holder nor the names of its contributors may be used to endorse or promote products derived
           from this software without specific prior written permission.

    THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES,
    INCLUDING, BUT NOT LIMITED TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
    DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL,
    SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR
    SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY,
    WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE
    USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/*
    Compile time schemas promise zero runtime overhead: every offset, shift, mask, byte count and
    copy length folds to a compile time constant, and the generated Read/Write is straight-line,
    call-free code whose only runtime work is moving the data and validating untrusted input.

    This translation unit pins that promise so CI can enforce it from PR to PR. It instantiates
    representative schemas — a fixed-layout packet exercising every field kind, and a branchy,
    composed packet — behind extern "C" wrappers with unmangled names. codegen_audit.py then
    disassembles this object file and fails if any audited function contains a call instruction
    (inlining broke), a backward branch (a loop appeared: the runtime bit cursor or a byte copy
    loop came back), or more instructions than a generous per-function budget (a wholesale codegen
    regression). Run by ctest as `codegen_audit` in Release builds; see codegen_audit.py.

    The static_asserts below additionally pin the compile time layout math itself: if a change
    shifts any field offset, the wire format changed and the build fails before a single test runs.
*/

#include <serialize.h>

// a fixed layout packet exercising every schema field kind
struct AuditPacket
{
    int32_t a;              // ranged: [-100,+100], 8 bits
    int32_t full;           // full range: the range check must not exist (vacuous by construction)
    uint32_t bits7;
    uint32_t bits13;
    uint32_t bits23;
    bool flag;
    float x, y;
    double d;
    int64_t range34;
    uint8_t blob[17];
    uint32_t tail;
};

using AuditSchema = serialize::schema<
    serialize::int_<&AuditPacket::a, -100, +100>,
    serialize::int_<&AuditPacket::full, INT32_MIN, INT32_MAX>,
    serialize::bits<&AuditPacket::bits7, 7>,
    serialize::bits<&AuditPacket::bits13, 13>,
    serialize::bits<&AuditPacket::bits23, 23>,
    serialize::bool_<&AuditPacket::flag>,
    serialize::float_<&AuditPacket::x>,
    serialize::float_<&AuditPacket::y>,
    serialize::double_<&AuditPacket::d>,
    serialize::int64<&AuditPacket::range34, -5000000000LL, +5000000000LL>,
    serialize::bytes<&AuditPacket::blob, 17>,
    serialize::bits<&AuditPacket::tail, 32> >;

// the layout is part of the wire format: pin it at compile time
static_assert( AuditSchema::MaxBits == 8 + 32 + 7 + 13 + 23 + 1 + 32 + 32 + 64 + 34 + 2 + 17 * 8 + 32 );
static_assert( AuditSchema::MaxBytes == 52 );

// a branchy, composed packet: object splicing and compile time if/else
struct AuditVec { float x, y, z; };

struct AuditBody { AuditVec position; bool atRest; AuditVec velocity; };

using AuditVecSchema = serialize::schema<
    serialize::float_<&AuditVec::x>,
    serialize::float_<&AuditVec::y>,
    serialize::float_<&AuditVec::z> >;

using AuditBodySchema = serialize::schema<
    serialize::object<&AuditBody::position, AuditVecSchema>,
    serialize::branch<&AuditBody::atRest,
        serialize::fields<>,
        serialize::fields< serialize::object<&AuditBody::velocity, AuditVecSchema> > > >;

static_assert( AuditBodySchema::MaxBits == 96 + 1 + 96 );

// a packet with runtime-length sections. these audit under the script's 'dynamic' profile: the
// runtime-length copies are allowed to loop (and strlen to be called), but the functions must stay
// within an instruction budget and may not use indirect branches — and everything around the
// dynamic sections still folds to constants.
struct AuditDyn
{
    uint32_t head;
    char name[16];
    int32_t mid;
    uint8_t blob[8];
    int32_t blob_count;
    AuditVec rings[3];
    int32_t ring_count;
    uint32_t tail;
};

using AuditDynSchema = serialize::schema<
    serialize::bits<&AuditDyn::head, 11>,
    serialize::string<&AuditDyn::name>,
    serialize::int_<&AuditDyn::mid, -100, +100>,
    serialize::bytes_n<&AuditDyn::blob, &AuditDyn::blob_count>,
    serialize::array_n<&AuditDyn::rings, &AuditDyn::ring_count, AuditVecSchema>,
    serialize::bits<&AuditDyn::tail, 32> >;

// the audited functions. extern "C" so the audit script finds them by unmangled name.

extern "C" bool audit_fixed_read( const uint8_t * data, int64_t bytes, AuditPacket & packet )
{
    return AuditSchema::Read( data, bytes, packet );
}

extern "C" int64_t audit_fixed_write( uint8_t * data, const AuditPacket & packet )
{
    return AuditSchema::Write( data, AuditSchema::MaxBytes, packet );
}

extern "C" bool audit_body_read( const uint8_t * data, int64_t bytes, AuditBody & body )
{
    return AuditBodySchema::Read( data, bytes, body );
}

extern "C" int64_t audit_body_write( uint8_t * data, const AuditBody & body )
{
    return AuditBodySchema::Write( data, AuditBodySchema::MaxBytes, body );
}

extern "C" bool audit_dynamic_read( const uint8_t * data, int64_t bytes, AuditDyn & packet )
{
    return AuditDynSchema::Read( data, bytes, packet );
}

extern "C" int64_t audit_dynamic_write( uint8_t * data, const AuditDyn & packet )
{
    return AuditDynSchema::Write( data, AuditDynSchema::MaxBytes, packet );
}
