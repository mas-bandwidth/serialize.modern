/*
    serialize

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
    libFuzzer harness for serialize.

    Every input runs two passes:

    1. Hostile read (FuzzRead). ReadStream is the trust boundary of this library: it must survive
       arbitrary hostile bytes, failing reads by returning false, never by corrupting memory or
       tripping an assert. Build with asserts enabled (Debug config) plus ASan/UBSan so all three
       failure modes are caught.

    2. Differential round trip (FuzzRoundTrip). Values generated from the input are written with
       WriteStream, read back with ReadStream, and compared. Any write/read asymmetry traps.
       MeasureStream runs the same ops and must never measure fewer bits than were written.

    The first bytes of the fuzz input are an op program selecting which serialize_* calls run and
    with what parameters. The remaining bytes are the hostile bitstream for pass 1 and the value
    pool for pass 2. This lets coverage-guided fuzzing explore interleavings of every primitive.
*/

#include "serialize.h"

#include <vector>

#define fuzz_check( condition ) do { if ( !(condition) ) { __builtin_trap(); } } while (0)

// hands out bytes from the fuzz input, wrapping around when exhausted. both round trip passes
// regenerate the identical value sequence, so nothing needs to be stored between passes.

struct ValuePool
{
    const uint8_t * data;
    size_t size;
    size_t index;

    ValuePool( const uint8_t * data_in, size_t size_in ) : data( data_in ), size( size_in ), index( 0 ) {}

    uint8_t NextByte()
    {
        if ( size == 0 )
            return 0;
        if ( index >= size )
            index = 0;
        return data[index++];
    }

    uint32_t NextUint32()
    {
        uint32_t value = 0;
        for ( int i = 0; i < 4; i++ )
            value = ( value << 8 ) | NextByte();
        return value;
    }

    uint64_t NextUint64()
    {
        return ( uint64_t( NextUint32() ) << 32 ) | NextUint32();
    }
};

// pass 1: hostile read. arbitrary bytes go in, reads must fail cleanly or produce in-contract values.

template <typename Stream> bool FuzzRead( Stream & stream, const uint8_t * ops, int numOps )
{
    uint8_t bytes[256];
    char string[256];
    wchar_t wstring[64];

    for ( int i = 0; i < numOps; i++ )
    {
        const int select = ops[i] & 15;
        const int param = ops[i] >> 4;              // [0,15]

        switch ( select )
        {
            case 0:
            {
                uint32_t value = 0;
                const int bits = param + 1;         // [1,16]
                serialize_bits( stream, value, bits );
                fuzz_check( value < ( 1U << bits ) );
            }
            break;

            case 1:
            {
                uint32_t value = 0;
                const int bits = param + 17;        // [17,32]
                serialize_bits( stream, value, bits );
                fuzz_check( bits == 32 || value < ( 1U << bits ) );
            }
            break;

            case 2:
            {
                int32_t value = 0;
                serialize_int( stream, value, -100, +100 );
                fuzz_check( value >= -100 && value <= +100 );
            }
            break;

            case 3:
            {
                int32_t value = 0;
                serialize_int( stream, value, INT32_MIN, INT32_MAX );
                int64_t value64 = 0;
                serialize_int64( stream, value64, INT64_MIN, INT64_MAX );
                const int64_t bound = int64_t( param + 1 ) << 35;               // spans of varying width exercise bits_required64
                int64_t ranged64 = 0;
                serialize_int64( stream, ranged64, -bound, +bound );
                fuzz_check( ranged64 >= -bound && ranged64 <= +bound );
            }
            break;

            case 4:
            {
                bool value = false;
                serialize_bool( stream, value );
            }
            break;

            case 5:
            {
                float value = 0.0f;
                serialize_float( stream, value );
            }
            break;

            case 6:
            {
                double value = 0.0;
                serialize_double( stream, value );
            }
            break;

            case 7:
            {
                float value = 0.0f;
                serialize_compressed_float( stream, value, -10.0f, +10.0f, 0.01f );
            }
            break;

            case 8:
            {
                uint64_t value = 0;
                serialize_uint64( stream, value );
            }
            break;

            case 9:
            {
                const int numBytes = param * 16 + 1;                    // [1,241]
                serialize_bytes( stream, bytes, numBytes );
            }
            break;

            case 10:
            {
                serialize_string( stream, string, (int) sizeof( string ) );
                fuzz_check( strlen( string ) < sizeof( string ) );
            }
            break;

            case 11:
            {
                serialize_wstring( stream, wstring, (int) ( sizeof( wstring ) / sizeof( wchar_t ) ) );
                fuzz_check( wcslen( wstring ) < sizeof( wstring ) / sizeof( wchar_t ) );
            }
            break;

            case 12:
            {
                serialize_align( stream );
            }
            break;

            case 13:
            {
                int previous = param * 1000 - 8000;
                int current = 0;
                serialize_int_relative( stream, previous, current );
            }
            break;

            case 14:
            {
                const int32_t max = ( param + 1 ) * 1000;               // ranges of varying width exercise bits_required
                int32_t value = 0;
                serialize_int( stream, value, 0, max );
                fuzz_check( value >= 0 && value <= max );
            }
            break;

            case 15:
            {
                uint8_t value8 = 0;
                serialize_uint8( stream, value8 );
                uint16_t value16 = 0;
                serialize_uint16( stream, value16 );
                uint32_t value32 = 0;
                serialize_uint32( stream, value32 );
            }
            break;
        }
    }

    return true;
}

