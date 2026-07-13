/*
    serialize.modern wire compatibility harness

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
    Proves serialize.modern is wire compatible with classic serialize, in both directions.

    This one source file compiles against EITHER library: the header is included with angle
    brackets, so the include path passed by the build selects classic or modern (the two headers
    share a header guard, namespace and macros, and can never coexist in one translation unit).
    Only the API surface common to both libraries is used.

    The build produces two executables, wire_compat_classic and wire_compat_modern. Each supports:

        wire_compat_<lib> write <file>      serialize the corpus and write the raw bytes to <file>
        wire_compat_<lib> read <file>       read <file> and verify every corpus value decodes exactly

    CI runs the full matrix: classic and modern each write the corpus, the two byte streams must be
    identical, and each library must read the stream the other wrote. Any wire format divergence
    fails the byte comparison or one of the cross reads, and the PR does not go green.

    The corpus is deterministic (fixed seed LCG, no platform dependent values) and deliberately
    stresses the places where the two implementations differ internally while promising identical
    bytes: bit widths in (32,64] (classic splits at 32, modern at 56), serialize_int64 ranges on
    both sides of every split point, every serialize_int_relative bucket, alignment padding, and
    byte blocks with head/tail sizes around the 64 bit word boundary.
*/

#include <serialize.h>

#include <stdio.h>
#include <stdlib.h>

// deterministic value source. write and read modes step it identically, so the reader always
// knows the exact value the writer serialized.

struct Gen
{
    uint64_t state;

    explicit Gen( uint64_t seed ) : state( seed ) {}

    uint64_t next()
    {
        state = state * 6364136223846793005ULL + 1442695040888963407ULL;
        return state;
    }

    uint32_t u32() { return uint32_t( next() >> 16 ); }

    uint64_t u64() { return ( uint64_t( u32() ) << 32 ) | u32(); }
};

static int g_failures = 0;