// pass 2: differential round trip. run once with WriteStream (and MeasureStream), then again with
// ReadStream over the bytes just written. every case must consume the same pool bytes when writing
// and reading, so the read pass regenerates the exact values the write pass produced.

template <typename Stream> bool FuzzRoundTrip( Stream & stream, const uint8_t * ops, int numOps, ValuePool & pool )
{
    for ( int i = 0; i < numOps; i++ )
    {
        const int select = ops[i] & 15;
        const int param = ops[i] >> 4;              // [0,15]

        switch ( select )
        {
            case 0:
            case 1:
            {
                const int bits = ( select == 0 ) ? param + 1 : param + 17;                      // [1,32]
                const uint32_t mask = ( bits == 32 ) ? 0xFFFFFFFF : ( ( 1U << bits ) - 1 );
                const uint32_t expected = pool.NextUint32() & mask;
                uint32_t value = Stream::IsWriting ? expected : 0;
                serialize_bits( stream, value, bits );
                if ( Stream::IsReading )
                {
                    fuzz_check( value == expected );
                }
            }
            break;

            case 2:
            {
                const int32_t expected = -100 + int32_t( pool.NextUint32() % 201 );
                int32_t value = Stream::IsWriting ? expected : 0;
                serialize_int( stream, value, -100, +100 );
                if ( Stream::IsReading )
                {
                    fuzz_check( value == expected );
                }
            }
            break;

            case 3:
            {
                const int32_t expected = (int32_t) pool.NextUint32();
                int32_t value = Stream::IsWriting ? expected : 0;
                serialize_int( stream, value, INT32_MIN, INT32_MAX );
                if ( Stream::IsReading )
                {
                    fuzz_check( value == expected );
                }

                const int64_t expected64 = (int64_t) pool.NextUint64();
                int64_t value64 = Stream::IsWriting ? expected64 : 0;
                serialize_int64( stream, value64, INT64_MIN, INT64_MAX );
                if ( Stream::IsReading )
                {
                    fuzz_check( value64 == expected64 );
                }

                const int64_t bound = int64_t( param + 1 ) << 35;               // spans of varying width exercise bits_required64
                const uint64_t span = uint64_t( bound ) * 2 + 1;
                const int64_t expected_ranged = -bound + int64_t( pool.NextUint64() % span );
                int64_t ranged64 = Stream::IsWriting ? expected_ranged : 0;
                serialize_int64( stream, ranged64, -bound, +bound );
                if ( Stream::IsReading )
                {
                    fuzz_check( ranged64 == expected_ranged );
                }
            }
            break;

            case 4:
            {
                const bool expected = ( pool.NextByte() & 1 ) != 0;
                bool value = Stream::IsWriting ? expected : false;
                serialize_bool( stream, value );
                if ( Stream::IsReading )
                {
                    fuzz_check( value == expected );
                }
            }
            break;

            case 5:
            {
                // arbitrary bit patterns, including nan and inf: floats must round trip bit exact
                const uint32_t expected_bits = pool.NextUint32();
                float value = 0.0f;
                if ( Stream::IsWriting )
                {
                    memcpy( &value, &expected_bits, 4 );
                }
                serialize_float( stream, value );
                if ( Stream::IsReading )
                {
                    uint32_t value_bits = 0;
                    memcpy( &value_bits, &value, 4 );
                    fuzz_check( value_bits == expected_bits );
                }
            }
            break;

            case 6:
            {
                const uint64_t expected_bits = pool.NextUint64();
                double value = 0.0;
                if ( Stream::IsWriting )
                {
                    memcpy( &value, &expected_bits, 8 );
                }
                serialize_double( stream, value );
                if ( Stream::IsReading )
                {
                    uint64_t value_bits = 0;
                    memcpy( &value_bits, &value, 8 );
                    fuzz_check( value_bits == expected_bits );
                }
            }
            break;

            case 7:
            {
                // arbitrary bit patterns again: out of range, nan and inf values must clamp into
                // [min,max] on write, and finite in range values must round trip within the resolution
                const uint32_t expected_bits = pool.NextUint32();
                float expected = 0.0f;
                memcpy( &expected, &expected_bits, 4 );
                float value = Stream::IsWriting ? expected : 0.0f;
                serialize_compressed_float( stream, value, -10.0f, +10.0f, 0.01f );
                if ( Stream::IsReading )
                {
                    fuzz_check( value >= -10.001f && value <= +10.001f );
                    const bool finite = ( expected_bits & 0x7FFFFFFF ) < 0x7F800000;            // bit test: immune to fast math
                    if ( finite && expected >= -10.0f && expected <= +10.0f )
                    {
                        fuzz_check( fabs( value - expected ) <= 0.011f );
                    }
                }
            }
            break;

            case 8:
            {
                const uint64_t expected = pool.NextUint64();
                uint64_t value = Stream::IsWriting ? expected : 0;
                serialize_uint64( stream, value );
                if ( Stream::IsReading )
                {
                    fuzz_check( value == expected );
                }
            }
            break;

            case 9:
            {
                const int numBytes = param * 16 + 1;                    // [1,241]
                uint8_t expected[256];
                for ( int j = 0; j < numBytes; j++ )
                    expected[j] = pool.NextByte();
                uint8_t value[256];
                if ( Stream::IsWriting )
                {
                    memcpy( value, expected, numBytes );
                }
                serialize_bytes( stream, value, numBytes );
                if ( Stream::IsReading )
                {
                    fuzz_check( memcmp( value, expected, numBytes ) == 0 );
                }
            }
            break;

            case 10:
            {
                char expected[32];
                const int length = pool.NextByte() % ( sizeof( expected ) - 1 );
                for ( int j = 0; j < length; j++ )
                {
                    const uint8_t c = pool.NextByte();
                    expected[j] = ( c != 0 ) ? (char) c : ' ';
                }
                expected[length] = '\0';
                char value[32];
                if ( Stream::IsWriting )
                {
                    memcpy( value, expected, length + 1 );
                }
                serialize_string( stream, value, (int) sizeof( value ) );
                if ( Stream::IsReading )
                {
                    fuzz_check( strcmp( value, expected ) == 0 );
                }
            }
            break;

            case 11:
            {
                wchar_t expected[8];
                const int length = pool.NextByte() % ( sizeof( expected ) / sizeof( wchar_t ) - 1 );
                for ( int j = 0; j < length; j++ )
                    expected[j] = (wchar_t) ( pool.NextUint32() % 0xFFFF + 1 );                 // [1,0xFFFF]: valid for 2 and 4 byte wchar_t
                expected[length] = L'\0';
                wchar_t value[8];
                if ( Stream::IsWriting )
                {
                    memcpy( value, expected, ( length + 1 ) * sizeof( wchar_t ) );
                }
                serialize_wstring( stream, value, (int) ( sizeof( value ) / sizeof( wchar_t ) ) );
                if ( Stream::IsReading )
                {
                    fuzz_check( wcscmp( value, expected ) == 0 );
                }
            }
            break;

            case 12:
            {
                serialize_align( stream );
            }
            break;

            case 13:
            {
                const int previous = param * 1000 - 8000;
                const int expected = previous + 1 + int( pool.NextUint32() % 1000000 );         // strictly greater than previous
                int value = Stream::IsWriting ? expected : 0;
                serialize_int_relative( stream, previous, value );
                if ( Stream::IsReading )
                {
                    fuzz_check( value == expected );
                }
            }
            break;

            case 14:
            {
                const int32_t max = ( param + 1 ) * 1000;               // ranges of varying width exercise bits_required
                const int32_t expected = int32_t( pool.NextUint32() % uint32_t( max + 1 ) );
                int32_t value = Stream::IsWriting ? expected : 0;
                serialize_int( stream, value, 0, max );
                if ( Stream::IsReading )
                {
                    fuzz_check( value == expected );
                }
            }
            break;

            case 15:
            {
                const uint8_t expected8 = pool.NextByte();
                const uint16_t expected16 = (uint16_t) pool.NextUint32();
                const uint32_t expected32 = pool.NextUint32();
                uint8_t value8 = Stream::IsWriting ? expected8 : (uint8_t) 0;
                uint16_t value16 = Stream::IsWriting ? expected16 : (uint16_t) 0;
                uint32_t value32 = Stream::IsWriting ? expected32 : 0;
                serialize_uint8( stream, value8 );
                serialize_uint16( stream, value16 );
                serialize_uint32( stream, value32 );
                if ( Stream::IsReading )
                {
                    fuzz_check( value8 == expected8 );
                    fuzz_check( value16 == expected16 );
                    fuzz_check( value32 == expected32 );
                }
            }
            break;
        }
    }

    return true;
}

// ---------------------------------------------------------------------------------------------
// schema coverage. the schema Read path is a large hostile-input parser in its own right, so it
// gets the same two passes as the streams: hostile bytes must fail cleanly or decode to in-contract
// values, and a differential round trip against a stream twin must be byte identical.

struct FuzzElement
{
    uint32_t value;
};

using FuzzElementSchema = serialize::schema< serialize::bits<&FuzzElement::value, 9> >;

struct FuzzSchemaPacket
{
    uint32_t style;
    bool extended;
    float charge;
    uint32_t bonus;
    int32_t base;
    int32_t next;
    char name[9];
    uint8_t blob[5];
    int32_t blob_count;
    FuzzElement rings[4];
    int32_t ring_count;
    uint32_t tail;
};

// one of everything the schema runner does: constants, back references, matches, relative ints,
// runtime-length sections, a bounded counted array with a nonzero minimum, and reserved bits
using FuzzSchema = serialize::schema<
    serialize::const_<0x5AF3, 16>,
    serialize::bits<&FuzzSchemaPacket::style, 2>,
    serialize::bool_<&FuzzSchemaPacket::extended>,
    serialize::branch_on<&FuzzSchemaPacket::extended,
        serialize::fields< serialize::float_<&FuzzSchemaPacket::charge> >,
        serialize::fields<> >,
    serialize::match<&FuzzSchemaPacket::style,
        serialize::case_<1, serialize::bits<&FuzzSchemaPacket::bonus, 8>>>,
    serialize::int_<&FuzzSchemaPacket::base, 0, 500>,
    serialize::int_relative_<&FuzzSchemaPacket::base, &FuzzSchemaPacket::next>,
    serialize::string<&FuzzSchemaPacket::name>,
    serialize::bytes_n<&FuzzSchemaPacket::blob, &FuzzSchemaPacket::blob_count>,
    serialize::array_n<&FuzzSchemaPacket::rings, &FuzzSchemaPacket::ring_count, FuzzElementSchema, 1, 3>,
    serialize::reserved_<3>,
    serialize::bits<&FuzzSchemaPacket::tail, 12> >;