#define wire_check( condition )                                                             \
    do                                                                                      \
    {                                                                                       \
        if ( !(condition) )                                                                 \
        {                                                                                   \
            printf( "wire check failed: ( %s ), line %d\n", #condition, __LINE__ );         \
            g_failures++;                                                                   \
            return false;                                                                   \
        }                                                                                   \
    } while (0)

// every field follows the same pattern: regenerate the expected value from the lockstep
// generator, serialize it on write, verify it on read.

template <typename Stream> bool CorpusBits( Stream & stream, Gen & gen, int bits )
{
    const uint64_t mask = ( bits == 64 ) ? 0xFFFFFFFFFFFFFFFFULL : ( ( uint64_t(1) << bits ) - 1 );
    const uint64_t expected = gen.u64() & mask;
    if ( bits <= 32 )
    {
        uint32_t value = 0;
        if ( Stream::IsWriting )
            value = uint32_t( expected );
        serialize_bits( stream, value, bits );
        if ( Stream::IsReading )
            wire_check( value == uint32_t( expected ) );
    }
    else
    {
        uint64_t value = 0;
        if ( Stream::IsWriting )
            value = expected;
        serialize_bits( stream, value, bits );
        if ( Stream::IsReading )
            wire_check( value == expected );
    }
    return true;
}

template <typename Stream> bool CorpusInt( Stream & stream, Gen & gen, int32_t min, int32_t max )
{
    // pick the expected value in the unsigned domain: min + (r % span) overflows signed arithmetic for the full range
    const uint32_t span = uint32_t( max ) - uint32_t( min );        // span 0xFFFFFFFF means the full 2^32 range
    const uint32_t offset = ( span == 0xFFFFFFFF ) ? gen.u32() : ( gen.u32() % ( span + 1 ) );
    const int32_t expected = int32_t( uint32_t( min ) + offset );
    int32_t value = 0;
    if ( Stream::IsWriting )
        value = expected;
    serialize_int( stream, value, min, max );
    if ( Stream::IsReading )
        wire_check( value == expected );
    return true;
}

template <typename Stream> bool CorpusInt64( Stream & stream, Gen & gen, int64_t min, int64_t max )
{
    const uint64_t span = uint64_t( max ) - uint64_t( min );
    const uint64_t offset = ( span == 0xFFFFFFFFFFFFFFFFULL ) ? gen.u64() : ( gen.u64() % ( span + 1 ) );
    const int64_t expected = int64_t( uint64_t( min ) + offset );
    int64_t value = 0;
    if ( Stream::IsWriting )
        value = expected;
    serialize_int64( stream, value, min, max );
    if ( Stream::IsReading )
        wire_check( value == expected );
    return true;
}

template <typename Stream> bool CorpusBool( Stream & stream, Gen & gen )
{
    const bool expected = ( gen.u32() & 1 ) != 0;
    bool value = false;
    if ( Stream::IsWriting )
        value = expected;
    serialize_bool( stream, value );
    if ( Stream::IsReading )
        wire_check( value == expected );
    return true;
}

template <typename Stream> bool CorpusFloat( Stream & stream, Gen & gen )
{
    // finite values only: the wire moves raw bits either way, but NaN != NaN would break the comparison below
    const float expected = float( int32_t( gen.u32() % 2000001 ) - 1000000 ) * 0.25f;
    float value = 0.0f;
    if ( Stream::IsWriting )
        value = expected;
    serialize_float( stream, value );
    if ( Stream::IsReading )
        wire_check( value == expected );
    return true;
}

template <typename Stream> bool CorpusDouble( Stream & stream, Gen & gen )
{
    const double expected = double( int64_t( gen.u64() % 2000000001ULL ) - 1000000000 ) * 0.125;
    double value = 0.0;
    if ( Stream::IsWriting )
        value = expected;
    serialize_double( stream, value );
    if ( Stream::IsReading )
        wire_check( value == expected );
    return true;
}

template <typename Stream> bool CorpusCompressedFloat( Stream & stream, Gen & gen, float min, float max, float res )
{
    const float expected = min + ( max - min ) * ( float( gen.u32() % 10001 ) / 10000.0f );
    float value = 0.0f;
    if ( Stream::IsWriting )
        value = expected;
    serialize_compressed_float( stream, value, min, max, res );
    if ( Stream::IsReading )
        wire_check( value >= min - res && value <= max + res && ( value - expected <= res && expected - value <= res ) );
    return true;
}

template <typename Stream> bool CorpusBytes( Stream & stream, Gen & gen )
{
    // lengths cycle through every head/tail combination around the 64 bit word boundary
    const int length = int( gen.u32() % 40 );
    uint8_t expected[40];
    for ( int i = 0; i < length; i++ )
        expected[i] = uint8_t( gen.u32() );
    uint8_t value[40];
    memset( value, 0, sizeof( value ) );
    if ( Stream::IsWriting )
        memcpy( value, expected, size_t( length ) );
    serialize_bytes( stream, value, length );
    if ( Stream::IsReading )
        wire_check( memcmp( value, expected, size_t( length ) ) == 0 );
    return true;
}

template <typename Stream> bool CorpusString( Stream & stream, Gen & gen )
{
    const int length = int( gen.u32() % 32 );
    char expected[33];
    for ( int i = 0; i < length; i++ )
        expected[i] = char( 'a' + gen.u32() % 26 );
    expected[length] = '\0';
    char value[33];
    memset( value, 0, sizeof( value ) );
    if ( Stream::IsWriting )
        memcpy( value, expected, size_t( length + 1 ) );
    serialize_string( stream, value, int( sizeof( value ) ) );
    if ( Stream::IsReading )
        wire_check( strcmp( value, expected ) == 0 );
    return true;
}

template <typename Stream> bool CorpusWString( Stream & stream, Gen & gen )
{
    const int length = int( gen.u32() % 16 );
    wchar_t expected[17];
    for ( int i = 0; i < length; i++ )
        expected[i] = wchar_t( 0x0400 + gen.u32() % 0x100 );        // cyrillic block: BMP only, valid on 2 and 4 byte wchar_t
    expected[length] = L'\0';
    wchar_t value[17];
    memset( value, 0, sizeof( value ) );
    if ( Stream::IsWriting )
        memcpy( value, expected, sizeof( wchar_t ) * size_t( length + 1 ) );
    serialize_wstring( stream, value, int( sizeof( value ) / sizeof( wchar_t ) ) );
    if ( Stream::IsReading )
        wire_check( wcscmp( value, expected ) == 0 );
    return true;
}

template <typename Stream> bool CorpusIntRelative( Stream & stream, Gen & gen )
{
    // every encoding bucket: +1, two bit, four bit, eight bit, twelve bit, sixteen bit, 32 bit fallback
    static const uint32_t bucket_diffs[] = { 1, 5, 20, 200, 4000, 60000, 1000000 };
    const uint32_t diff = bucket_diffs[gen.u32() % 7];
    const int previous = int( gen.u32() % 1000000 );
    const int expected = int( uint32_t( previous ) + diff );
    int value = 0;
    if ( Stream::IsWriting )
        value = expected;
    serialize_int_relative( stream, previous, value );
    if ( Stream::IsReading )
        wire_check( value == expected );
    return true;
}

// int64 ranges straddling every internal split point: classic encodes wide values as 32+32,
// modern as a single put up to 56 bits then 56+rest. identical bytes are the whole point.

struct Int64Range { int64_t min; int64_t max; };

static const Int64Range int64_ranges[] =
{
    { -1, 1 },
    { -100, +100 },
    { 0, 1000000 },
    { INT32_MIN, INT32_MAX },
    { 0, int64_t( 1ULL << 31 ) },                       // 32 bits
    { 0, int64_t( 1ULL << 32 ) },                       // 33 bits: first width past the classic single dword path
    { -5000000000LL, +5000000000LL },                   // 34 bits
    { 0, int64_t( ( 1ULL << 55 ) - 1 ) },               // 55 bits
    { 0, int64_t( ( 1ULL << 56 ) - 1 ) },               // 56 bits: the modern single put limit
    { 0, int64_t( 1ULL << 56 ) },                       // 57 bits: first width past the modern single put
    { 0, int64_t( ( 1ULL << 62 ) - 1 ) },               // 62 bits
    { INT64_MIN / 2, INT64_MAX / 2 },                   // 63 bits
    { INT64_MIN, INT64_MAX },                           // 64 bits
};

template <typename Stream> bool SerializeCorpus( Stream & stream )
{
    Gen gen( 0x5EED5EED5EED5EEDULL );

    const int NumIterations = 2000;

    for ( int i = 0; i < NumIterations; i++ )
    {
        // every bit width in [1,64], walking the intra-byte offset through every alignment
        if ( !CorpusBits( stream, gen, 1 + i % 64 ) )
            return false;

        if ( !CorpusInt( stream, gen, -100, +100 ) )
            return false;

        if ( !CorpusInt( stream, gen, INT32_MIN, INT32_MAX ) )
            return false;

        if ( !CorpusInt64( stream, gen, int64_ranges[i % 13].min, int64_ranges[i % 13].max ) )
            return false;

        if ( !CorpusBool( stream, gen ) )
            return false;

        if ( !CorpusFloat( stream, gen ) )
            return false;

        if ( !CorpusIntRelative( stream, gen ) )
            return false;

        // heavier fields on a stride, so the corpus stays dominated by bitpacked values
        if ( ( i % 5 ) == 0 )
        {
            if ( !CorpusDouble( stream, gen ) )
                return false;

            if ( !CorpusCompressedFloat( stream, gen, -10.0f, +10.0f, 0.01f ) )
                return false;

            if ( !CorpusBytes( stream, gen ) )
                return false;
        }

        if ( ( i % 7 ) == 0 )
        {
            if ( !CorpusString( stream, gen ) )
                return false;

            if ( !CorpusWString( stream, gen ) )
                return false;

            serialize_align( stream );
        }
    }

    return true;
}

// buffer sizing satisfies every contract of both libraries: the size is a multiple of 8
// (classic write requirement) and the allocation extends 8 bytes past it (modern write
// requirement, and the read requirement of both).

static const int64_t BufferSize = 8 * 1024 * 1024;

int Usage()
{
    printf( "usage: wire_compat [write|read] <file>\n" );
    return 1;
}

int main( int argc, char ** argv )
{
    if ( argc != 3 )
        return Usage();

    const char * mode = argv[1];
    const char * path = argv[2];

    uint8_t * buffer = (uint8_t*) malloc( BufferSize + 8 );
    if ( !buffer )
    {
        printf( "error: could not allocate buffer\n" );
        return 1;
    }
    memset( buffer, 0, size_t( BufferSize + 8 ) );

    if ( strcmp( mode, "write" ) == 0 )
    {
        serialize::WriteStream stream( buffer, BufferSize );
        if ( !SerializeCorpus( stream ) )
        {
            printf( "error: corpus write failed\n" );
            return 1;
        }
        stream.Flush();
        const int64_t bytesWritten = stream.GetBytesProcessed();

        FILE * file = fopen( path, "wb" );
        if ( !file )
        {
            printf( "error: could not open '%s' for writing\n", path );
            return 1;
        }
        if ( (int64_t) fwrite( buffer, 1, size_t( bytesWritten ), file ) != bytesWritten )
        {
            printf( "error: short write to '%s'\n", path );
            return 1;
        }
        fclose( file );

        printf( "wrote %lld bytes of corpus (%s)\n", (long long) bytesWritten, SERIALIZE_VERSION );
        free( buffer );
        return 0;
    }

    if ( strcmp( mode, "read" ) == 0 )
    {
        FILE * file = fopen( path, "rb" );
        if ( !file )
        {
            printf( "error: could not open '%s' for reading\n", path );
            return 1;
        }
        const int64_t bytesRead = (int64_t) fread( buffer, 1, size_t( BufferSize ), file );
        fclose( file );
        if ( bytesRead <= 0 )
        {
            printf( "error: could not read '%s'\n", path );
            return 1;
        }

        serialize::ReadStream stream( buffer, bytesRead );
        if ( !SerializeCorpus( stream ) || g_failures != 0 )
        {
            printf( "error: corpus read failed: the wire format has diverged (%s reading %lld bytes)\n", SERIALIZE_VERSION, (long long) bytesRead );
            return 1;
        }

        // the reader must consume exactly the bytes the writer produced, not just decode a prefix
        const int64_t bytesProcessed = stream.GetBytesProcessed();
        if ( bytesProcessed != bytesRead )
        {
            printf( "error: reader consumed %lld of %lld bytes\n", (long long) bytesProcessed, (long long) bytesRead );
            return 1;
        }

        printf( "verified %lld bytes of corpus (%s)\n", (long long) bytesRead, SERIALIZE_VERSION );
        free( buffer );
        return 0;
    }

    return Usage();
}