// the stream twin: must be byte identical to FuzzSchema, field for field
template <typename Stream> bool FuzzSchemaTwin( Stream & stream, FuzzSchemaPacket & p )
{
    uint32_t magic = 0x5AF3;
    serialize_bits( stream, magic, 16 );
    if ( Stream::IsReading && magic != 0x5AF3 )
        return false;
    serialize_bits( stream, p.style, 2 );
    serialize_bool( stream, p.extended );
    if ( p.extended )
    {
        serialize_float( stream, p.charge );
    }
    switch ( p.style )
    {
        case 1: serialize_bits( stream, p.bonus, 8 ); break;
        default: break;
    }
    serialize_int( stream, p.base, 0, 500 );
    serialize_int_relative( stream, p.base, p.next );
    serialize_string( stream, p.name, (int) sizeof( p.name ) );
    serialize_int( stream, p.blob_count, 0, 5 );
    serialize_bytes( stream, p.blob, p.blob_count );
    serialize_int( stream, p.ring_count, 1, 3 );
    for ( int i = 0; i < p.ring_count; i++ )
        serialize_bits( stream, p.rings[i].value, 9 );
    uint32_t reserved = 0;
    serialize_bits( stream, reserved, 3 );
    if ( Stream::IsReading && reserved != 0 )
        return false;
    serialize_bits( stream, p.tail, 12 );
    return true;
}

inline bool FuzzSchemaPacketsEqual( const FuzzSchemaPacket & a, const FuzzSchemaPacket & b )
{
    if ( a.style != b.style || a.extended != b.extended || a.base != b.base || a.next != b.next )
        return false;
    if ( a.extended && memcmp( &a.charge, &b.charge, 4 ) != 0 )     // bit compare: charge may be any 32 bits, including NaN
        return false;
    if ( a.style == 1 && a.bonus != b.bonus )
        return false;
    if ( strcmp( a.name, b.name ) != 0 )
        return false;
    if ( a.blob_count != b.blob_count || memcmp( a.blob, b.blob, (size_t) a.blob_count ) != 0 )
        return false;
    if ( a.ring_count != b.ring_count )
        return false;
    for ( int i = 0; i < a.ring_count; i++ )
    {
        if ( a.rings[i].value != b.rings[i].value )
            return false;
    }
    return a.tail == b.tail;
}

// pass 1: hostile bytes. reads must fail cleanly or produce in-contract values, and a successful
// decode must survive a write/read round trip with identical fields and an exact measure.
inline void FuzzSchemaHostile( const uint8_t * buffer, int64_t bytes )
{
    FuzzSchemaPacket p;
    memset( (void*) &p, 0, sizeof( p ) );
    if ( !FuzzSchema::Read( buffer, bytes, p ) )
        return;

    fuzz_check( p.style <= 3 );
    fuzz_check( p.base >= 0 && p.base <= 500 );
    fuzz_check( p.next > p.base );
    fuzz_check( strlen( p.name ) < sizeof( p.name ) );
    fuzz_check( p.blob_count >= 0 && p.blob_count <= 5 );
    fuzz_check( p.ring_count >= 1 && p.ring_count <= 3 );
    for ( int i = 0; i < p.ring_count; i++ )
        fuzz_check( p.rings[i].value < 512 );
    fuzz_check( p.tail < 4096 );

    uint8_t reencoded[FuzzSchema::MaxBytes + 8];
    memset( reencoded, 0, sizeof( reencoded ) );
    const int64_t written = FuzzSchema::Write( reencoded, FuzzSchema::MaxBytes, p );
    fuzz_check( written == FuzzSchema::MeasureBytes( p ) );

    FuzzSchemaPacket q;
    memset( (void*) &q, 0, sizeof( q ) );
    fuzz_check( FuzzSchema::Read( reencoded, written, q ) == true );
    fuzz_check( FuzzSchemaPacketsEqual( p, q ) );
}

// pass 2: differential round trip. in-range values from the pool go through the schema writer and
// the stream twin: the bytes must be identical, the measure exact, and both readers must agree.
inline void FuzzSchemaDifferential( ValuePool & pool )
{
    FuzzSchemaPacket p;
    memset( (void*) &p, 0, sizeof( p ) );
    p.style = pool.NextByte() & 3;
    p.extended = ( pool.NextByte() & 1 ) != 0;
    const uint32_t charge_bits = pool.NextUint32();
    memcpy( &p.charge, &charge_bits, 4 );
    p.bonus = pool.NextByte();
    p.base = int32_t( pool.NextUint32() % 501 );
    static const uint32_t bucket_diffs[8] = { 1, 3, 20, 200, 4000, 60000, 1000000, 5 };
    p.next = int32_t( uint32_t( p.base ) + bucket_diffs[pool.NextByte() & 7] );
    const int name_length = int( pool.NextByte() % 9 );
    for ( int i = 0; i < name_length; i++ )
        p.name[i] = char( 'a' + pool.NextByte() % 26 );
    p.name[name_length] = '\0';
    p.blob_count = int( pool.NextByte() % 6 );
    for ( int i = 0; i < p.blob_count; i++ )
        p.blob[i] = pool.NextByte();
    p.ring_count = 1 + int( pool.NextByte() % 3 );
    for ( int i = 0; i < p.ring_count; i++ )
        p.rings[i].value = pool.NextUint32() & 511;
    p.tail = pool.NextUint32() & 4095;

    uint8_t schema_buffer[FuzzSchema::MaxBytes + 8];
    memset( schema_buffer, 0, sizeof( schema_buffer ) );
    const int64_t schema_bytes = FuzzSchema::Write( schema_buffer, FuzzSchema::MaxBytes, p );
    fuzz_check( schema_bytes == FuzzSchema::MeasureBytes( p ) );

    uint8_t twin_buffer[64 + 8];
    memset( twin_buffer, 0, sizeof( twin_buffer ) );
    serialize::WriteStream twinStream( twin_buffer, 64 );
    fuzz_check( FuzzSchemaTwin( twinStream, p ) == true );
    twinStream.Flush();
    fuzz_check( twinStream.GetBytesProcessed() == schema_bytes );
    fuzz_check( memcmp( twin_buffer, schema_buffer, (size_t) schema_bytes ) == 0 );
    fuzz_check( FuzzSchema::MeasureBits( p ) == twinStream.GetBitsProcessed() );

    FuzzSchemaPacket out;
    memset( (void*) &out, 0, sizeof( out ) );
    fuzz_check( FuzzSchema::Read( twin_buffer, schema_bytes, out ) == true );
    fuzz_check( FuzzSchemaPacketsEqual( p, out ) );

    FuzzSchemaPacket out2;
    memset( (void*) &out2, 0, sizeof( out2 ) );
    serialize::ReadStream twinRead( schema_buffer, schema_bytes );
    fuzz_check( FuzzSchemaTwin( twinRead, out2 ) == true );
    fuzz_check( FuzzSchemaPacketsEqual( p, out2 ) );
}

extern "C" int LLVMFuzzerTestOneInput( const uint8_t * data, size_t size )
{
    const int NumOps = 32;

    if ( size <= NumOps || size > NumOps + 4096 )
    {
        return 0;
    }

    const uint8_t * ops = data;
    const uint8_t * payload = data + NumOps;
    const size_t payloadBytes = size - NumOps;

    // pass 1: hostile read of arbitrary bytes
    {
        // the buffer allocation must extend at least 8 bytes past the data, because the bit reader loads 64 bit windows
        std::vector<uint8_t> buffer( payloadBytes + 8, 0 );
        memcpy( buffer.data(), payload, payloadBytes );

        serialize::ReadStream stream( buffer.data(), (int) payloadBytes );

        FuzzRead( stream, ops, NumOps );
    }

    // pass 2: differential round trip of values generated from the same bytes
    {
        // worst case is ~260 bytes per op (a 241 byte serialize_bytes plus alignment), so 32 ops fit comfortably.
        // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader
        const int WriteBufferSize = 16 * 1024;
        alignas( 4 ) uint8_t writeBuffer[WriteBufferSize + 8];
        memset( writeBuffer, 0, sizeof( writeBuffer ) );

        serialize::WriteStream writeStream( writeBuffer, WriteBufferSize );
        ValuePool writePool( payload, payloadBytes );
        fuzz_check( FuzzRoundTrip( writeStream, ops, NumOps, writePool ) == true );             // writing in-range values must always succeed
        writeStream.Flush();

        serialize::MeasureStream measureStream;
        ValuePool measurePool( payload, payloadBytes );
        fuzz_check( FuzzRoundTrip( measureStream, ops, NumOps, measurePool ) == true );
        fuzz_check( measureStream.GetBitsProcessed() >= writeStream.GetBitsProcessed() );       // measure must be conservative

        // the 16KB writeBuffer extends well past the written bytes, satisfying the reader's 8-bytes-past allocation contract
        serialize::ReadStream readStream( writeBuffer, writeStream.GetBytesProcessed() );
        ValuePool readPool( payload, payloadBytes );
        fuzz_check( FuzzRoundTrip( readStream, ops, NumOps, readPool ) == true );               // reading back our own data must always succeed
    }

    // pass 3: schema hostile read of the same arbitrary bytes
    {
        std::vector<uint8_t> buffer( payloadBytes + 8, 0 );
        memcpy( buffer.data(), payload, payloadBytes );
        FuzzSchemaHostile( buffer.data(), (int64_t) payloadBytes );
    }

    // pass 4: schema differential round trip against the stream twin
    {
        ValuePool schemaPool( payload, payloadBytes );
        FuzzSchemaDifferential( schemaPool );
    }

    return 0;
}
