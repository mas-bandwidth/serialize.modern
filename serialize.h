/*
    serialize.modern

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

#ifndef SERIALIZE_H
#define SERIALIZE_H

/** @file */

/*
    serialize.modern is the modern C++ (C++23) port of serialize. The wire format is identical to
    classic serialize: data written by either library reads back correctly with the other, and the
    golden wire format test pins the exact bytes.

    What changed relative to classic serialize:

    - Requires C++23. Endianness, byte swapping, bit math and type punning use <bit> and std::bit_cast
      instead of platform macros, compiler builtins and memcpy punning.

    - The bit writer keeps the classic 64 bit scratch, qword flush core. A fully branchless
      store-per-write design was built and measured against it on Apple Silicon: the well predicted
      flush branch won on every benchmark (the branchless design serializes on an extra shift of the
      scratch per write), so the flush design stays. Call FlushBits when you finish writing, exactly
      as in classic serialize.

    - Up to 56 bits move in a single operation (WriteBits64/ReadBits64). The bit stream is LSB-first,
      so one n-bit put produces bytes identical to the classic 32+32 split; 64 bit values with
      ranges that need at most 56 bits (serialize_int64) now cost one put instead of two.

    - The buffer allocation contract is symmetric and simpler: for both the writer and the reader, the
      buffer allocation must extend at least 8 bytes past the end of the data (the writer's final
      flush stores a whole qword; the reader loads 64 bit windows at byte granularity). The classic
      multiple-of-8 buffer size requirement for writes is gone: any buffer size works, as long as the
      allocation extends 8 bytes past it.

    - Compile time schemas (serialize::schema) describe fixed-layout packets as types: every field
      offset, shift and mask folds to a constant, reads become independent window loads and writes
      become ORs into registers. Measured ~3x faster reads and up to ~7x faster writes than the
      stream path, byte-identical on the wire. Schemas support compile time if/else (branch)
      and compose like serialize_object (object). See the schema section below.

    - The hot bitpack core is explicitly force-inlined. The 64 bit paths are big enough that compilers
      otherwise leave them out of line, and a function call in the middle of packet decode costs
      measurably (~25% stream read throughput on Apple Silicon).
*/

#define SERIALIZE_VERSION_MAJOR 1
#define SERIALIZE_VERSION_MINOR 0
#define SERIALIZE_VERSION_PATCH 0
#define SERIALIZE_VERSION "1.0.0"

#if defined(_MSVC_LANG)
    #if _MSVC_LANG < 202302L
        #error serialize.modern requires C++23. Compile with /std:c++latest, or use classic serialize for older toolchains.
    #endif
#elif defined(__cplusplus)
    // 202100L rather than 202302L: GCC 13 and earlier report __cplusplus as 202100L in -std=c++23
    // mode because their C++23 support is incomplete (GCC 14 is the first to report 202302L).
    // Everything this header needs (<bit>, std::byteswap, std::bit_cast, std::endian, if constexpr)
    // is present from GCC 12, so the partial-support value is accepted.
    #if __cplusplus < 202100L
        #error serialize.modern requires C++23. Compile with -std=c++23, or use classic serialize for older toolchains.
    #endif
#endif

#if defined(_MSC_VER)
#define serialize_restrict __restrict
#else // #if defined(_MSC_VER)
#define serialize_restrict __restrict__
#endif // #if defined(_MSC_VER)

#ifndef serialize_assert
#include <cassert>
#define serialize_assert assert
#endif // #ifndef serialize_assert

// forces the hot bitpack core inline. the 64 bit paths exceed compiler inline budgets, and a call
// in the middle of packet encode/decode spills the decode state: measured ~25% of stream read
// throughput. everything annotated with this is a handful of instructions once inlined.
#if defined(_MSC_VER)
#define serialize_force_inline __forceinline
#else
#define serialize_force_inline inline __attribute__((always_inline))
#endif

#if !defined(SERIALIZE_DEBUG) && !defined(SERIALIZE_RELEASE)
#if defined(NDEBUG)
#define SERIALIZE_RELEASE
#else
#define SERIALIZE_DEBUG
#endif
#elif defined(SERIALIZE_DEBUG) && defined(SERIALIZE_RELEASE)
#error Can only define one of debug & release
#endif

// everything the library itself needs. serialize.h is intentionally self contained:
// including it into a translation unit with no prior includes must compile.
#include <cstdint>      // fixed width integer types
#include <cstddef>      // size_t
#include <cstring>      // memcpy, memset, strlen
#include <cwchar>       // wcslen
#include <cmath>        // ceil, floor
#include <bit>          // bit_width, byteswap, bit_cast, endian
#include <concepts>     // unsigned_integral, convertible_to

namespace serialize
{
    // mixed endian platforms are not supported. everything below may assume the platform is one or the other.
    static_assert( std::endian::native == std::endian::little || std::endian::native == std::endian::big,
                   "serialize requires a little endian or big endian platform" );

    /// True if this platform is little endian. Network byte order for this library is little endian, because most machines are.
    inline constexpr bool little_endian = ( std::endian::native == std::endian::little );

    /**
        Convert an integer value from local byte order to network byte order.
        IMPORTANT: Because most machines are little endian, serialize defines network byte order to be little endian.
        @param value The input value in local byte order.
        @returns The input value converted to network byte order. On little endian processors this is the identity function. On big endian processors the bytes are swapped.
     */

    template <std::unsigned_integral T>
    [[nodiscard]] constexpr T host_to_network( T value ) noexcept
    {
        if constexpr ( little_endian )
        {
            return value;
        }
        else
        {
            return std::byteswap( value );
        }
    }

    /**
        Convert an integer value from network byte order to local byte order.
        IMPORTANT: Because most machines are little endian, serialize defines network byte order to be little endian.
        @param value The input value in network byte order.
        @returns The input value converted to local byte order. On little endian processors this is the identity function. On big endian processors the bytes are swapped.
     */

    template <std::unsigned_integral T>
    [[nodiscard]] constexpr T network_to_host( T value ) noexcept
    {
        if constexpr ( little_endian )
        {
            return value;
        }
        else
        {
            return std::byteswap( value );
        }
    }

    /**
        Copy a small block of bytes inline.
        memcpy with a runtime size compiles to a libc call, and for the small blocks typical of
        packet fields (a few to a few dozen bytes) the call and dispatch overhead costs more than
        the copy. Blocks of 8 bytes or more move as 8 byte chunks with an overlapping tail; smaller
        blocks split once on 4 or 2 bytes. The source and destination must not overlap.
     */

    serialize_force_inline void small_copy( uint8_t * serialize_restrict dst, const uint8_t * serialize_restrict src, size_t bytes ) noexcept
    {
        if ( bytes >= 8 )
        {
            size_t i = 0;
            for ( ; i + 8 <= bytes; i += 8 )
                memcpy( dst + i, src + i, 8 );
            if ( i < bytes )
                memcpy( dst + bytes - 8, src + bytes - 8, 8 );      // overlapping tail: in bounds on both sides because bytes >= 8
        }
        else if ( bytes >= 4 )
        {
            memcpy( dst, src, 4 );
            memcpy( dst + bytes - 4, src + bytes - 4, 4 );
        }
        else if ( bytes >= 2 )
        {
            memcpy( dst, src, 2 );
            memcpy( dst + bytes - 2, src + bytes - 2, 2 );
        }
        else if ( bytes == 1 )
        {
            dst[0] = src[0];
        }
    }

    /// Small copies bypass libc memcpy below this size. 64 bytes covers typical packet fields; the crossover to libc is far higher.
    inline constexpr int64_t SmallCopyMaxBytes = 64;

    /**
        Calculates the number of bits required to serialize an integer in range [min,max].
        constexpr: with constant min and max the result folds to a compile time constant.
        @param min The minimum value.
        @param max The maximum value.
        @returns The number of bits required to serialize the integer.
     */

    [[nodiscard]] constexpr int bits_required( uint32_t min, uint32_t max ) noexcept
    {
        // subtract in the unsigned domain: max - min overflows signed arithmetic when the range is wider than 2^31.
        // bit_width(0) == 0 handles the min == max case without a branch.
        return int( std::bit_width( max - min ) );
    }

    /**
        Calculates the number of bits required to serialize a 64 bit integer in range [min,max].
        constexpr: with constant min and max the result folds to a compile time constant.
        @param min The minimum value.
        @param max The maximum value.
        @returns The number of bits required to serialize the integer in [0,64].
     */

    [[nodiscard]] constexpr int bits_required64( uint64_t min, uint64_t max ) noexcept
    {
        // subtract in the unsigned domain: max - min overflows signed arithmetic when the range is wider than 2^63
        return int( std::bit_width( max - min ) );
    }

    /**
        The number of bits required to serialize an integer in [min,max], as a compile time constant.
        This is the modern spelling of classic serialize's BitsRequired<min,max>::result. The constexpr
        functions above work in constant expressions too; this variable template guarantees compile time
        evaluation and reads better in template arguments and array bounds.
        @see bits_required64
     */

    template <int64_t min, int64_t max>
    inline constexpr int bits_required_v = bits_required64( uint64_t(min), uint64_t(max) );

    /**
        Convert a signed integer to an unsigned integer with zig-zag encoding.
        0,-1,+1,-2,+2... becomes 0,1,2,3,4 ...
        @param n The input value.
        @returns The input value converted from signed to unsigned with zig-zag encoding.
     */

    [[nodiscard]] constexpr uint32_t signed_to_unsigned( int32_t n ) noexcept
    {
        // shift in the unsigned domain, so the sign bit cannot shift into undefined territory
        return ( uint32_t(n) << 1 ) ^ ( 0 - ( uint32_t(n) >> 31 ) );
    }

    /**
        Convert an unsigned integer to as signed integer with zig-zag encoding.
        0,1,2,3,4... becomes 0,-1,+1,-2,+2...
        @param n The input value.
        @returns The input value converted from unsigned to signed with zig-zag encoding.
     */

    [[nodiscard]] constexpr int32_t unsigned_to_signed( uint32_t n ) noexcept
    {
        return int32_t( ( n >> 1 ) ^ ( 0 - ( n & 1 ) ) );
    }

    /**
        Bitpacks unsigned integer values to a buffer.

        Implementation: 64 bit scratch, qword flush — the classic serialize design. Integer bit values
        are written to a 64 bit scratch value from right to left. Once the scratch fills to 64 bits it
        is flushed to memory as a qword and the handful of bits that spilled past 64 carry over into
        the next scratch. A fully branchless store-per-write design was measured against this one and
        lost on every benchmark: the flush branch predicts well, and the branchless design pays for an
        extra serial shift of the scratch on every write.

        Unlike classic serialize, a single put can move up to MaxWriteBits (56) bits, so 64 bit values
        with small ranges cost one put instead of two.

        The bit stream is written to memory in little endian order, which is considered network byte
        order for this library. Each word is stored with memcpy, so the buffer needs no particular
        alignment.

        IMPORTANT: The buffer allocation must extend at least 8 bytes past the end of the buffer size
        passed in: flushes store whole qwords, and when the buffer size is not a multiple of 8 the
        final flush lands up to 7 bytes past the end. Bytes past the written data are only ever
        written as zeros. This is the same allocation contract as BitReader, so one rule covers both
        sides: allocate 8 bytes more than you use. (Classic serialize instead required buffer sizes
        to be a multiple of 8; here any size works.)

        @see BitReader
     */

    class BitWriter
    {
    public:

        /// The maximum number of bits a single put can move. Matches BitReader::MaxReadBits so wide
        /// values split at the same point on both sides. Safe for the spill logic below: a put can
        /// only spill when the scratch already holds at least 64 - 56 = 8 bits, so the carry shift
        /// stays in [8,63] and never reaches the undefined shift-by-64.
        static constexpr int MaxWriteBits = 56;

        BitWriter() noexcept = default;

        /**
            Bit writer constructor.
            Creates a bit writer object to write to the specified buffer.
            @param data The pointer to the buffer to fill with bitpacked data. Does not need to be aligned: each word is stored with memcpy, matching the bit reader.
            @param bytes The size of the buffer in bytes. Any size is supported (the classic multiple-of-8 requirement is gone), but the allocation must extend at least 8 bytes past this size. Buffer sizes are effectively unlimited, because bit counts are stored in 64 bit signed integers.
         */

        BitWriter( void * serialize_restrict data, int64_t bytes ) noexcept
            : m_data( static_cast<uint8_t*>( data ) ), m_numBits( bytes * 8 )
        {
            serialize_assert( data );
            serialize_assert( bytes >= 0 );
        }

        void Initialize( void * serialize_restrict data, int64_t bytes ) noexcept
        {
            serialize_assert( data );
            serialize_assert( bytes >= 0 );
            m_data = static_cast<uint8_t*>( data );
            m_scratch = 0;
            m_numBits = bytes * 8;
            m_bitsWritten = 0;
            m_wordIndex = 0;
            m_scratchBits = 0;
        }

        /**
            Write bits to the buffer.
            Bits are written to the buffer as-is, without padding to nearest byte. Will assert if you try to write past the end of the buffer.
            A boolean value writes just 1 bit to the buffer, a value in range [0,31] can be written with just 5 bits and so on.
            IMPORTANT: When you have finished writing to your buffer, take care to call BitWriter::FlushBits, otherwise the last word of data will not get flushed to memory!
            @param value The integer value to write to the buffer. Must be in [0,(1<<bits)-1].
            @param bits The number of bits to encode in [1,32].
            @see BitReader::ReadBits
         */

        serialize_force_inline void WriteBits( uint32_t value, int bits ) noexcept
        {
            serialize_assert( m_data );                 // if this fires, the writer was used before Initialize
            serialize_assert( bits > 0 );
            serialize_assert( bits <= 32 );
            serialize_assert( m_bitsWritten + bits <= m_numBits );
            serialize_assert( uint64_t( value ) <= ( ( uint64_t(1) << bits ) - 1 ) );
            PutBits( value, bits );
        }

        /**
            Write up to 64 bits to the buffer.
            Values up to MaxWriteBits (56) wide are stored with a single put; wider values split into
            two puts. The bit stream is LSB-first, so the bytes produced are identical to writing the
            low 32 bits then the high bits as two calls (the classic encoding of 64 bit values).
            IMPORTANT: When you have finished writing to your buffer, take care to call BitWriter::FlushBits, otherwise the last word of data will not get flushed to memory!
            @param value The integer value to write to the buffer. Must be in [0,(1<<bits)-1] (all values are legal at 64 bits).
            @param bits The number of bits to encode in [1,64].
            @see BitReader::ReadBits64
         */

        serialize_force_inline void WriteBits64( uint64_t value, int bits ) noexcept
        {
            serialize_assert( m_data );                 // if this fires, the writer was used before Initialize
            serialize_assert( bits > 0 );
            serialize_assert( bits <= 64 );
            serialize_assert( m_bitsWritten + bits <= m_numBits );
            serialize_assert( bits == 64 || value <= ( ( uint64_t(1) << bits ) - 1 ) );
            if ( bits <= MaxWriteBits )
            {
                PutBits( value, bits );
            }
            else
            {
                // low bits first, then the high remainder: LSB-first makes this byte-identical to any other split
                PutBits( value & ( ( uint64_t(1) << MaxWriteBits ) - 1 ), MaxWriteBits );
                PutBits( value >> MaxWriteBits, bits - MaxWriteBits );
            }
        }

        /**
            Write an alignment to the bit stream, padding zeros so the bit index becomes is a multiple of 8.
            This is useful if you want to write some data to a packet that should be byte aligned. For example, an array of bytes, or a string.
            IMPORTANT: If the current bit index is already a multiple of 8, nothing is written.
            @see BitReader::ReadAlign
         */

        void WriteAlign() noexcept
        {
            const int remainderBits = int( m_bitsWritten % 8 );

            if ( remainderBits != 0 )
            {
                PutBits( 0, 8 - remainderBits );
                serialize_assert( ( m_bitsWritten % 8 ) == 0 );
            }
        }

        /**
            Write an array of bytes to the bit stream.
            Use this when you have to copy a large block of data into your bitstream.
            Faster than writing each byte via BitWriter::WriteBits( value, 8 ), because the bulk of the data is copied with memcpy once the stream reaches a 64 bit word boundary.
            The bit index must be byte aligned (see BitWriter::WriteAlign).
            @param data The byte array data to write to the bit stream.
            @param bytes The number of bytes to write.
            @see BitReader::ReadBytes
         */

        void WriteBytes( const uint8_t * serialize_restrict data, int64_t bytes ) noexcept
        {
            serialize_assert( m_data );                 // if this fires, the writer was used before Initialize
            serialize_assert( GetAlignBits() == 0 );
            serialize_assert( uint64_t(m_bitsWritten) + uint64_t(bytes) * 8 <= uint64_t(m_numBits) );
            serialize_assert( ( m_bitsWritten % 8 ) == 0 );

            int64_t headBytes = ( 8 - ( m_bitsWritten % 64 ) / 8 ) % 8;
            if ( headBytes > bytes )
                headBytes = bytes;
            for ( int64_t i = 0; i < headBytes; ++i )
                PutBits( data[i], 8 );
            if ( headBytes == bytes )
                return;

            serialize_assert( ( m_bitsWritten % 64 ) == 0 && m_scratchBits == 0 );      // the head bytes flushed the scratch at the word boundary

            const int64_t numWords = ( bytes - headBytes ) / 8;
            if ( numWords > 0 )
            {
                if ( numWords * 8 <= SmallCopyMaxBytes )
                    small_copy( m_data + (size_t) m_wordIndex * 8, data + headBytes, (size_t) ( numWords * 8 ) );
                else
                    memcpy( m_data + (size_t) m_wordIndex * 8, data + headBytes, (size_t) ( numWords * 8 ) );
                m_bitsWritten += numWords * 64;
                m_wordIndex += numWords;
                m_scratch = 0;
            }

            const int64_t tailStart = headBytes + numWords * 8;
            const int64_t tailBytes = bytes - tailStart;
            serialize_assert( tailBytes >= 0 && tailBytes < 8 );
            for ( int64_t i = 0; i < tailBytes; ++i )
                PutBits( data[tailStart+i], 8 );

            serialize_assert( GetAlignBits() == 0 );
            serialize_assert( headBytes + numWords * 8 + tailBytes == bytes );
        }

        /**
            Flush any remaining bits to memory.
            Call this once after you've finished writing bits to flush the last word of scratch to memory!
            @see BitWriter::WriteBits
         */

        void FlushBits() noexcept
        {
            if ( m_scratchBits != 0 )
            {
                serialize_assert( m_data );             // if this fires, the writer was used before Initialize
                serialize_assert( m_scratchBits < 64 );
                const uint64_t word = host_to_network( m_scratch );
                memcpy( m_data + (size_t) m_wordIndex * 8, &word, sizeof( word ) );     // stores a full qword: the allocation extends 8 bytes past the buffer size, and bytes past the written data are zeros
                m_scratch = 0;
                m_scratchBits = 0;
                m_wordIndex++;
            }
        }

        /**
            How many align bits would be written, if we were to write an align right now?
            @returns Result in [0,7], where 0 is zero bits required to align (already aligned) and 7 is worst case.
         */

        [[nodiscard]] int GetAlignBits() const noexcept
        {
            return ( 8 - int( m_bitsWritten % 8 ) ) % 8;
        }

        /**
            How many bits have we written so far?
            @returns The number of bits written to the bit buffer.
         */

        [[nodiscard]] int64_t GetBitsWritten() const noexcept
        {
            return m_bitsWritten;
        }

        /**
            How many bits are still available to write?
            For example, if the buffer size is 4, we have 32 bits available to write, if we have already written 10 bits then 22 are still available to write.
            @returns The number of bits available to write.
         */

        [[nodiscard]] int64_t GetBitsAvailable() const noexcept
        {
            return m_numBits - m_bitsWritten;
        }

        /**
            Get a pointer to the data written by the bit writer.
            Corresponds to the data block passed in to the constructor.
            @returns Pointer to the data written by the bit writer.
         */

        [[nodiscard]] const uint8_t * GetData() const noexcept
        {
            return m_data;
        }

        /**
            The number of bytes flushed to memory.
            This is effectively the size of the packet that you should send after you have finished bitpacking values with this class.
            The returned value is not always a multiple of 8, even though we flush qwords to memory. You won't miss any data in this case because the order of bits written is designed to work with the little endian memory layout.
            IMPORTANT: Make sure you call BitWriter::FlushBits before calling this method, otherwise you risk missing the last word of data.
         */

        [[nodiscard]] int64_t GetBytesWritten() const noexcept
        {
            return ( m_bitsWritten + 7 ) / 8;
        }

    private:

        /**
            The core all bit writes funnel into. Writes bits in [1,MaxWriteBits].
            The value is ORed into the scratch above the bits already buffered. If the scratch fills to
            64 bits it is stored to memory as a qword and the bits of the value that spilled past 64
            carry into the next scratch. A spilling put implies the scratch already held at least
            64 - MaxWriteBits = 8 bits, so the carry shift is in [8,63]: always defined.
         */

        serialize_force_inline void PutBits( uint64_t value, int bits ) noexcept
        {
            serialize_assert( bits > 0 );
            serialize_assert( bits <= MaxWriteBits );

            m_scratch |= value << m_scratchBits;

            const int newScratchBits = m_scratchBits + bits;

            if ( newScratchBits >= 64 )
            {
                const uint64_t word = host_to_network( m_scratch );
                memcpy( m_data + (size_t) m_wordIndex * 8, &word, sizeof( word ) );
                m_wordIndex++;
                m_scratch = value >> ( 64 - m_scratchBits );
                m_scratchBits = newScratchBits - 64;
            }
            else
            {
                m_scratchBits = newScratchBits;
            }

            m_bitsWritten += bits;
        }

        uint8_t * serialize_restrict m_data = nullptr;  ///< The buffer we are writing to. The allocation extends at least 8 bytes past the end of the buffer size, so qword flushes always stay in bounds.
        uint64_t m_scratch = 0;                         ///< The scratch value where we write bits to (right to left). When it fills to 64 bits it is stored to memory as a qword and the bits that spilled past 64 carry over.
        int64_t m_numBits = 0;                          ///< The number of bits in the buffer. This is equivalent to the size of the buffer in bytes multiplied by 8.
        int64_t m_bitsWritten = 0;                      ///< The number of bits written so far.
        int64_t m_wordIndex = 0;                        ///< The current word index. The next word flushed to memory will be at this index in m_data.
        int m_scratchBits = 0;                          ///< The number of valid bits in scratch, in [0,63].
    };

    /**
        Reads bit packed integer values from a buffer.
        Relies on the user reconstructing the exact same set of bit reads as bit writes when the buffer was written. This is an unattributed bitpacked binary stream!
        Implementation: branchless. Each read loads a 64 bit window from the current byte position with memcpy and shifts by the bit remainder.
        There is no scratch state and no refill branch, so reads carry no dependency between calls other than advancing the bit index.
        IMPORTANT: The buffer allocation must extend at least 8 bytes past the end of the packet data, because the reader loads 8 byte windows at byte granularity. The bytes past the end are loaded but never interpreted. This is the same allocation contract as BitWriter.
     */

    class BitReader
    {
    public:

        /// The maximum number of bits a single window load can produce. A window shifted by the worst
        /// case intra-byte offset of 7 holds 57 valid bits, so 56 leaves headroom and mirrors the
        /// writer's MaxWriteBits: both sides split wide values at the same point.
        static constexpr int MaxReadBits = 56;

        BitReader() noexcept = default;

        /**
            Bit reader constructor.
            Any buffer size is supported, as odd sizes naturally occur when packets are read from the network.
            IMPORTANT: The actual buffer allocated for the packet data must extend at least 8 bytes past the end of the data, because the reader loads a 64 bit window from the current byte position, and near the end of the stream that window begins inside the final bytes. The bytes past the end are loaded but never interpreted.
            @param data Pointer to the bitpacked data to read. Does not need to be aligned: the reader loads each window with memcpy, which packet payloads require because they typically start at an unaligned offset once the transport header is stripped.
            @param bytes The number of bytes of bitpacked data to read. Buffer sizes are effectively unlimited, because bit counts are stored in 64 bit signed integers.
            @see BitWriter
         */

        BitReader( const void * serialize_restrict data, int64_t bytes ) noexcept
            : m_data( static_cast<const uint8_t*>( data ) ), m_numBits( bytes * 8 )
        {
            serialize_assert( data );
            serialize_assert( bytes >= 0 );
        }

        void Initialize( const void * serialize_restrict data, int64_t bytes ) noexcept
        {
            serialize_assert( data );
            serialize_assert( bytes >= 0 );
            m_data = static_cast<const uint8_t*>( data );
            m_numBits = bytes * 8;
            m_bitsRead = 0;
        }

        /**
            Would the bit reader would read past the end of the buffer if it read this many bits?
            @param bits The number of bits that would be read.
            @returns True if reading the number of bits would read past the end of the buffer.
         */

        [[nodiscard]] bool WouldReadPastEnd( int bits ) const noexcept
        {
            return m_bitsRead + bits > m_numBits;
        }

        /**
            Read bits from the bit buffer.
            This function will assert in debug builds if this read would read past the end of the buffer.
            In production situations, the higher level ReadStream takes care of checking all packet data and never calling this function if it would read past the end of the buffer.
            @param bits The number of bits to read in [1,32].
            @returns The integer value read in range [0,(1<<bits)-1].
            @see BitReader::WouldReadPastEnd
            @see BitWriter::WriteBits
         */

        [[nodiscard]] serialize_force_inline uint32_t ReadBits( int bits ) noexcept
        {
            serialize_assert( m_data );                 // if this fires, the reader was used before Initialize
            serialize_assert( bits > 0 );
            serialize_assert( bits <= 32 );
            serialize_assert( m_bitsRead + bits <= m_numBits );
            return uint32_t( GetBits( bits ) );
        }

        /**
            Read up to 64 bits from the bit buffer.
            Values up to MaxReadBits (56) wide are read with a single window load; wider values take two.
            The bit stream is LSB-first, so this decodes bytes identical to reading the low 32 bits then
            the high bits as two calls (the classic encoding of 64 bit values).
            @param bits The number of bits to read in [1,64].
            @returns The integer value read in range [0,(1<<bits)-1].
            @see BitWriter::WriteBits64
         */

        [[nodiscard]] serialize_force_inline uint64_t ReadBits64( int bits ) noexcept
        {
            serialize_assert( m_data );                 // if this fires, the reader was used before Initialize
            serialize_assert( bits > 0 );
            serialize_assert( bits <= 64 );
            serialize_assert( m_bitsRead + bits <= m_numBits );
            if ( bits <= MaxReadBits )
            {
                return GetBits( bits );
            }
            // low bits first, then the high remainder: LSB-first makes this byte-identical to any other split
            const uint64_t lo = GetBits( MaxReadBits );
            const uint64_t hi = GetBits( bits - MaxReadBits );
            return ( hi << MaxReadBits ) | lo;
        }

        /**
            Read an align.
            Call this on read to correspond to a WriteAlign call when the bitpacked buffer was written.
            This makes sure we skip ahead to the next aligned byte index. As a safety check, we verify that the padding to next byte is zero bits and return false if that's not the case.
            This will typically abort packet read. Just another safety measure...
            @returns True if we successfully read an align and skipped ahead past zero pad, false otherwise (probably means, no align was written to the stream).
            @see BitWriter::WriteAlign
         */

        [[nodiscard]] bool ReadAlign() noexcept
        {
            const int remainderBits = int( m_bitsRead % 8 );
            if ( remainderBits != 0 )
            {
                const uint64_t value = GetBits( 8 - remainderBits );
                serialize_assert( m_bitsRead % 8 == 0 );
                if ( value != 0 )
                    return false;
            }
            return true;
        }

        /**
            Read bytes from the bitpacked data.
            @see BitWriter::WriteBytes
         */

        void ReadBytes( uint8_t * serialize_restrict data, int64_t bytes ) noexcept
        {
            serialize_assert( m_data );                 // if this fires, the reader was used before Initialize
            serialize_assert( GetAlignBits() == 0 );
            serialize_assert( uint64_t(m_bitsRead) + uint64_t(bytes) * 8 <= uint64_t(m_numBits) );

            // the bit index is byte aligned here (see the align assert), so this is a straight copy
            if ( bytes <= SmallCopyMaxBytes )
                small_copy( data, m_data + ( m_bitsRead >> 3 ), size_t( bytes ) );
            else
                memcpy( data, m_data + ( m_bitsRead >> 3 ), size_t( bytes ) );

            m_bitsRead += bytes * 8;
        }

        /**
            How many align bits would be read, if we were to read an align right now?
            @returns Result in [0,7], where 0 is zero bits required to align (already aligned) and 7 is worst case.
         */

        [[nodiscard]] int GetAlignBits() const noexcept
        {
            return ( 8 - int( m_bitsRead % 8 ) ) % 8;
        }

        /**
            How many bits have we read so far?
            @returns The number of bits read from the bit buffer so far.
         */

        [[nodiscard]] int64_t GetBitsRead() const noexcept
        {
            return m_bitsRead;
        }

        /**
            How many bits are still available to read?
            For example, if the buffer size is 4, we have 32 bits available to read, if we have already read 10 bits then 22 are still available.
            @returns The number of bits available to read.
         */

        [[nodiscard]] int64_t GetBitsRemaining() const noexcept
        {
            return m_numBits - m_bitsRead;
        }

    private:

        /**
            The branchless core all bit reads funnel into. Reads bits in [1,MaxReadBits].
            Loads a 64 bit window at the byte containing the current bit index (up to 7 bytes past the
            last data byte: the allocation contract covers this), shifts by the intra-byte remainder and
            masks to the requested width.
         */

        [[nodiscard]] serialize_force_inline uint64_t GetBits( int bits ) noexcept
        {
            serialize_assert( bits > 0 );
            serialize_assert( bits <= MaxReadBits );

            uint64_t window;
            memcpy( &window, m_data + ( m_bitsRead >> 3 ), sizeof( window ) );
            window = network_to_host( window );

            const uint64_t output = ( window >> ( m_bitsRead & 7 ) ) & ( ( uint64_t(1) << bits ) - 1 );

            m_bitsRead += bits;

            return output;
        }

        const uint8_t * serialize_restrict m_data = nullptr;    ///< The bitpacked data we're reading. The allocation extends at least 8 bytes past the end of the data.
        int64_t m_numBits = 0;                                  ///< Number of bits to read in the buffer.
        int64_t m_bitsRead = 0;                                 ///< Number of bits read from the buffer so far. This is the only state the reader carries between reads.
    };

    /**
        Functionality common to all stream classes.
     */

    class BaseStream
    {
    public:

        /**
            Base stream constructor.
         */

        explicit BaseStream() noexcept = default;

        /**
            Set a context on the stream.
            The context lets you pass data through to your serialize functions, for example lookup tables or min/max ranges needed to read and write values.
            Call BaseStream::GetContext inside your serialize method to retrieve it.
         */

        void SetContext( void * context ) noexcept
        {
            m_context = context;
        }

        /**
            Get the context pointer set on the stream.

            @returns The context pointer. May be NULL.
         */

        [[nodiscard]] void * GetContext() const noexcept
        {
            return m_context;
        }

        /**
            Set an allocator pointer on the stream.
            This can be helpful if you want to perform allocations within serialize functions.
         */

        void SetAllocator( void * allocator ) noexcept
        {
            m_allocator = allocator;
        }

        /**
            Get the allocator pointer set on the stream.

            @returns The allocator pointer. May be NULL.
         */

        [[nodiscard]] void * GetAllocator() const noexcept
        {
            return m_allocator;
        }

    private:

        void * m_context = nullptr;                 ///< The context pointer set on the stream. May be NULL.
        void * m_allocator = nullptr;               ///< The allocator pointer set on the stream. May be NULL.
    };

    /**
        Stream class for writing bitpacked data.
        This class is a wrapper around the bit writer class. Its purpose is to provide unified interface for reading and writing.
        You can determine if you are writing to a stream by checking Stream::IsWriting inside your templated serialize method.
        IsWriting and IsReading are compile time constants, letting the compiler generate optimized serialize functions without the hassle of maintaining separate read and write functions (branch on them with if constexpr for guaranteed zero cost).
        IMPORTANT: Generally, you don't call methods on this class directly. Use the serialize_* macros instead.
        @see BitWriter
     */

    class WriteStream : public BaseStream
    {
    public:

        static constexpr bool IsWriting = true;
        static constexpr bool IsReading = false;

        WriteStream() noexcept = default;

        void Initialize( uint8_t * buffer, int64_t bytes ) noexcept
        {
            m_writer.Initialize( buffer, bytes );
        }

        /**
            Write stream constructor.
            @param buffer The buffer to write to. Does not need to be aligned.
            @param bytes The number of bytes in the buffer. The allocation must extend at least 8 bytes past this size, because the bit writer flushes whole qwords: for buffer sizes that are not a multiple of 8 the final flush lands up to 7 bytes past the end.
         */

        WriteStream( uint8_t * buffer, int64_t bytes ) noexcept : m_writer( buffer, bytes ) {}

        /**
            Serialize an integer (write).
            @param value The integer value in [min,max].
            @param min The minimum value.
            @param max The maximum value.
            @returns Always returns true. All checking is performed by debug asserts only on write.
         */

        serialize_force_inline bool SerializeInteger( int32_t value, int32_t min, int32_t max ) noexcept
        {
            serialize_assert( min < max );
            serialize_assert( value >= min );
            serialize_assert( value <= max );
            const int bits = bits_required( uint32_t(min), uint32_t(max) );
            // subtract in the unsigned domain: value - min overflows signed arithmetic when the range is wider than 2^31
            const uint32_t unsigned_value = uint32_t(value) - uint32_t(min);
            m_writer.WriteBits( unsigned_value, bits );
            return true;
        }

        /**
            Serialize a 64 bit integer (write).
            Ranges that need at most 56 bits are stored with a single operation.
            @param value The integer value in [min,max].
            @param min The minimum value.
            @param max The maximum value.
            @returns Always returns true. All checking is performed by debug asserts only on write.
         */

        serialize_force_inline bool SerializeInteger64( int64_t value, int64_t min, int64_t max ) noexcept
        {
            serialize_assert( min < max );
            serialize_assert( value >= min );
            serialize_assert( value <= max );
            const int bits = bits_required64( uint64_t(min), uint64_t(max) );
            // subtract in the unsigned domain: value - min overflows signed arithmetic when the range is wider than 2^63
            const uint64_t unsigned_value = uint64_t(value) - uint64_t(min);
            m_writer.WriteBits64( unsigned_value, bits );
            return true;
        }

        /**
            Serialize a number of bits (write).
            @param value The unsigned integer value to serialize. Must be in range [0,(1<<bits)-1].
            @param bits The number of bits to write in [1,32].
            @returns Always returns true. All checking is performed by debug asserts on write.
         */

        serialize_force_inline bool SerializeBits( uint32_t value, int bits ) noexcept
        {
            serialize_assert( bits > 0 );
            serialize_assert( bits <= 32 );
            m_writer.WriteBits( value, bits );
            return true;
        }

        /**
            Serialize a number of bits up to 64 (write).
            @param value The unsigned integer value to serialize. Must be in range [0,(1<<bits)-1] (all values are legal at 64 bits).
            @param bits The number of bits to write in [1,64].
            @returns Always returns true. All checking is performed by debug asserts on write.
         */

        serialize_force_inline bool SerializeBits64( uint64_t value, int bits ) noexcept
        {
            serialize_assert( bits > 0 );
            serialize_assert( bits <= 64 );
            m_writer.WriteBits64( value, bits );
            return true;
        }

        /**
            Serialize an array of bytes (write).
            @param data Array of bytes to be written.
            @param bytes The number of bytes to write.
            @returns Always returns true. All checking is performed by debug asserts on write.
         */

        bool SerializeBytes( const uint8_t * data, int64_t bytes ) noexcept
        {
            serialize_assert( data );
            serialize_assert( bytes >= 0 );
            SerializeAlign();
            m_writer.WriteBytes( data, bytes );
            return true;
        }

        /**
            Serialize an align (write).
            @returns Always returns true. All checking is performed by debug asserts on write.
         */

        bool SerializeAlign() noexcept
        {
            m_writer.WriteAlign();
            return true;
        }

        /**
            If we were to write an align right now, how many bits would be required?
            @returns The number of zero pad bits required to achieve byte alignment in [0,7].
         */

        [[nodiscard]] int GetAlignBits() const noexcept
        {
            return m_writer.GetAlignBits();
        }

        /**
            Flush the stream to memory after you finish writing.
            Always call this after you finish writing and before you call WriteStream::GetData, or you'll potentially truncate the last word of data you wrote.
            @see BitWriter::FlushBits
         */

        void Flush() noexcept
        {
            m_writer.FlushBits();
        }

        /**
            Get a pointer to the data written by the stream.
            IMPORTANT: Call WriteStream::Flush before you call this function!
            @returns A pointer to the data written by the stream
         */

        [[nodiscard]] const uint8_t * GetData() const noexcept
        {
            return m_writer.GetData();
        }

        /**
            How many bytes have been written so far?
            @returns Number of bytes written. This is effectively the packet size.
         */

        [[nodiscard]] int64_t GetBytesProcessed() const noexcept
        {
            return m_writer.GetBytesWritten();
        }

        /**
            Get number of bits written so far.
            @returns Number of bits written.
         */

        [[nodiscard]] int64_t GetBitsProcessed() const noexcept
        {
            return m_writer.GetBitsWritten();
        }

    private:

        BitWriter m_writer;                 ///< The bit writer used for all bitpacked write operations.
    };

    /**
        Stream class for reading bitpacked data.
        This class is a wrapper around the bit reader class. Its purpose is to provide unified interface for reading and writing.
        You can determine if you are reading from a stream by checking Stream::IsReading inside your templated serialize method.
        IsWriting and IsReading are compile time constants, letting the compiler generate optimized serialize functions without the hassle of maintaining separate read and write functions (branch on them with if constexpr for guaranteed zero cost).
        IMPORTANT: Generally, you don't call methods on this class directly. Use the serialize_* macros instead.
        @see BitReader
     */

    class ReadStream : public BaseStream
    {
    public:

        static constexpr bool IsWriting = false;
        static constexpr bool IsReading = true;

        ReadStream() noexcept = default;

        void Initialize( const uint8_t * buffer, int64_t bytes ) noexcept
        {
            m_reader.Initialize( buffer, bytes );
        }

        /**
            Read stream constructor.
            @param buffer The buffer to read from.
            @param bytes The number of bytes of packet data to read. IMPORTANT: the underlying allocation must extend at least 8 bytes past the end of the data, because the bit reader loads 64 bit windows at byte granularity. See BitReader for details.
         */

        ReadStream( const uint8_t * buffer, int64_t bytes ) noexcept : m_reader( buffer, bytes ) {}

        /**
            Serialize an integer (read).
            @param value The integer value read is stored here. It is guaranteed to be in [min,max] if this function succeeds.
            @param min The minimum allowed value.
            @param max The maximum allowed value.
            @returns Returns true if the serialize succeeded and the value is in the correct range. False otherwise.
         */

        [[nodiscard]] serialize_force_inline bool SerializeInteger( int32_t & value, int32_t min, int32_t max ) noexcept
        {
            serialize_assert( min < max );
            const int bits = bits_required( uint32_t(min), uint32_t(max) );
            if ( m_reader.WouldReadPastEnd( bits ) )
                return false;
            const uint32_t unsigned_value = m_reader.ReadBits( bits );
            if ( unsigned_value > uint32_t(max) - uint32_t(min) )
                return false;
            // add in the unsigned domain: unsigned_value + min overflows signed arithmetic when the range is wider than 2^31
            value = int32_t( unsigned_value + uint32_t(min) );
            return true;
        }

        /**
            Serialize a 64 bit integer (read).
            Ranges that need at most 56 bits are read with a single window load.
            @param value The integer value read is stored here. It is guaranteed to be in [min,max] if this function succeeds.
            @param min The minimum allowed value.
            @param max The maximum allowed value.
            @returns Returns true if the serialize succeeded and the value is in the correct range. False otherwise.
         */

        [[nodiscard]] serialize_force_inline bool SerializeInteger64( int64_t & value, int64_t min, int64_t max ) noexcept
        {
            serialize_assert( min < max );
            const int bits = bits_required64( uint64_t(min), uint64_t(max) );
            if ( m_reader.WouldReadPastEnd( bits ) )
                return false;
            const uint64_t unsigned_value = m_reader.ReadBits64( bits );
            if ( unsigned_value > uint64_t(max) - uint64_t(min) )
                return false;
            // add in the unsigned domain: unsigned_value + min overflows signed arithmetic when the range is wider than 2^63
            value = int64_t( unsigned_value + uint64_t(min) );
            return true;
        }

        /**
            Serialize a number of bits (read).
            @param value The integer value read is stored here. Will be in range [0,(1<<bits)-1].
            @param bits The number of bits to read in [1,32].
            @returns Returns true if the serialize read succeeded, false otherwise.
         */

        [[nodiscard]] serialize_force_inline bool SerializeBits( uint32_t & value, int bits ) noexcept
        {
            serialize_assert( bits > 0 );
            serialize_assert( bits <= 32 );
            if ( m_reader.WouldReadPastEnd( bits ) )
                return false;
            value = m_reader.ReadBits( bits );
            return true;
        }

        /**
            Serialize a number of bits up to 64 (read).
            @param value The integer value read is stored here. Will be in range [0,(1<<bits)-1].
            @param bits The number of bits to read in [1,64].
            @returns Returns true if the serialize read succeeded, false otherwise.
         */

        [[nodiscard]] serialize_force_inline bool SerializeBits64( uint64_t & value, int bits ) noexcept
        {
            serialize_assert( bits > 0 );
            serialize_assert( bits <= 64 );
            if ( m_reader.WouldReadPastEnd( bits ) )
                return false;
            value = m_reader.ReadBits64( bits );
            return true;
        }

        /**
            Serialize an array of bytes (read).
            @param data Array of bytes to read.
            @param bytes The number of bytes to read.
            @returns Returns true if the serialize read succeeded. False otherwise.
         */

        [[nodiscard]] bool SerializeBytes( uint8_t * data, int64_t bytes ) noexcept
        {
            if ( bytes < 0 )
                return false;
            if ( !SerializeAlign() )
                return false;
            // compare in bytes rather than bits, consistent with the 64 bit bookkeeping
            if ( bytes > m_reader.GetBitsRemaining() / 8 )
                return false;
            m_reader.ReadBytes( data, bytes );
            return true;
        }

        /**
            Serialize an align (read).
            @returns Returns true if the serialize read succeeded. False otherwise.
         */

        [[nodiscard]] bool SerializeAlign() noexcept
        {
            const int alignBits = m_reader.GetAlignBits();
            if ( m_reader.WouldReadPastEnd( alignBits ) )
                return false;
            if ( !m_reader.ReadAlign() )
                return false;
            return true;
        }

        /**
            If we were to read an align right now, how many bits would we need to read?
            @returns The number of zero pad bits required to achieve byte alignment in [0,7].
         */

        [[nodiscard]] int GetAlignBits() const noexcept
        {
            return m_reader.GetAlignBits();
        }

        /**
            Get number of bits read so far.
            @returns Number of bits read.
         */

        [[nodiscard]] int64_t GetBitsProcessed() const noexcept
        {
            return m_reader.GetBitsRead();
        }

        /**
            How many bytes have been read so far?
            @returns Number of bytes read. Effectively this is the number of bits read, rounded up to the next byte where necessary.
         */

        [[nodiscard]] int64_t GetBytesProcessed() const noexcept
        {
            return ( m_reader.GetBitsRead() + 7 ) / 8;
        }

    private:

        BitReader m_reader;             ///< The bit reader used for all bitpacked read operations.
    };

    /**
        Stream class for estimating how many bits it would take to serialize something.
        This class acts like a bit writer (IsWriting is true, IsReading is false), but instead of writing data, it counts how many bits would be written.
        Note that when the serialization includes alignment to byte (see MeasureStream::SerializeAlign), this is an estimate and not an exact measurement. The estimate is guaranteed to be conservative.
        @see BitWriter
        @see BitReader
     */

    class MeasureStream : public BaseStream
    {
    public:

        static constexpr bool IsWriting = true;
        static constexpr bool IsReading = false;

        /**
            Measure stream constructor.
         */

        explicit MeasureStream() noexcept = default;

        /**
            Serialize an integer (measure).
            @param value The integer value to write. Not actually used or checked.
            @param min The minimum value.
            @param max The maximum value.
            @returns Always returns true. All checking is performed by debug asserts only on measure.
         */

        serialize_force_inline bool SerializeInteger( int32_t value, int32_t min, int32_t max ) noexcept
        {
            (void) value;
            serialize_assert( min < max );
            serialize_assert( value >= min );
            serialize_assert( value <= max );
            m_bitsWritten += bits_required( uint32_t(min), uint32_t(max) );
            return true;
        }

        /**
            Serialize a 64 bit integer (measure).
            @param value The integer value to write. Not actually used or checked.
            @param min The minimum value.
            @param max The maximum value.
            @returns Always returns true. All checking is performed by debug asserts only on measure.
         */

        serialize_force_inline bool SerializeInteger64( int64_t value, int64_t min, int64_t max ) noexcept
        {
            (void) value;
            serialize_assert( min < max );
            serialize_assert( value >= min );
            serialize_assert( value <= max );
            m_bitsWritten += bits_required64( uint64_t(min), uint64_t(max) );
            return true;
        }

        /**
            Serialize a number of bits (measure).
            @param value The unsigned integer value to serialize. Not actually used or checked.
            @param bits The number of bits to write in [1,32].
            @returns Always returns true. All checking is performed by debug asserts on measure.
         */

        serialize_force_inline bool SerializeBits( uint32_t value, int bits ) noexcept
        {
            (void) value;
            serialize_assert( bits > 0 );
            serialize_assert( bits <= 32 );
            m_bitsWritten += bits;
            return true;
        }

        /**
            Serialize a number of bits up to 64 (measure).
            @param value The unsigned integer value to serialize. Not actually used or checked.
            @param bits The number of bits to write in [1,64].
            @returns Always returns true. All checking is performed by debug asserts on measure.
         */

        serialize_force_inline bool SerializeBits64( uint64_t value, int bits ) noexcept
        {
            (void) value;
            serialize_assert( bits > 0 );
            serialize_assert( bits <= 64 );
            m_bitsWritten += bits;
            return true;
        }

        /**
            Serialize an array of bytes (measure).
            @param data Array of bytes to 'write'. Not actually used.
            @param bytes The number of bytes to 'write'.
            @returns Always returns true. All checking is performed by debug asserts on measure.
         */

        bool SerializeBytes( const uint8_t * data, int64_t bytes ) noexcept
        {
            (void) data;
            serialize_assert( bytes >= 0 );
            SerializeAlign();
            m_bitsWritten += bytes * 8;
            return true;
        }

        /**
            Serialize an align (measure).
            @returns Always returns true. All checking is performed by debug asserts on measure.
         */

        bool SerializeAlign() noexcept
        {
            m_bitsWritten += GetAlignBits();
            return true;
        }

        /**
            If we were to write an align right now, how many bits would be required?
            IMPORTANT: Since the number of bits required for alignment depends on where an object is written in the final bit stream, this measurement is conservative.
            @returns Always returns worst case 7 bits.
         */

        [[nodiscard]] int GetAlignBits() const noexcept
        {
            return 7;
        }

        /**
            Get number of bits written so far.
            @returns Number of bits written.
         */

        [[nodiscard]] int64_t GetBitsProcessed() const noexcept
        {
            return m_bitsWritten;
        }

        /**
            How many bytes have been written so far?
            @returns Number of bytes written.
         */

        [[nodiscard]] int64_t GetBytesProcessed() const noexcept
        {
            return ( m_bitsWritten + 7 ) / 8;
        }

    private:

        int64_t m_bitsWritten = 0;          ///< Counts the number of bits written.
    };

    /**
        Concept satisfied by WriteStream, ReadStream, MeasureStream and any user defined stream class
        exposing compile time IsWriting/IsReading constants. The internal serialize function templates
        are constrained on this concept, so passing something that is not a stream produces a readable
        compile error instead of a wall of template instantiation failures.
     */

    template <typename S>
    concept StreamType = requires
    {
        { S::IsWriting } -> std::convertible_to<bool>;
        { S::IsReading } -> std::convertible_to<bool>;
    };

    static_assert( StreamType<WriteStream> );
    static_assert( StreamType<ReadStream> );
    static_assert( StreamType<MeasureStream> );

    /*
        Compile time schemas.

        The streams above thread a runtime bit cursor through every field: each read depends on the
        cursor the previous field just advanced, which serializes the whole decode. When the packet
        layout is known at compile time, every field's byte offset, shift and mask is a constant:
        reads become independent constant-offset window loads the CPU executes in parallel, and
        writes become ORs into a handful of registers with constant shifts. Measured on the
        benchmark packet this is ~3x faster to read and ~7x faster to write than the stream path,
        with byte-identical wire output.

        Conditional structure is supported and stays compile time: branch serializes a bool
        and then one of two field lists. The schema becomes a tree of fixed layouts — the remaining
        fields are instantiated once per branch outcome, so every path through the packet keeps
        fully constant offsets. The cost is code size: each conditional roughly doubles the number
        of specialized paths, so keep heavily branching packets on the streams instead.

        Schemas compose, the equivalent of serialize_object: object<&Outer::member, InnerSchema>
        splices the inner schema's fields in place at compile time, with every inner accessor composed
        with the outer member pointer. Nesting is free: the flattened schema is one field list, so
        constant offsets and branch specialization pass through composition unchanged.

        Wire format: identical to the equivalent serialize_* calls, field for field (bytes
        aligns first, exactly like serialize_bytes; object is exactly serialize_object).
        Validation is preserved: reads bounds-check each path segment, range-check ranged integers
        and reject nonzero alignment padding. Writes follow the trust model: debug asserts,
        unchecked in release.

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

            int64_t bytesWritten = BodySchema::Write( buffer, BufferSize, body );
            bool ok = BodySchema::Read( buffer, bytesWritten, body );

        Buffer contract is the same as everywhere else in this library: the allocation must extend
        at least 8 bytes past the end. Members must be addressable (bit-field members have no
        member pointers, so widen them or keep those packets on the streams).
     */

    /// How the schema runner treats a field: an inline bitpacked value, an aligned byte block, or a conditional branch.
    enum class field_kind { leaf, bytes, branch };

    namespace schema_detail
    {
        template <typename M> struct member_traits;
        template <typename C, typename V> struct member_traits<V C::*>
        {
            using object_type = C;
            using value_type = V;
        };

        template <int Bits>
        consteval uint64_t bit_mask()
        {
            return ( Bits == 64 ) ? 0xFFFFFFFFFFFFFFFFULL : ( ( uint64_t(1) << Bits ) - 1 );
        }

        consteval int64_t align_pad( int64_t K )
        {
            return ( 8 - ( K % 8 ) ) % 8;
        }

        // load bits [Pos, Pos + Bits) from the wire at compile time constant offsets. loads stay
        // within the allocation because it extends 8 bytes past the data, same as BitReader.
        template <int64_t Pos, int Bits>
        [[nodiscard]] serialize_force_inline uint64_t get_const( const uint8_t * data ) noexcept
        {
            static_assert( Bits >= 1 && Bits <= 64 );
            constexpr int64_t byte = Pos >> 3;
            constexpr int shift = int( Pos & 7 );
            uint64_t window;
            memcpy( &window, data + byte, 8 );
            window = network_to_host( window );
            if constexpr ( shift + Bits <= 64 )
            {
                return ( window >> shift ) & bit_mask<Bits>();
            }
            else
            {
                uint64_t high;
                memcpy( &high, data + byte + 8, 8 );
                high = network_to_host( high );
                return ( ( window >> shift ) | ( high << ( 64 - shift ) ) ) & bit_mask<Bits>();
            }
        }

        // OR bits [K, K + Bits) into the staging words of the current segment (segment starts at SegBase)
        template <int64_t SegBase, int64_t K, int Bits>
        serialize_force_inline void put_const( uint64_t * words, uint64_t value ) noexcept
        {
            static_assert( Bits >= 1 && Bits <= 64 );
            constexpr int64_t index = ( K - SegBase ) >> 6;
            constexpr int shift = int( ( K - SegBase ) & 63 );
            words[index] |= value << shift;
            if constexpr ( shift + Bits > 64 )
                words[index+1] |= value >> ( 64 - shift );          // shift >= 1 whenever this spills
        }

        // copy a compile time sized block as unrolled 8 byte chunks with an overlapping tail.
        // written without a loop on purpose: GCC recognizes constant-count copy loops and converts
        // them into out of line memcpy calls, which the codegen audit forbids. a memcpy with a
        // constant size of 8 or less always expands inline.
        template <int64_t N, int64_t I = 0>
        serialize_force_inline void copy_const( uint8_t * serialize_restrict dst, const uint8_t * serialize_restrict src ) noexcept
        {
            static_assert( N >= 1 );
            if constexpr ( I + 8 <= N )
            {
                memcpy( dst + I, src + I, 8 );
                copy_const<N, I + 8>( dst, src );
            }
            else if constexpr ( I < N )
            {
                if constexpr ( N >= 8 )
                {
                    memcpy( dst + N - 8, src + N - 8, 8 );          // overlapping tail: in bounds because N >= 8
                }
                else if constexpr ( N >= 4 )
                {
                    memcpy( dst, src, 4 );
                    memcpy( dst + N - 4, src + N - 4, 4 );
                }
                else if constexpr ( N >= 2 )
                {
                    memcpy( dst, src, 2 );
                    memcpy( dst + N - 2, src + N - 2, 2 );
                }
                else
                {
                    dst[0] = src[0];
                }
            }
        }

        // store the segment's staging words [SegBase, K) to the wire and re-zero them for the next
        // segment. unrolled by recursion for the same reason as copy_const: no loop may exist here.
        template <int64_t SegBase, int64_t K, int64_t I = 0>
        serialize_force_inline void flush_words( uint8_t * data, uint64_t * words ) noexcept
        {
            constexpr int64_t numWords = ( K - SegBase + 63 ) / 64;
            if constexpr ( I < numWords )
            {
                const uint64_t word = host_to_network( words[I] );
                memcpy( data + ( SegBase >> 3 ) + I * 8, &word, 8 );
                words[I] = 0;
                flush_words<SegBase, K, I + 1>( data, words );
            }
        }
    }

    /// Serialize an unsigned integral member with a fixed number of bits in [1,64]. Wire identical to serialize_bits.
    template <auto Member, int Bits> struct bits
    {
        static_assert( Bits >= 1 && Bits <= 64 );
        using object_type = typename schema_detail::member_traits<decltype(Member)>::object_type;
        using value_type = typename schema_detail::member_traits<decltype(Member)>::value_type;

        static constexpr field_kind kind = field_kind::leaf;

        static consteval int64_t bits_at( int64_t ) { return Bits; }

        template <int64_t K> static serialize_force_inline bool read( const uint8_t * data, object_type & object ) noexcept
        {
            object.*Member = value_type( schema_detail::get_const<K, Bits>( data ) );
            return true;
        }

        template <int64_t SegBase, int64_t K> static serialize_force_inline void write( uint8_t *, uint64_t * words, const object_type & object ) noexcept
        {
            serialize_assert( Bits == 64 || uint64_t( object.*Member ) <= ( ( uint64_t(1) << Bits ) - 1 ) );
            schema_detail::put_const<SegBase, K, Bits>( words, uint64_t( object.*Member ) );
        }
    };

    /// Serialize a signed 32 bit integer member in [Min,Max] with the minimal number of bits. Wire identical to serialize_int.
    template <auto Member, int32_t Min, int32_t Max> struct int_
    {
        static_assert( Min < Max );
        using object_type = typename schema_detail::member_traits<decltype(Member)>::object_type;

        static constexpr field_kind kind = field_kind::leaf;
        static constexpr int Bits = bits_required_v<Min, Max>;

        static consteval int64_t bits_at( int64_t ) { return Bits; }

        template <int64_t K> static serialize_force_inline bool read( const uint8_t * data, object_type & object ) noexcept
        {
            const uint32_t unsigned_value = uint32_t( schema_detail::get_const<K, Bits>( data ) );
            // a range that exactly fills its bits cannot encode an out of range value: no check exists
            if constexpr ( uint32_t(Max) - uint32_t(Min) != uint32_t( schema_detail::bit_mask<Bits>() ) )
            {
                if ( unsigned_value > uint32_t(Max) - uint32_t(Min) )
                    return false;
            }
            object.*Member = int32_t( unsigned_value + uint32_t(Min) );
            return true;
        }

        template <int64_t SegBase, int64_t K> static serialize_force_inline void write( uint8_t *, uint64_t * words, const object_type & object ) noexcept
        {
            serialize_assert( object.*Member >= Min );
            serialize_assert( object.*Member <= Max );
            schema_detail::put_const<SegBase, K, Bits>( words, uint32_t( object.*Member ) - uint32_t(Min) );
        }
    };

    /// Serialize a signed 64 bit integer member in [Min,Max] with the minimal number of bits. Wire identical to serialize_int64.
    template <auto Member, int64_t Min, int64_t Max> struct int64
    {
        static_assert( Min < Max );
        using object_type = typename schema_detail::member_traits<decltype(Member)>::object_type;

        static constexpr field_kind kind = field_kind::leaf;
        static constexpr int Bits = bits_required_v<Min, Max>;

        static consteval int64_t bits_at( int64_t ) { return Bits; }

        template <int64_t K> static serialize_force_inline bool read( const uint8_t * data, object_type & object ) noexcept
        {
            const uint64_t unsigned_value = schema_detail::get_const<K, Bits>( data );
            // a range that exactly fills its bits cannot encode an out of range value: no check exists
            if constexpr ( uint64_t(Max) - uint64_t(Min) != schema_detail::bit_mask<Bits>() )
            {
                if ( unsigned_value > uint64_t(Max) - uint64_t(Min) )
                    return false;
            }
            object.*Member = int64_t( unsigned_value + uint64_t(Min) );
            return true;
        }

        template <int64_t SegBase, int64_t K> static serialize_force_inline void write( uint8_t *, uint64_t * words, const object_type & object ) noexcept
        {
            serialize_assert( object.*Member >= Min );
            serialize_assert( object.*Member <= Max );
            schema_detail::put_const<SegBase, K, Bits>( words, uint64_t( object.*Member ) - uint64_t(Min) );
        }
    };

    /// Serialize a bool member as one bit. Wire identical to serialize_bool.
    template <auto Member> struct bool_
    {
        using object_type = typename schema_detail::member_traits<decltype(Member)>::object_type;

        static constexpr field_kind kind = field_kind::leaf;

        static consteval int64_t bits_at( int64_t ) { return 1; }

        template <int64_t K> static serialize_force_inline bool read( const uint8_t * data, object_type & object ) noexcept
        {
            object.*Member = schema_detail::get_const<K, 1>( data ) != 0;
            return true;
        }

        template <int64_t SegBase, int64_t K> static serialize_force_inline void write( uint8_t *, uint64_t * words, const object_type & object ) noexcept
        {
            schema_detail::put_const<SegBase, K, 1>( words, ( object.*Member ) ? 1 : 0 );
        }
    };

    /// Serialize a float member as its 32 raw bits. Wire identical to serialize_float.
    template <auto Member> struct float_
    {
        using object_type = typename schema_detail::member_traits<decltype(Member)>::object_type;

        static constexpr field_kind kind = field_kind::leaf;

        static consteval int64_t bits_at( int64_t ) { return 32; }

        template <int64_t K> static serialize_force_inline bool read( const uint8_t * data, object_type & object ) noexcept
        {
            object.*Member = std::bit_cast<float>( uint32_t( schema_detail::get_const<K, 32>( data ) ) );
            return true;
        }

        template <int64_t SegBase, int64_t K> static serialize_force_inline void write( uint8_t *, uint64_t * words, const object_type & object ) noexcept
        {
            schema_detail::put_const<SegBase, K, 32>( words, std::bit_cast<uint32_t>( object.*Member ) );
        }
    };

    /// Serialize a double member as its 64 raw bits. Wire identical to serialize_double.
    template <auto Member> struct double_
    {
        using object_type = typename schema_detail::member_traits<decltype(Member)>::object_type;

        static constexpr field_kind kind = field_kind::leaf;

        static consteval int64_t bits_at( int64_t ) { return 64; }

        template <int64_t K> static serialize_force_inline bool read( const uint8_t * data, object_type & object ) noexcept
        {
            object.*Member = std::bit_cast<double>( schema_detail::get_const<K, 64>( data ) );
            return true;
        }

        template <int64_t SegBase, int64_t K> static serialize_force_inline void write( uint8_t *, uint64_t * words, const object_type & object ) noexcept
        {
            schema_detail::put_const<SegBase, K, 64>( words, std::bit_cast<uint64_t>( object.*Member ) );
        }
    };

    /// Pad with zero bits to the next byte boundary. Wire identical to serialize_align. Reads reject nonzero padding.
    struct align
    {
        static constexpr field_kind kind = field_kind::leaf;

        static consteval int64_t bits_at( int64_t K ) { return schema_detail::align_pad( K ); }

        template <int64_t K, typename T> static serialize_force_inline bool read( const uint8_t * data, T & ) noexcept
        {
            constexpr int pad = int( schema_detail::align_pad( K ) );
            if constexpr ( pad > 0 )
            {
                if ( schema_detail::get_const<K, pad>( data ) != 0 )
                    return false;
            }
            (void) data;
            return true;
        }

        template <int64_t SegBase, int64_t K, typename T> static serialize_force_inline void write( uint8_t *, uint64_t *, const T & ) noexcept
        {
            // nothing to do: the staging words are already zero
        }
    };

    /// Serialize a uint8_t[NumBytes] member. Aligns to a byte boundary first, wire identical to serialize_bytes.
    template <auto Member, int64_t NumBytes> struct bytes
    {
        static_assert( NumBytes >= 1 );
        using object_type = typename schema_detail::member_traits<decltype(Member)>::object_type;

        static constexpr field_kind kind = field_kind::bytes;
        static constexpr int64_t num_bytes = NumBytes;

        static consteval int64_t bits_at( int64_t K ) { return schema_detail::align_pad( K ) + NumBytes * 8; }

        static serialize_force_inline uint8_t * dst( object_type & object ) noexcept { return object.*Member; }
        static serialize_force_inline const uint8_t * src( const object_type & object ) noexcept { return object.*Member; }
    };

    /// A list of fields: the two sides of a branch, or the inner fields of an object.
    template <typename... Fields> struct fields {};

    /**
        Serialize a bool member, then one of two field lists depending on it: if true do this, else do that.
        Wire identical to serialize_bool followed by the equivalent serialize_* calls of the taken side.
        The remaining schema is instantiated once per outcome, so every path keeps compile time constant
        offsets; each branch roughly doubles the generated code for what follows it.
     */
    template <auto Member, typename ThenFields, typename ElseFields> struct branch
    {
        using object_type = typename schema_detail::member_traits<decltype(Member)>::object_type;
        using then_list = ThenFields;
        using else_list = ElseFields;

        static constexpr field_kind kind = field_kind::branch;

        static consteval int64_t bits_at( int64_t ) { return 1; }       // the condition bit; the taken side is accounted per path

        static serialize_force_inline bool get( const object_type & object ) noexcept { return object.*Member; }
        static serialize_force_inline void set( object_type & object, bool value ) noexcept { object.*Member = value; }
    };

    namespace schema_detail
    {
        // nest<Outer, F>: the field F of an inner object, re-targeted at the outer object through
        // the Outer member pointer. this is how object composes: pure accessor composition,
        // no runtime cost. selected by field kind so branches and byte blocks compose too.
        template <auto Outer, typename F, field_kind Kind = F::kind> struct nest;

        template <typename List> struct flatten;
        template <typename List> using flatten_t = typename flatten<List>::type;

        template <auto Outer, typename List> struct wrap;
        template <auto Outer, typename... Fs> struct wrap<Outer, fields<Fs...>> { using type = fields<nest<Outer, Fs>...>; };
        template <auto Outer, typename List> using wrap_t = typename wrap<Outer, List>::type;

        template <auto Outer, typename F> struct nest<Outer, F, field_kind::leaf>
        {
            using object_type = typename member_traits<decltype(Outer)>::object_type;

            static constexpr field_kind kind = field_kind::leaf;

            static consteval int64_t bits_at( int64_t K ) { return F::bits_at( K ); }

            template <int64_t K> static serialize_force_inline bool read( const uint8_t * data, object_type & object ) noexcept
            {
                return F::template read<K>( data, object.*Outer );
            }

            template <int64_t SegBase, int64_t K> static serialize_force_inline void write( uint8_t * data, uint64_t * words, const object_type & object ) noexcept
            {
                F::template write<SegBase, K>( data, words, object.*Outer );
            }
        };

        template <auto Outer, typename F> struct nest<Outer, F, field_kind::bytes>
        {
            using object_type = typename member_traits<decltype(Outer)>::object_type;

            static constexpr field_kind kind = field_kind::bytes;
            static constexpr int64_t num_bytes = F::num_bytes;

            static consteval int64_t bits_at( int64_t K ) { return F::bits_at( K ); }

            static serialize_force_inline uint8_t * dst( object_type & object ) noexcept { return F::dst( object.*Outer ); }
            static serialize_force_inline const uint8_t * src( const object_type & object ) noexcept { return F::src( object.*Outer ); }
        };

        template <auto Outer, typename F> struct nest<Outer, F, field_kind::branch>
        {
            using object_type = typename member_traits<decltype(Outer)>::object_type;
            using then_list = wrap_t<Outer, typename F::then_list>;
            using else_list = wrap_t<Outer, typename F::else_list>;

            static constexpr field_kind kind = field_kind::branch;

            static consteval int64_t bits_at( int64_t ) { return 1; }

            static serialize_force_inline bool get( const object_type & object ) noexcept { return F::get( object.*Outer ); }
            static serialize_force_inline void set( object_type & object, bool value ) noexcept { F::set( object.*Outer, value ); }
        };

        template <typename F, typename List> struct cons;
        template <typename F, typename... Ls> struct cons<F, fields<Ls...>> { using type = fields<F, Ls...>; };

        template <typename A, typename B> struct concat;
        template <typename... As, typename... Bs> struct concat<fields<As...>, fields<Bs...>> { using type = fields<As..., Bs...>; };
    }

    /**
        Splice another schema's fields in place, serializing a member object: the equivalent of serialize_object.
        Inner may be a serialize::schema or a serialize::fields list, and may itself contain branches and
        nested objects. Composition happens entirely at compile time by flattening the inner fields
        with composed member accessors, so nesting costs nothing at runtime.
     */
    template <auto Member, typename Inner> struct object
    {
        using object_type = typename schema_detail::member_traits<decltype(Member)>::object_type;
    };

    namespace schema_detail
    {
        template <typename X> struct field_list_of { using type = X; };            // a plain fields<...> list

        // flatten: replace every object with its inner fields (accessors composed through nest),
        // and flatten the two sides of every branch. the result contains only leaf/bytes/branch fields.
        template <> struct flatten<fields<>> { using type = fields<>; };

        template <typename F, typename... Rest> struct flatten<fields<F, Rest...>>
        {
            using type = typename cons<F, flatten_t<fields<Rest...>>>::type;
        };

        template <auto M, typename A, typename B, typename... Rest> struct flatten<fields<branch<M, A, B>, Rest...>>
        {
            using type = typename cons<branch<M, flatten_t<A>, flatten_t<B>>, flatten_t<fields<Rest...>>>::type;
        };

        template <auto M, typename Inner, typename... Rest> struct flatten<fields<object<M, Inner>, Rest...>>
        {
            using expanded = wrap_t<M, flatten_t<typename field_list_of<Inner>::type>>;
            using type = typename concat<expanded, flatten_t<fields<Rest...>>>::type;
        };

        template <typename T, int64_t SegBase, int64_t K, typename... Fs> struct runner;

        // splice a fields<...> list in front of the remaining fields
        template <typename T, int64_t SegBase, int64_t K, typename List, typename... Rest> struct splice;
        template <typename T, int64_t SegBase, int64_t K, typename... Ls, typename... Rest> struct splice<T, SegBase, K, fields<Ls...>, Rest...>
        {
            using type = runner<T, SegBase, K, Ls..., Rest...>;
        };

        template <typename T, int64_t SegBase, int64_t K> struct runner<T, SegBase, K>
        {
            static consteval int64_t prefix_end() { return K; }
            static consteval int64_t max_end() { return K; }

            static serialize_force_inline bool read( const uint8_t *, int64_t, T & ) noexcept { return true; }

            static serialize_force_inline int64_t write( uint8_t * data, uint64_t * words, const T & ) noexcept
            {
                (void) data; (void) words;
                if constexpr ( K > SegBase )
                    flush_words<SegBase, K>( data, words );
                return ( K + 7 ) / 8;
            }
        };

        template <typename T, int64_t SegBase, int64_t K, typename F, typename... Rest> struct runner<T, SegBase, K, F, Rest...>
        {
            static constexpr int64_t next = K + F::bits_at( K );

            // the bit position where the next bounds check is needed: the end of this fixed run
            // (a branch decides the rest of the layout, so a run ends just after its condition bit)
            static consteval int64_t prefix_end()
            {
                if constexpr ( F::kind == field_kind::branch )
                    return K + 1;
                else
                    return runner<T, SegBase, next, Rest...>::prefix_end();
            }

            // the longest path through the schema, for staging buffer sizing and capacity asserts
            static consteval int64_t max_end()
            {
                if constexpr ( F::kind == field_kind::branch )
                {
                    using then_runner = typename splice<T, SegBase, K + 1, typename F::then_list, Rest...>::type;
                    using else_runner = typename splice<T, SegBase, K + 1, typename F::else_list, Rest...>::type;
                    const int64_t a = then_runner::max_end();
                    const int64_t b = else_runner::max_end();
                    return a > b ? a : b;
                }
                else
                {
                    return runner<T, SegBase, next, Rest...>::max_end();
                }
            }

            static serialize_force_inline bool read( const uint8_t * data, int64_t bytes, T & object ) noexcept
            {
                if constexpr ( F::kind == field_kind::branch )
                {
                    const bool condition = get_const<K, 1>( data ) != 0;
                    F::set( object, condition );
                    if ( condition )
                    {
                        using then_runner = typename splice<T, SegBase, K + 1, typename F::then_list, Rest...>::type;
                        if ( bytes * 8 < then_runner::prefix_end() )
                            return false;
                        return then_runner::read( data, bytes, object );
                    }
                    else
                    {
                        using else_runner = typename splice<T, SegBase, K + 1, typename F::else_list, Rest...>::type;
                        if ( bytes * 8 < else_runner::prefix_end() )
                            return false;
                        return else_runner::read( data, bytes, object );
                    }
                }
                else if constexpr ( F::kind == field_kind::bytes )
                {
                    constexpr int pad = int( align_pad( K ) );
                    if constexpr ( pad > 0 )
                    {
                        if ( get_const<K, pad>( data ) != 0 )
                            return false;
                    }
                    copy_const<F::num_bytes>( F::dst( object ), data + ( ( K + pad ) >> 3 ) );
                    return runner<T, SegBase, next, Rest...>::read( data, bytes, object );
                }
                else
                {
                    if ( !F::template read<K>( data, object ) )
                        return false;
                    return runner<T, SegBase, next, Rest...>::read( data, bytes, object );
                }
            }

            static serialize_force_inline int64_t write( uint8_t * data, uint64_t * words, const T & object ) noexcept
            {
                if constexpr ( F::kind == field_kind::branch )
                {
                    const bool condition = F::get( object );
                    put_const<SegBase, K, 1>( words, condition ? 1 : 0 );
                    if ( condition )
                    {
                        using then_runner = typename splice<T, SegBase, K + 1, typename F::then_list, Rest...>::type;
                        return then_runner::write( data, words, object );
                    }
                    else
                    {
                        using else_runner = typename splice<T, SegBase, K + 1, typename F::else_list, Rest...>::type;
                        return else_runner::write( data, words, object );
                    }
                }
                else if constexpr ( F::kind == field_kind::bytes )
                {
                    // flush the words accumulated so far, copy the bytes directly, start a new segment after them
                    constexpr int pad = int( align_pad( K ) );
                    constexpr int64_t blob_byte = ( K + pad ) >> 3;
                    if constexpr ( K + pad > SegBase )
                        flush_words<SegBase, K + pad>( data, words );
                    copy_const<F::num_bytes>( data + blob_byte, F::src( object ) );
                    return runner<T, next, next, Rest...>::write( data, words, object );
                }
                else
                {
                    F::template write<SegBase, K>( data, words, object );
                    return runner<T, SegBase, next, Rest...>::write( data, words, object );
                }
            }
        };

        template <typename T, typename List> struct runner_of;
        template <typename T, typename... Fs> struct runner_of<T, fields<Fs...>> { using type = runner<T, 0, 0, Fs...>; };

        template <typename List> struct first_field_of;
        template <typename F, typename... Fs> struct first_field_of<fields<F, Fs...>> { using type = F; };
    }

    /**
        A compile time packet schema: a list of fields serialized in order with constant offsets.
        @see bits, int_, int64, bool_, float_, double_, align, bytes, branch, object
     */
    template <typename FirstField, typename... RestFields> struct schema
    {
        /// The flattened field list: object fields spliced in place, only leaf/bytes/branch fields remain.
        using field_list = schema_detail::flatten_t<fields<FirstField, RestFields...>>;

        using object_type = typename schema_detail::first_field_of<field_list>::type::object_type;

    private:

        using root = typename schema_detail::runner_of<object_type, field_list>::type;

    public:

        /// The size in bits of the longest path through the schema.
        static constexpr int64_t MaxBits = root::max_end();

        /// The size in bytes of the longest path through the schema. Size write buffers from this (plus the 8 byte allocation slack).
        static constexpr int64_t MaxBytes = ( MaxBits + 7 ) / 8;

        /**
            Read the schema from bitpacked data (the generated equivalent of a serialize method with a ReadStream).
            @param data The bitpacked data. The allocation must extend at least 8 bytes past the end, as everywhere in this library.
            @param bytes The number of bytes of packet data.
            @param object The object to fill.
            @returns True if every field decoded and validated, false otherwise (bad packets are rejected, exactly like the read stream).
         */

        [[nodiscard]] static bool Read( const uint8_t * data, int64_t bytes, object_type & object ) noexcept
        {
            serialize_assert( data );
            if ( bytes * 8 < root::prefix_end() )
                return false;
            return root::read( data, bytes, object );
        }

        /**
            Write the schema as bitpacked data (the generated equivalent of a serialize method with a WriteStream).
            No flush is needed: all bytes are stored when this returns. Follows the write trust model: debug asserts, unchecked in release.
            @param data The buffer to write to. The allocation must extend at least 8 bytes past the end, as everywhere in this library.
            @param bytes The size of the buffer in bytes. Must be at least MaxBytes (asserted in debug).
            @param object The object to write.
            @returns The number of bytes written.
         */

        static int64_t Write( uint8_t * data, int64_t bytes, const object_type & object ) noexcept
        {
            serialize_assert( data );
            serialize_assert( bytes >= MaxBytes );
            (void) bytes;
            uint64_t words[( MaxBits + 63 ) / 64] = {};
            return root::write( data, words, object );
        }
    };

    namespace schema_detail
    {
        template <typename F1, typename... Fs> struct field_list_of<schema<F1, Fs...>>
        {
            using type = typename schema<F1, Fs...>::field_list;      // already flattened
        };
    }

    /**
        Serialize integer value (read/write/measure).
        This is a helper macro to make writing unified serialize functions easier.
        Serialize macros returns false on error so we don't need to use exceptions for error handling on read. This is an important safety measure because packet data comes from the network and may be malicious.
        IMPORTANT: This macro must be called inside a templated serialize function with template \<typename Stream\>. The serialize method must have a bool return value.
        @param stream The stream object. May be a read, write or measure stream.
        @param value The integer value to serialize in [min,max].
        @param min The minimum value.
        @param max The maximum value.
     */

    #define serialize_int( stream, value, min, max )                    \
        do                                                              \
        {                                                               \
            serialize_assert( (min) < (max) );                          \
            int32_t serialize_temp_int32 = 0;                                    \
            if constexpr ( Stream::IsWriting )                          \
            {                                                           \
                serialize_assert( int64_t(value) >= int64_t(min) );     \
                serialize_assert( int64_t(value) <= int64_t(max) );     \
                serialize_temp_int32 = (int32_t) ( value );                      \
            }                                                           \
            if ( !stream.SerializeInteger( serialize_temp_int32, min, max ) )    \
            {                                                           \
                return false;                                           \
            }                                                           \
            if constexpr ( Stream::IsReading )                          \
            {                                                           \
                value = serialize_temp_int32;                                    \
                if ( int64_t(value) < int64_t(min) ||                   \
                     int64_t(value) > int64_t(max) )                    \
                {                                                       \
                    return false;                                       \
                }                                                       \
            }                                                           \
        } while (0)

    /**
        Serialize a 64 bit integer value (read/write/measure).
        This is a helper macro to make writing unified serialize functions easier.
        The full 64 bit range is supported, and the minimal number of bits for [min,max] is used on the wire.
        Serialize macros returns false on error so we don't need to use exceptions for error handling on read. This is an important safety measure because packet data comes from the network and may be malicious.
        IMPORTANT: This macro must be called inside a templated serialize function with template \<typename Stream\>. The serialize method must have a bool return value.
        @param stream The stream object. May be a read, write or measure stream.
        @param value The 64 bit integer value to serialize in [min,max].
        @param min The minimum value.
        @param max The maximum value.
     */

    #define serialize_int64( stream, value, min, max )                  \
        do                                                              \
        {                                                               \
            serialize_assert( int64_t(min) < int64_t(max) );            \
            int64_t serialize_temp_int64 = 0;                                    \
            if constexpr ( Stream::IsWriting )                          \
            {                                                           \
                serialize_assert( int64_t(value) >= int64_t(min) );     \
                serialize_assert( int64_t(value) <= int64_t(max) );     \
                serialize_temp_int64 = (int64_t) ( value );                      \
            }                                                           \
            if ( !stream.SerializeInteger64( serialize_temp_int64, min, max ) )  \
            {                                                           \
                return false;                                           \
            }                                                           \
            if constexpr ( Stream::IsReading )                          \
            {                                                           \
                value = serialize_temp_int64;                                    \
                if ( int64_t(value) < int64_t(min) ||                   \
                     int64_t(value) > int64_t(max) )                    \
                {                                                       \
                    return false;                                       \
                }                                                       \
            }                                                           \
        } while (0)

    /**
        Serialize bits to the stream (read/write/measure).
        This is a helper macro to make writing unified serialize functions easier.
        Serialize macros returns false on error so we don't need to use exceptions for error handling on read. This is an important safety measure because packet data comes from the network and may be malicious.
        IMPORTANT: This macro must be called inside a templated serialize function with template \<typename Stream\>. The serialize method must have a bool return value.
        @param stream The stream object. May be a read, write or measure stream.
        @param value The unsigned integer value to serialize.
        @param bits The number of bits to serialize in [1,64].
     */

    #define serialize_bits( stream, value, bits )                       \
        do                                                              \
        {                                                               \
            serialize_assert( (bits) > 0 );                             \
            serialize_assert( (bits) <= 64 );                           \
            if ( (bits) <= 32 )                                         \
            {                                                           \
                uint32_t serialize_temp_uint32 = 0;                              \
                if constexpr ( Stream::IsWriting )                      \
                {                                                       \
                    serialize_temp_uint32 = (uint32_t) ( value );                \
                }                                                       \
                if ( !stream.SerializeBits( serialize_temp_uint32, bits ) )      \
                {                                                       \
                    return false;                                       \
                }                                                       \
                if constexpr ( Stream::IsReading )                      \
                {                                                       \
                    value = serialize_temp_uint32;                               \
                }                                                       \
            }                                                           \
            else                                                        \
            {                                                           \
                uint64_t serialize_temp_uint64 = 0;                              \
                if constexpr ( Stream::IsWriting )                      \
                {                                                       \
                    serialize_temp_uint64 = (uint64_t) ( value );                \
                }                                                       \
                if ( !stream.SerializeBits64( serialize_temp_uint64, bits ) )    \
                {                                                       \
                    return false;                                       \
                }                                                       \
                if constexpr ( Stream::IsReading )                      \
                {                                                       \
                    value = serialize_temp_uint64;                               \
                }                                                       \
            }                                                           \
        } while (0)


    /**
        Serialize a boolean value to the stream (read/write/measure).
        This is a helper macro to make writing unified serialize functions easier.
        Serialize macros returns false on error so we don't need to use exceptions for error handling on read. This is an important safety measure because packet data comes from the network and may be malicious.
        IMPORTANT: This macro must be called inside a templated serialize function with template \<typename Stream\>. The serialize method must have a bool return value.
        @param stream The stream object. May be a read, write or measure stream.
        @param value The boolean value to serialize.
     */

    #define serialize_bool( stream, value )                             \
        do                                                              \
        {                                                               \
            uint32_t serialize_temp_bool = 0;                             \
            if constexpr ( Stream::IsWriting )                          \
            {                                                           \
                serialize_temp_bool = ( value ) ? 1 : 0;                  \
            }                                                           \
            serialize_bits( stream, serialize_temp_bool, 1 );             \
            if constexpr ( Stream::IsReading )                          \
            {                                                           \
                value = serialize_temp_bool ? true : false;               \
            }                                                           \
        } while (0)

    template <StreamType Stream> bool serialize_float_internal( Stream & stream, float & value )
    {
        uint32_t int_value = 0;
        if constexpr ( Stream::IsWriting )
        {
            int_value = std::bit_cast<uint32_t>( value );
        }
        const bool result = stream.SerializeBits( int_value, 32 );
        if constexpr ( Stream::IsReading )
        {
            value = std::bit_cast<float>( int_value );
        }
        return result;
    }

    /**
        Serialize floating point value (read/write/measure).
        This is a helper macro to make writing unified serialize functions easier.
        Serialize macros returns false on error so we don't need to use exceptions for error handling on read. This is an important safety measure because packet data comes from the network and may be malicious.
        IMPORTANT: This macro must be called inside a templated serialize function with template \<typename Stream\>. The serialize method must have a bool return value.
        @param stream The stream object. May be a read, write or measure stream.
        @param value The float value to serialize.
     */

    #define serialize_float( stream, value )                                        \
        do                                                                          \
        {                                                                           \
            if ( !serialize::serialize_float_internal( stream, value ) )            \
            {                                                                       \
                return false;                                                       \
            }                                                                       \
        } while (0)

    template <StreamType Stream> bool serialize_compressed_float_internal( Stream & stream, float & value, float min, float max, float res )
    {
        serialize_assert( min < max && res > 0 );

        const float delta = max - min;

        float values = delta / res;

        // clamp so the uint32_t cast below is defined even for pathological delta / res (the !>= form also catches NaN)
        if ( !( values >= 1.0f ) )
        {
            values = 1.0f;
        }
        else if ( values > 4294967040.0f )      // largest float below 2^32
        {
            values = 4294967040.0f;
        }

        const uint32_t maxIntegerValue = uint32_t( std::ceil( values ) );

        const int bits = bits_required( 0, maxIntegerValue );

        uint32_t integerValue = 0;

        if constexpr ( Stream::IsWriting )
        {
            // clamp with the !>= / !<= form so a NaN value is forced into range instead of reaching the uint32 cast below
            float normalizedValue = ( value - min ) / delta;
            if ( !( normalizedValue >= 0.0f ) )
            {
                normalizedValue = 0.0f;
            }
            else if ( !( normalizedValue <= 1.0f ) )
            {
                normalizedValue = 1.0f;
            }
            integerValue = uint32_t( std::floor( normalizedValue * maxIntegerValue + 0.5f ) );
        }

        if ( !stream.SerializeBits( integerValue, bits ) )
        {
            return false;
        }

        if constexpr ( Stream::IsReading )
        {
            if ( integerValue > maxIntegerValue )
            {
                return false;
            }
            const float normalizedValue = integerValue / float( maxIntegerValue );
            value = normalizedValue * delta + min;
        }

        return true;
    }

    /**
        Serialize compressed floating point value (read/write/measure).
        This is a helper macro to make writing unified serialize functions easier.
        Serialize macros returns false on error so we don't need to use exceptions for error handling on read. This is an important safety measure because packet data comes from the network and may be malicious.
        IMPORTANT: This macro must be called inside a templated serialize function with template \<typename Stream\>. The serialize method must have a bool return value.
        @param stream The stream object. May be a read, write or measure stream.
        @param value The float value to serialize.
     */

    #define serialize_compressed_float(stream, value, min, max, res)                                \
    do                                                                                              \
    {                                                                                               \
        if ( !serialize::serialize_compressed_float_internal( stream, value, min, max, res) )       \
        {                                                                                           \
            return false;                                                                           \
        }                                                                                           \
    } while (0)

    template <StreamType Stream> bool serialize_double_internal( Stream & stream, double & value )
    {
        uint64_t int_value = 0;
        if constexpr ( Stream::IsWriting )
        {
            int_value = std::bit_cast<uint64_t>( value );
        }
        if ( !stream.SerializeBits64( int_value, 64 ) )
        {
            return false;
        }
        if constexpr ( Stream::IsReading )
        {
            value = std::bit_cast<double>( int_value );
        }
        return true;
    }

    /**
        Serialize double precision floating point value to the stream (read/write/measure).
        This is a helper macro to make writing unified serialize functions easier.
        Serialize macros returns false on error so we don't need to use exceptions for error handling on read. This is an important safety measure because packet data comes from the network and may be malicious.
        IMPORTANT: This macro must be called inside a templated serialize function with template \<typename Stream\>. The serialize method must have a bool return value.
        @param stream The stream object. May be a read, write or measure stream.
        @param value The double precision floating point value to serialize.
     */

    #define serialize_double( stream, value )                                       \
        do                                                                          \
        {                                                                           \
            if ( !serialize::serialize_double_internal( stream, value ) )           \
            {                                                                       \
                return false;                                                       \
            }                                                                       \
        } while (0)

    template <StreamType Stream> bool serialize_bytes_internal( Stream & stream, uint8_t * data, int64_t bytes )
    {
        return stream.SerializeBytes( data, bytes );
    }

    /**
        Serialize unsigned 8 bit integer (read/write/measure).
        IMPORTANT: This macro must be called inside a templated serialize function with template \<typename Stream\>. The serialize method must have a bool return value.
        @param stream The stream object. May be a read, write or measure stream.
        @param value The unsigned 8 bit integer value.
     */

    #define serialize_uint8( stream, value ) serialize_bits( stream, value, 8 )

    /**
        Serialize unsigned 16 bit integer (read/write/measure).
        IMPORTANT: This macro must be called inside a templated serialize function with template \<typename Stream\>. The serialize method must have a bool return value.
        @param stream The stream object. May be a read, write or measure stream.
        @param value The unsigned 16 bit integer value.
     */

    #define serialize_uint16( stream, value ) serialize_bits( stream, value, 16 )

    /**
        Serialize unsigned 32 bit integer (read/write/measure).
        IMPORTANT: This macro must be called inside a templated serialize function with template \<typename Stream\>. The serialize method must have a bool return value.
        @param stream The stream object. May be a read, write or measure stream.
        @param value The unsigned 32 bit integer value.
     */

    #define serialize_uint32( stream, value ) serialize_bits( stream, value, 32 )

    /**
        Serialize unsigned 64 bit integer (read/write/measure).
        IMPORTANT: This macro must be called inside a templated serialize function with template \<typename Stream\>. The serialize method must have a bool return value.
        @param stream The stream object. May be a read, write or measure stream.
        @param value The unsigned 64 bit integer value.
     */

    #define serialize_uint64( stream, value ) serialize_bits( stream, value, 64 )

    /**
        Serialize an array of bytes to the stream (read/write/measure).
        This is a helper macro to make unified serialize functions easier.
        Serialize macros returns false on error so we don't need to use exceptions for error handling on read. This is an important safety measure because packet data comes from the network and may be malicious.
        IMPORTANT: This macro must be called inside a templated serialize function with template \<typename Stream\>. The serialize method must have a bool return value.
        @param stream The stream object. May be a read, write or measure stream.
        @param data Pointer to the data to be serialized.
        @param bytes The number of bytes to serialize.
     */

    #define serialize_bytes( stream, data, bytes )                                  \
        do                                                                          \
        {                                                                           \
            if ( !serialize::serialize_bytes_internal( stream, data, bytes ) )      \
            {                                                                       \
                return false;                                                       \
            }                                                                       \
        } while (0)

    template <StreamType Stream> bool serialize_string_internal( Stream & stream, char * string, int buffer_size )
    {
        int length = 0;
        if constexpr ( Stream::IsWriting )
        {
            length = int( strlen( string ) );
            serialize_assert( length < buffer_size );
        }
        serialize_int( stream, length, 0, buffer_size - 1 );
        serialize_bytes( stream, (uint8_t*) string, length );
        if constexpr ( Stream::IsReading )
        {
            string[length] = '\0';
        }
        return true;
    }

    // Wire format is 32 bits per character, so streams are compatible between platforms with 2 and 4 byte wchar_t.
    // Code points above 0xFFFF are not translated between UTF-16 and UTF-32 platforms: reading a value that doesn't
    // fit in the local wchar_t fails rather than truncating.

    template <StreamType Stream> bool serialize_wstring_internal( Stream & stream, wchar_t * string, int buffer_size )
    {
        int length = 0;
        if constexpr ( Stream::IsWriting )
        {
            length = int( wcslen( string ) );
            serialize_assert( length < buffer_size );
        }

        serialize_int( stream, length, 0, buffer_size - 1 );
        for ( int i = 0; i < length; i++ )
        {
            uint32_t char_value = 0;
            if constexpr ( Stream::IsWriting )
            {
                char_value = uint32_t( string[i] );
            }
            serialize_bits( stream, char_value, 32 );
            if constexpr ( Stream::IsReading )
            {
                if constexpr ( sizeof(wchar_t) == 2 )
                {
                    if ( char_value > 0xFFFF )
                    {
                        return false;
                    }
                }
                string[i] = wchar_t( char_value );
            }
        }
        if constexpr ( Stream::IsReading )
        {
            string[length] = L'\0';
        }
        return true;
    }

    /**
        Serialize a string to the stream (read/write/measure).
        This is a helper macro to make writing unified serialize functions easier.
        Serialize macros returns false on error so we don't need to use exceptions for error handling on read. This is an important safety measure because packet data comes from the network and may be malicious.
        IMPORTANT: This macro must be called inside a templated serialize function with template \<typename Stream\>. The serialize method must have a bool return value.
        @param stream The stream object. May be a read, write or measure stream.
        @param string The string to serialize write/measure. Pointer to buffer to be filled on read.
        @param buffer_size The size of the string buffer. String with terminating null character must fit into this buffer.
     */

    #define serialize_string( stream, string, buffer_size )                                 \
        do                                                                                  \
        {                                                                                   \
            if ( !serialize::serialize_string_internal( stream, string, buffer_size ) )     \
            {                                                                               \
                return false;                                                               \
            }                                                                               \
        } while (0)

    #define serialize_wstring( stream, string, buffer_size )                                \
        do                                                                                  \
        {                                                                                   \
            if ( !serialize::serialize_wstring_internal( stream, string, buffer_size ) )    \
            {                                                                               \
                return false;                                                               \
            }                                                                               \
        } while (0)

    /**
        Serialize an alignment to the stream (read/write/measure).
        This is a helper macro to make writing unified serialize functions easier.
        Serialize macros returns false on error so we don't need to use exceptions for error handling on read. This is an important safety measure because packet data comes from the network and may be malicious.
        IMPORTANT: This macro must be called inside a templated serialize function with template \<typename Stream\>. The serialize method must have a bool return value.
        @param stream The stream object. May be a read, write or measure stream.
     */

    #define serialize_align( stream )                                                       \
        do                                                                                  \
        {                                                                                   \
            if ( !stream.SerializeAlign() )                                                 \
            {                                                                               \
                return false;                                                               \
            }                                                                               \
        } while (0)

    /**
        Serialize an object to the stream (read/write/measure).
        This is a helper macro to make writing unified serialize functions easier.
        Serialize macros returns false on error so we don't need to use exceptions for error handling on read. This is an important safety measure because packet data comes from the network and may be malicious.
        IMPORTANT: This macro must be called inside a templated serialize function with template \<typename Stream\>. The serialize method must have a bool return value.
        @param stream The stream object. May be a read, write or measure stream.
        @param object The object to serialize. Must have a serialize method on it.
     */

    #define serialize_object( stream, object )                                              \
        do                                                                                  \
        {                                                                                   \
            if ( !( object ).Serialize( stream ) )                                          \
            {                                                                               \
                return false;                                                               \
            }                                                                               \
        }                                                                                   \
        while(0)

    template <StreamType Stream, typename T> bool serialize_int_relative_internal( Stream & stream, T previous, T & current )
    {
        uint32_t difference = 0;
        if constexpr ( Stream::IsWriting )
        {
            serialize_assert( previous < current );
            // subtract in the unsigned domain: current - previous overflows signed arithmetic when the gap is wider than 2^31
            difference = uint32_t( current ) - uint32_t( previous );
        }

        bool oneBit = false;
        if constexpr ( Stream::IsWriting )
        {
            oneBit = difference == 1;
        }
        serialize_bool( stream, oneBit );
        if ( oneBit )
        {
            if constexpr ( Stream::IsReading )
            {
                // reconstruct in the unsigned domain: previous + difference overflows signed arithmetic near the type maximum
                current = T( uint32_t( previous ) + 1 );
            }
            return true;
        }

        bool twoBits = false;
        if constexpr ( Stream::IsWriting )
        {
            twoBits = difference <= 6;
        }
        serialize_bool( stream, twoBits );
        if ( twoBits )
        {
            serialize_int( stream, difference, 2, 6 );
            if constexpr ( Stream::IsReading )
            {
                // reconstruct in the unsigned domain: previous + difference overflows signed arithmetic near the type maximum
                current = T( uint32_t( previous ) + difference );
            }
            return true;
        }

        bool fourBits = false;
        if constexpr ( Stream::IsWriting )
        {
            fourBits = difference <= 23;
        }
        serialize_bool( stream, fourBits );
        if ( fourBits )
        {
            serialize_int( stream, difference, 7, 23 );
            if constexpr ( Stream::IsReading )
            {
                // reconstruct in the unsigned domain: previous + difference overflows signed arithmetic near the type maximum
                current = T( uint32_t( previous ) + difference );
            }
            return true;
        }

        bool eightBits = false;
        if constexpr ( Stream::IsWriting )
        {
            eightBits = difference <= 280;
        }
        serialize_bool( stream, eightBits );
        if ( eightBits )
        {
            serialize_int( stream, difference, 24, 280 );
            if constexpr ( Stream::IsReading )
            {
                // reconstruct in the unsigned domain: previous + difference overflows signed arithmetic near the type maximum
                current = T( uint32_t( previous ) + difference );
            }
            return true;
        }

        bool twelveBits = false;
        if constexpr ( Stream::IsWriting )
        {
            twelveBits = difference <= 4377;
        }
        serialize_bool( stream, twelveBits );
        if ( twelveBits )
        {
            serialize_int( stream, difference, 281, 4377 );
            if constexpr ( Stream::IsReading )
            {
                // reconstruct in the unsigned domain: previous + difference overflows signed arithmetic near the type maximum
                current = T( uint32_t( previous ) + difference );
            }
            return true;
        }

        bool sixteenBits = false;
        if constexpr ( Stream::IsWriting )
        {
            sixteenBits = difference <= 69914;
        }
        serialize_bool( stream, sixteenBits );
        if ( sixteenBits )
        {
            serialize_int( stream, difference, 4378, 69914 );
            if constexpr ( Stream::IsReading )
            {
                // reconstruct in the unsigned domain: previous + difference overflows signed arithmetic near the type maximum
                current = T( uint32_t( previous ) + difference );
            }
            return true;
        }

        uint32_t value = uint32_t( current );
        serialize_bits( stream, value, 32 );
        if constexpr ( Stream::IsReading )
        {
            current = T( value );
            if ( current <= previous )
            {
                return false;
            }
        }

        return true;
    }

    /**
        Serialize an integer value relative to another (read/write/measure).
        This is a helper macro to make writing unified serialize functions easier.
        Serialize macros returns false on error so we don't need to use exceptions for error handling on read. This is an important safety measure because packet data comes from the network and may be malicious.
        IMPORTANT: This macro must be called inside a templated serialize function with template \<typename Stream\>. The serialize method must have a bool return value.
        @param stream The stream object. May be a read, write or measure stream.
        @param previous The previous integer value.
        @param current The current integer value.
     */

    #define serialize_int_relative( stream, previous, current )                             \
        do                                                                                  \
        {                                                                                   \
            if ( !serialize::serialize_int_relative_internal( stream, previous, current ) ) \
            {                                                                               \
                return false;                                                               \
            }                                                                               \
        } while (0)

}

inline void serialize_copy_string( char * dest, const char * source, size_t dest_size )
{
    serialize_assert( dest );
    serialize_assert( source );
    serialize_assert( dest_size >= 1 );
    memset( dest, 0, dest_size );
    for ( size_t i = 0; i < dest_size - 1; i++ )
    {
        if ( source[i] == '\0' )
            break;
        dest[i] = source[i];
    }
}

inline void serialize_copy_wstring( wchar_t * dest, const wchar_t * source, size_t dest_size )
{
    serialize_assert( dest );
    serialize_assert( source );
    serialize_assert( dest_size >= 1 );
    memset( dest, 0, dest_size * sizeof(wchar_t) );
    for ( size_t i = 0; i < dest_size - 1; i++ )
    {
        if ( source[i] == L'\0' )
            break;
        dest[i] = source[i];
    }
}

#if SERIALIZE_ENABLE_TESTS

#include <cstdio>       // printf
#include <cstdlib>      // exit

inline void SerializeCheckHandler( const char * condition,
                                   const char * function,
                                   const char * file,
                                   int line )
{
    printf( "check failed: ( %s ), function %s, file %s, line %d\n", condition, function, file, line );
#ifndef NDEBUG
    #if defined( __GNUC__ )
        __builtin_trap();
    #elif defined( _MSC_VER )
        __debugbreak();
    #endif
#endif
    exit( 1 );
}

#define serialize_check( condition )                                                    \
do                                                                                      \
{                                                                                       \
    if ( !(condition) )                                                                 \
    {                                                                                   \
        SerializeCheckHandler( #condition, __FUNCTION__, __FILE__, __LINE__ );          \
    }                                                                                   \
} while(0)

inline void test_endian()
{
    const uint32_t value = 0x11223344;

    const char * bytes = (const char*) &value;

    if constexpr ( serialize::little_endian )
    {
        serialize_check( bytes[0] == 0x44 );
        serialize_check( bytes[1] == 0x33 );
        serialize_check( bytes[2] == 0x22 );
        serialize_check( bytes[3] == 0x11 );
    }
    else
    {
        serialize_check( bytes[3] == 0x44 );
        serialize_check( bytes[2] == 0x33 );
        serialize_check( bytes[1] == 0x22 );
        serialize_check( bytes[0] == 0x11 );
    }
}

inline void test_bitpacker()
{
    const int BufferSize = 256;

    uint8_t buffer[BufferSize + 8];         // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

    serialize::BitWriter writer( buffer, BufferSize );

    serialize_check( writer.GetData() == buffer );
    serialize_check( writer.GetBitsWritten() == 0 );
    serialize_check( writer.GetBytesWritten() == 0 );
    serialize_check( writer.GetBitsAvailable() == BufferSize * 8 );

    writer.WriteBits( 0, 1 );
    writer.WriteBits( 1, 1 );
    writer.WriteBits( 10, 8 );
    writer.WriteBits( 255, 8 );
    writer.WriteBits( 1000, 10 );
    writer.WriteBits( 50000, 16 );
    writer.WriteBits( 9999999, 32 );
    writer.FlushBits();

    const int bitsWritten = 1 + 1 + 8 + 8 + 10 + 16 + 32;

    serialize_check( writer.GetBytesWritten() == 10 );
    serialize_check( writer.GetBitsWritten() == bitsWritten );
    serialize_check( writer.GetBitsAvailable() == BufferSize * 8 - bitsWritten );

    const int bytesWritten = int( writer.GetBytesWritten() );

    serialize_check( bytesWritten == 10 );

    memset( buffer + bytesWritten, 0, sizeof(buffer) - bytesWritten );

    serialize::BitReader reader( buffer, bytesWritten );

    serialize_check( reader.GetBitsRead() == 0 );
    serialize_check( reader.GetBitsRemaining() == bytesWritten * 8 );

    uint32_t a = reader.ReadBits( 1 );
    uint32_t b = reader.ReadBits( 1 );
    uint32_t c = reader.ReadBits( 8 );
    uint32_t d = reader.ReadBits( 8 );
    uint32_t e = reader.ReadBits( 10 );
    uint32_t f = reader.ReadBits( 16 );
    uint32_t g = reader.ReadBits( 32 );

    serialize_check( a == 0 );
    serialize_check( b == 1 );
    serialize_check( c == 10 );
    serialize_check( d == 255 );
    serialize_check( e == 1000 );
    serialize_check( f == 50000 );
    serialize_check( g == 9999999 );

    serialize_check( reader.GetBitsRead() == bitsWritten );
    serialize_check( reader.GetBitsRemaining() == bytesWritten * 8 - bitsWritten );
}

inline void test_bits64()
{
    // WriteBits64/ReadBits64 must produce and decode bytes identical to the classic lo/hi 32 bit split,
    // for every width in (32,64]. this pins the wire compatibility of the single-store 64 bit paths.
    for ( int bits = 33; bits <= 64; bits++ )
    {
        const uint64_t value = 0x9E3779B97F4A7C15ULL >> ( 64 - bits );

        uint8_t buffer_a[32 + 8] = { 0 };       // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader
        serialize::BitWriter writer_a( buffer_a, 32 );
        writer_a.WriteBits( 1, 3 );             // start unaligned so the intra-byte shift is exercised
        writer_a.WriteBits64( value, bits );
        writer_a.FlushBits();

        uint8_t buffer_b[32 + 8] = { 0 };
        serialize::BitWriter writer_b( buffer_b, 32 );
        writer_b.WriteBits( 1, 3 );
        writer_b.WriteBits( uint32_t( value & 0xFFFFFFFF ), 32 );       // the classic encoding of 64 bit values
        writer_b.WriteBits( uint32_t( value >> 32 ), bits - 32 );
        writer_b.FlushBits();

        serialize_check( writer_a.GetBitsWritten() == writer_b.GetBitsWritten() );
        serialize_check( memcmp( buffer_a, buffer_b, sizeof( buffer_a ) ) == 0 );

        serialize::BitReader reader( buffer_a, writer_a.GetBytesWritten() );
        const uint32_t three = reader.ReadBits( 3 );
        serialize_check( three == 1 );
        const uint64_t read_value = reader.ReadBits64( bits );
        serialize_check( read_value == value );

        serialize::BitReader split_reader( buffer_a, writer_a.GetBytesWritten() );
        const uint32_t split_three = split_reader.ReadBits( 3 );
        serialize_check( split_three == 1 );
        const uint32_t lo = split_reader.ReadBits( 32 );
        const uint32_t hi = split_reader.ReadBits( bits - 32 );
        serialize_check( ( ( uint64_t(hi) << 32 ) | lo ) == value );
    }

    // widths [1,56] take the single window path on both sides and must round trip
    for ( int bits = 1; bits <= 56; bits++ )
    {
        const uint64_t value = 0xF0E1D2C3B4A59687ULL >> ( 64 - bits );

        uint8_t buffer[16 + 8] = { 0 };         // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader
        serialize::BitWriter writer( buffer, 16 );
        writer.WriteBits64( value, bits );
        writer.FlushBits();

        serialize::BitReader reader( buffer, writer.GetBytesWritten() );
        const uint64_t read_value = reader.ReadBits64( bits );
        serialize_check( read_value == value );
    }

    // a maximum width put starting at every intra-byte offset, followed by more data. this pins the
    // scratch spill/carry logic at every offset: a spilling 56 bit put must recover exactly the bits
    // that fell past 64, and nothing written afterwards may be disturbed.
    for ( int offset = 1; offset <= 7; offset++ )
    {
        const uint64_t big = 0x00AABBCCDDEEFF11ULL & ( ( uint64_t(1) << serialize::BitWriter::MaxWriteBits ) - 1 );

        uint8_t buffer[32 + 8] = { 0 };        // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader
        serialize::BitWriter writer( buffer, 32 );
        writer.WriteBits( ( 1u << offset ) - 1, offset );
        writer.WriteBits64( big, serialize::BitWriter::MaxWriteBits );
        writer.WriteBits( 0xAB, 8 );
        writer.WriteBits64( 0x123456789ABCDEF0ULL, 64 );
        writer.FlushBits();

        serialize::BitReader reader( buffer, writer.GetBytesWritten() );
        const uint32_t head = reader.ReadBits( offset );
        serialize_check( head == ( 1u << offset ) - 1 );
        const uint64_t read_big = reader.ReadBits64( serialize::BitReader::MaxReadBits );
        serialize_check( read_big == big );
        const uint32_t marker = reader.ReadBits( 8 );
        serialize_check( marker == 0xAB );
        const uint64_t tail = reader.ReadBits64( 64 );
        serialize_check( tail == 0x123456789ABCDEF0ULL );
    }
}

inline void test_bits_required()
{
    serialize_check( serialize::bits_required( 0, 0 ) == 0 );
    serialize_check( serialize::bits_required( 0, 1 ) == 1 );
    serialize_check( serialize::bits_required( 0, 2 ) == 2 );
    serialize_check( serialize::bits_required( 0, 3 ) == 2 );
    serialize_check( serialize::bits_required( 0, 4 ) == 3 );
    serialize_check( serialize::bits_required( 0, 5 ) == 3 );
    serialize_check( serialize::bits_required( 0, 6 ) == 3 );
    serialize_check( serialize::bits_required( 0, 7 ) == 3 );
    serialize_check( serialize::bits_required( 0, 8 ) == 4 );
    serialize_check( serialize::bits_required( 0, 255 ) == 8 );
    serialize_check( serialize::bits_required( 0, 65535 ) == 16 );
    serialize_check( serialize::bits_required( 0, 4294967295 ) == 32 );

    // constexpr: these fold to compile time constants
    static_assert( serialize::bits_required( 0, 255 ) == 8 );
    static_assert( serialize::bits_required_v<0, 255> == 8 );
    static_assert( serialize::bits_required_v<-100, +100> == 8 );
    static_assert( serialize::bits_required_v<-5000000000LL, +5000000000LL> == 34 );
}

inline void test_bits_required64()
{
    serialize_check( serialize::bits_required64( 0, 0 ) == 0 );
    serialize_check( serialize::bits_required64( 0, 1 ) == 1 );
    serialize_check( serialize::bits_required64( 0, 255 ) == 8 );
    serialize_check( serialize::bits_required64( 0, 4294967295ULL ) == 32 );
    serialize_check( serialize::bits_required64( 0, 4294967296ULL ) == 33 );
    serialize_check( serialize::bits_required64( 0, ( 1ULL << 40 ) ) == 41 );
    serialize_check( serialize::bits_required64( 0, 0xFFFFFFFFFFFFFFFFULL ) == 64 );
    serialize_check( serialize::bits_required64( uint64_t(INT64_MIN), uint64_t(INT64_MAX) ) == 64 );
    serialize_check( serialize::bits_required64( uint64_t(-5000000000LL), uint64_t(+5000000000LL) ) == 34 );

    // constexpr: these fold to compile time constants
    static_assert( serialize::bits_required64( 0, 4294967296ULL ) == 33 );
}

inline void test_zigzag()
{
    serialize_check( serialize::signed_to_unsigned( 0 ) == 0 );
    serialize_check( serialize::signed_to_unsigned( -1 ) == 1 );
    serialize_check( serialize::signed_to_unsigned( +1 ) == 2 );
    serialize_check( serialize::signed_to_unsigned( -2 ) == 3 );
    serialize_check( serialize::signed_to_unsigned( +2 ) == 4 );
    serialize_check( serialize::signed_to_unsigned( INT32_MAX ) == 0xFFFFFFFE );
    serialize_check( serialize::signed_to_unsigned( INT32_MIN ) == 0xFFFFFFFF );

    serialize_check( serialize::unsigned_to_signed( 0 ) == 0 );
    serialize_check( serialize::unsigned_to_signed( 1 ) == -1 );
    serialize_check( serialize::unsigned_to_signed( 2 ) == +1 );
    serialize_check( serialize::unsigned_to_signed( 3 ) == -2 );
    serialize_check( serialize::unsigned_to_signed( 4 ) == +2 );
    serialize_check( serialize::unsigned_to_signed( 0xFFFFFFFE ) == INT32_MAX );
    serialize_check( serialize::unsigned_to_signed( 0xFFFFFFFF ) == INT32_MIN );

    const int32_t values[] = { 0, -1, +1, -2, +2, 12345, -12345, INT32_MAX, INT32_MIN };

    for ( int i = 0; i < (int) ( sizeof(values) / sizeof(values[0]) ); i++ )
    {
        serialize_check( serialize::unsigned_to_signed( serialize::signed_to_unsigned( values[i] ) ) == values[i] );
    }

    // constexpr: zig-zag folds at compile time
    static_assert( serialize::signed_to_unsigned( -2 ) == 3 );
    static_assert( serialize::unsigned_to_signed( 3 ) == -2 );
}

const int MaxItems = 11;

struct TestData
{
    TestData()
    {
        memset( this, 0, sizeof( TestData ) );
    }

    int a,b,c;
    uint32_t d : 8;
    uint32_t e : 8;
    uint32_t f : 8;
    bool g;
    int numItems;
    int items[MaxItems];
    float float_value;
    float compressed_float_value;
    double double_value;
    uint8_t uint8_value;
    uint16_t uint16_value;
    uint32_t uint32_value;
    uint64_t uint64_value;
    int int_relative;
    int64_t int64_full;
    int64_t int64_range;
    uint8_t bytes[17];
    char string[256];
    wchar_t wstring[256];
};

struct TestContext
{
    int min;
    int max;
};

struct TestObject
{
    TestData data;

    void Init()
    {
        data.a = 1;
        data.b = -2;
        data.c = 150;
        data.d = 55;
        data.e = 255;
        data.f = 127;
        data.g = true;

        data.numItems = MaxItems / 2;
        for ( int i = 0; i < data.numItems; ++i )
            data.items[i] = i + 10;

        data.compressed_float_value = 2.13f;
        data.float_value = 3.1415926f;
        data.double_value = 1 / 3.0;
        data.uint8_value = 123;
        data.uint16_value = 0x1234;
        data.uint32_value = 0x12345678;
        data.uint64_value = 0x1234567898765432L;
        data.int_relative = 5;
        data.int64_full = -123456789012345LL;
        data.int64_range = 4123456789LL;

        for ( int i = 0; i < (int) sizeof( data.bytes ); ++i )
            data.bytes[i] = (uint8_t) ( i + 5 ) * 13;

        serialize_copy_string( data.string, "hello world!", sizeof(data.string) - 1 );

        serialize_copy_wstring( data.wstring, L"привіт, світ!", sizeof(data.wstring) / sizeof(wchar_t) - 1 );
    }

    template <typename Stream> bool Serialize( Stream & stream )
    {
        const TestContext & context = *(const TestContext*) stream.GetContext();

        serialize_int( stream, data.a, context.min, context.max );
        serialize_int( stream, data.b, context.min, context.max );

        serialize_int( stream, data.c, -100, 10000 );

        serialize_bits( stream, data.d, 6 );
        serialize_bits( stream, data.e, 8 );
        serialize_bits( stream, data.f, 7 );

        serialize_align( stream );

        serialize_bool( stream, data.g );

        serialize_int( stream, data.numItems, 0, MaxItems - 1 );
        for ( int i = 0; i < data.numItems; ++i )
            serialize_bits( stream, data.items[i], 8 );

        serialize_float( stream, data.float_value );

        serialize_compressed_float( stream, data.compressed_float_value, 0, 10, 0.01 );

        serialize_double( stream, data.double_value );

        serialize_uint8( stream, data.uint8_value );
        serialize_uint16( stream, data.uint16_value );
        serialize_uint32( stream, data.uint32_value );
        serialize_uint64( stream, data.uint64_value );

        serialize_int_relative( stream, data.a, data.int_relative );

        serialize_int64( stream, data.int64_full, INT64_MIN, INT64_MAX );
        serialize_int64( stream, data.int64_range, -5000000000LL, +5000000000LL );

        serialize_bytes( stream, data.bytes, sizeof( data.bytes ) );

        serialize_string( stream, data.string, sizeof( data.string ) );
        serialize_wstring( stream, data.wstring, sizeof( data.wstring ) / sizeof( wchar_t ) );

        return true;
    }

    bool operator == ( const TestObject & other ) const
    {
        return memcmp( &data, &other.data, sizeof( TestData ) ) == 0;
    }

    bool operator != ( const TestObject & other ) const
    {
        return ! ( *this == other );
    }
};

inline void test_serialize()
{
    const int BufferSize = 1024;

    uint8_t buffer[BufferSize + 8];         // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

    TestContext context;
    context.min = -10;
    context.max = +10;

    serialize::WriteStream writeStream( buffer, BufferSize );

    TestObject writeObject;
    writeObject.Init();
    writeStream.SetContext( &context );
    writeObject.Serialize( writeStream );
    writeStream.Flush();

    const int bytesWritten = int( writeStream.GetBytesProcessed() );

    memset( buffer + bytesWritten, 0, sizeof(buffer) - bytesWritten );

    TestObject readObject;
    serialize::ReadStream readStream( buffer, bytesWritten );
    readStream.SetContext( &context );
    readObject.Serialize( readStream );

    serialize_check( readObject == writeObject );
}

// Separate read and write functions, without the classic read_*/write_* macro families: alias the
// stream type as Stream and the unified serialize_* macros work unchanged, with the compile time
// IsWriting/IsReading branches folded and reads validated. The values below are checked one by one
// as they decode, because that is what a hand-written read function looks like.

bool WriteFunction( serialize::WriteStream & stream )
{
    using Stream = serialize::WriteStream;

    uint32_t bits4 = 13;
    serialize_bits( stream, bits4, 4 );

    bool flag = true;
    serialize_bool( stream, flag );

    uint32_t value_uint8 = 255;
    serialize_uint8( stream, value_uint8 );

    uint32_t value_uint16 = 65535;
    serialize_uint16( stream, value_uint16 );

    uint32_t value_uint32 = 0xFFFFFFFF;
    serialize_uint32( stream, value_uint32 );

    uint64_t value_uint64 = 0xFFFFFFFFFFFFFFFFULL;
    serialize_uint64( stream, value_uint64 );

    int value_int = 55;
    serialize_int( stream, value_int, 10, 90 );

    int64_t value_int64 = -50000000001LL;
    serialize_int64( stream, value_int64, -60000000000LL, 60000000000LL );

    float value_float = 100.0f;
    serialize_float( stream, value_float );

    double value_double = 1000000000.0;
    serialize_double( stream, value_double );

    uint8_t data[5] = { 1, 2, 3, 4, 5 };
    serialize_bytes( stream, data, 5 );

    char string[10];
    serialize_copy_string( string, "hello", sizeof( string ) );
    serialize_string( stream, string, (int) sizeof( string ) );

    wchar_t wstring[20];
    serialize_copy_wstring( wstring, L"привіт", sizeof( wstring ) / sizeof( wchar_t ) );
    serialize_wstring( stream, wstring, (int) ( sizeof( wstring ) / sizeof( wchar_t ) ) );

    serialize_align( stream );

    TestContext context;
    context.min = -10;
    context.max = +10;

    stream.SetContext( &context );

    TestObject object;
    object.Init();

    serialize_object( stream, object );

    int relative = 105;
    serialize_int_relative( stream, 100, relative );

    return true;
}

bool ReadFunction( serialize::ReadStream & readStream )
{
    using Stream = serialize::ReadStream;

    {
        uint32_t value;
        serialize_bits( readStream, value, 4 );
        serialize_check( value == 13 );
    }

    {
        bool value;
        serialize_bool( readStream, value );
        serialize_check( value == true );
    }

    {
        uint8_t value;
        serialize_uint8( readStream, value );
        serialize_check( value == 255 );
    }

    {
        uint16_t value;
        serialize_uint16( readStream, value );
        serialize_check( value == 65535 );
    }

    {
        uint32_t value;
        serialize_uint32( readStream, value );
        serialize_check( value == 0xFFFFFFFF );
    }

    {
        uint64_t value;
        serialize_uint64( readStream, value );
        serialize_check( value == 0xFFFFFFFFFFFFFFFFULL );      // i am very full
    }

    {
        int value;
        serialize_int( readStream, value, 10, 90 );
        serialize_check( value == 55 );
    }

    {
        int64_t value;
        serialize_int64( readStream, value, -60000000000LL, 60000000000LL );
        serialize_check( value == -50000000001LL );
    }

    {
        float value;
        serialize_float( readStream, value );
        serialize_check( value == 100.0f );
    }

    {
        double value;
        serialize_double( readStream, value );
        serialize_check( value == 1000000000.0 );
    }

    {
        uint8_t value[5];
        serialize_bytes( readStream, value, 5 );
        serialize_check( value[0] == 1 );
        serialize_check( value[1] == 2 );
        serialize_check( value[2] == 3 );
        serialize_check( value[3] == 4 );
        serialize_check( value[4] == 5 );
    }

    {
        char string[10];
        serialize_string( readStream, string, (int) sizeof( string ) );
        serialize_check( string[0] == 'h' );
        serialize_check( string[1] == 'e' );
        serialize_check( string[2] == 'l' );
        serialize_check( string[3] == 'l' );
        serialize_check( string[4] == 'o' );
        serialize_check( string[5] == '\0' );
    }

    {
        wchar_t wstring[20];
        serialize_wstring( readStream, wstring, (int) ( sizeof( wstring ) / sizeof( wchar_t ) ) );
        // explicit code points rather than cyrillic literals: serialize_check stringizes its
        // condition into a narrow string, which warns as C4566 on MSVC with a western code page
        serialize_check( wstring[0] == 0x043F );        // 'п'
        serialize_check( wstring[1] == 0x0440 );        // 'р'
        serialize_check( wstring[2] == 0x0438 );        // 'и'
        serialize_check( wstring[3] == 0x0432 );        // 'в'
        serialize_check( wstring[4] == 0x0456 );        // 'і'
        serialize_check( wstring[5] == 0x0442 );        // 'т'
    }

    serialize_align( readStream );

    TestContext context;
    context.min = -10;
    context.max = +10;

    readStream.SetContext( &context );
    {
        TestObject expectedObject;
        expectedObject.Init();

        TestObject readObject;

        serialize_object( readStream, readObject );

        serialize_check( readObject == expectedObject );
    }

    {
        int value = 0;
        serialize_int_relative( readStream, 100, value );
        serialize_check( value == 105 );
    }

    return true;
}

inline void test_read_write()
{
    const int BufferSize = 10 * 1024;

    uint8_t buffer[BufferSize + 8];         // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

    int bytesWritten = 0;

    // write to the buffer
    {
        serialize::WriteStream writeStream;
        writeStream.Initialize( buffer, BufferSize );
        serialize_check( WriteFunction( writeStream ) == true );
        writeStream.Flush();
        bytesWritten = int( writeStream.GetBytesProcessed() );
        memset( buffer + bytesWritten, 0, sizeof(buffer) - bytesWritten );
    }

    // read from the buffer
    {
        serialize::ReadStream readStream;
        readStream.Initialize( buffer, bytesWritten );
        serialize_check( ReadFunction( readStream ) );
    }
}

inline void test_serialize_integer_validation()
{
    // bits_required(0,5) is 3 bits, so a malicious packet can encode 6 or 7. reads must reject values above max.
    uint8_t buffer[8 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

    serialize::WriteStream writeStream( buffer, 8 );
    uint32_t out_of_range = 7;
    writeStream.SerializeBits( out_of_range, 3 );
    writeStream.Flush();

    serialize::ReadStream readStream( buffer, 4 );
    int32_t value = 0;
    serialize_check( readStream.SerializeInteger( value, 0, 5 ) == false );
}

inline void test_serialize_integer_full_range()
{
    // ranges wider than 2^31 overflow if [min,max] arithmetic is done signed (undefined behavior)
    const int32_t values[] = { INT32_MIN, INT32_MIN + 1, -1, 0, +1, INT32_MAX - 1, INT32_MAX };

    for ( int i = 0; i < (int) ( sizeof(values) / sizeof(values[0]) ); i++ )
    {
        uint8_t buffer[8 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

        serialize::WriteStream writeStream( buffer, 8 );
        serialize_check( writeStream.SerializeInteger( values[i], INT32_MIN, INT32_MAX ) == true );
        writeStream.Flush();

        serialize::ReadStream readStream( buffer, 8 );
        int32_t value = 0;
        serialize_check( readStream.SerializeInteger( value, INT32_MIN, INT32_MAX ) == true );
        serialize_check( value == values[i] );
    }

    {
        uint8_t buffer[8 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

        serialize::WriteStream writeStream( buffer, 8 );
        serialize_check( writeStream.SerializeInteger( 1000000000, -2000000000, 2000000000 ) == true );
        writeStream.Flush();

        serialize::ReadStream readStream( buffer, 8 );
        int32_t value = 0;
        serialize_check( readStream.SerializeInteger( value, -2000000000, 2000000000 ) == true );
        serialize_check( value == 1000000000 );
    }
}

inline void test_serialize_int64_full_range()
{
    // ranges wider than 2^63 overflow if [min,max] arithmetic is done signed (undefined behavior)
    {
        const int64_t values[] = { INT64_MIN, INT64_MIN + 1, -1, 0, +1, INT64_MAX - 1, INT64_MAX };

        for ( int i = 0; i < (int) ( sizeof(values) / sizeof(values[0]) ); i++ )
        {
            uint8_t buffer[16 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

            serialize::WriteStream writeStream( buffer, 16 );
            serialize_check( writeStream.SerializeInteger64( values[i], INT64_MIN, INT64_MAX ) == true );
            writeStream.Flush();

            serialize::ReadStream readStream( buffer, 16 );
            int64_t value = 0;
            serialize_check( readStream.SerializeInteger64( value, INT64_MIN, INT64_MAX ) == true );
            serialize_check( value == values[i] );
        }
    }

    // ranges spanning more than 32 bits take the single window path (bits <= 56 is one store now)
    {
        const int64_t min = -5000000000LL;
        const int64_t max = +5000000000LL;
        const int64_t values[] = { min, min + 1, -1, 0, +1, 4123456789LL, max - 1, max };

        for ( int i = 0; i < (int) ( sizeof(values) / sizeof(values[0]) ); i++ )
        {
            uint8_t buffer[16 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

            serialize::WriteStream writeStream( buffer, 16 );
            serialize_check( writeStream.SerializeInteger64( values[i], min, max ) == true );
            writeStream.Flush();

            serialize::ReadStream readStream( buffer, 16 );
            int64_t value = 0;
            serialize_check( readStream.SerializeInteger64( value, min, max ) == true );
            serialize_check( value == values[i] );
        }
    }

    // ranges wider than 56 bits split into two stores and must still round trip
    {
        const int64_t min = INT64_MIN / 2;
        const int64_t max = INT64_MAX / 2;                       // 63 bit range
        const int64_t values[] = { min, min + 1, -1, 0, +1, 4123456789LL, max - 1, max };

        for ( int i = 0; i < (int) ( sizeof(values) / sizeof(values[0]) ); i++ )
        {
            uint8_t buffer[16 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

            serialize::WriteStream writeStream( buffer, 16 );
            serialize_check( writeStream.SerializeInteger64( values[i], min, max ) == true );
            writeStream.Flush();

            serialize::ReadStream readStream( buffer, 16 );
            int64_t value = 0;
            serialize_check( readStream.SerializeInteger64( value, min, max ) == true );
            serialize_check( value == values[i] );
        }
    }

    // small ranges use the single dword path and the minimal number of bits
    {
        uint8_t buffer[8 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

        serialize::WriteStream writeStream( buffer, 8 );
        serialize_check( writeStream.SerializeInteger64( 55, -100, +100 ) == true );
        writeStream.Flush();

        serialize_check( writeStream.GetBitsProcessed() == 8 );        // bits_required64(-100,100) == 8, same as the 32 bit path

        serialize::ReadStream readStream( buffer, 8 );
        int64_t value = 0;
        serialize_check( readStream.SerializeInteger64( value, -100, +100 ) == true );
        serialize_check( value == 55 );
    }
}

inline void test_serialize_int64_validation()
{
    // a malicious packet can smuggle an out of range value into the bit headroom. reads must reject it.
    {
        uint8_t buffer[16 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

        serialize::WriteStream writeStream( buffer, 16 );
        const uint64_t out_of_range = ( 1ULL << 34 ) + 5;               // range [0, 2^34] is 35 bits, so values above 2^34 fit in the headroom
        writeStream.SerializeBits64( out_of_range, 35 );
        writeStream.Flush();

        serialize::ReadStream readStream( buffer, 16 );
        int64_t value = 0;
        serialize_check( readStream.SerializeInteger64( value, 0, int64_t( 1ULL << 34 ) ) == false );
    }

    // reads past the end of the buffer must fail cleanly
    {
        uint8_t buffer[4 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

        serialize::ReadStream readStream( buffer, 4 );
        int64_t value = 0;
        serialize_check( readStream.SerializeInteger64( value, INT64_MIN, INT64_MAX ) == false );
    }
}

inline void test_serialize_bytes_validation()
{
    // negative and huge byte counts must be rejected, not overflow the bounds check in bits
    uint8_t buffer[16 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader
    uint8_t data[16];

    {
        serialize::ReadStream readStream( buffer, 16 );
        serialize_check( readStream.SerializeBytes( data, -1 ) == false );
    }

    {
        serialize::ReadStream readStream( buffer, 16 );
        serialize_check( readStream.SerializeBytes( data, 1 << 29 ) == false );
    }
}

inline void test_int_relative_validation()
{
    // the 32 bit fallback must reject values that violate the previous < current contract
    {
        uint8_t buffer[8 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

        serialize::WriteStream writeStream( buffer, 8 );
        uint32_t six_false_bools = 0;
        writeStream.SerializeBits( six_false_bools, 6 );
        uint32_t bad_current = 50;
        writeStream.SerializeBits( bad_current, 32 );
        writeStream.Flush();

        serialize::ReadStream readStream( buffer, 8 );
        int previous = 100;
        int current = 0;
        serialize_check( serialize::serialize_int_relative_internal( readStream, previous, current ) == false );
    }

    // a legitimate fallback round trip must still succeed
    {
        uint8_t buffer[8 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

        serialize::WriteStream writeStream( buffer, 8 );
        int previous = 100;
        int written = 100000;
        serialize_check( serialize::serialize_int_relative_internal( writeStream, previous, written ) == true );
        writeStream.Flush();

        serialize::ReadStream readStream( buffer, 8 );
        int current = 0;
        serialize_check( serialize::serialize_int_relative_internal( readStream, previous, current ) == true );
        serialize_check( current == written );
    }

    // gaps wider than 2^31 overflow if the difference is computed in signed arithmetic (undefined behavior)
    {
        uint8_t buffer[8 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

        serialize::WriteStream writeStream( buffer, 8 );
        int previous = -1000;
        int written = INT32_MAX;
        serialize_check( serialize::serialize_int_relative_internal( writeStream, previous, written ) == true );
        writeStream.Flush();

        serialize::ReadStream readStream( buffer, 8 );
        int current = 0;
        serialize_check( serialize::serialize_int_relative_internal( readStream, previous, current ) == true );
        serialize_check( current == written );
    }

    // read side reconstructs current = previous + difference; a large previous overflows signed arithmetic.
    // this must wrap in the unsigned domain rather than invoke undefined behavior.
    {
        // difference of 1 exercises the oneBit branch, difference of 5 exercises a bucket branch
        const int differences[] = { 1, 5 };

        for ( int d = 0; d < (int) ( sizeof(differences) / sizeof(differences[0]) ); d++ )
        {
            uint8_t buffer[8 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

            serialize::WriteStream writeStream( buffer, 8 );
            int prevWrite = 10;
            int curWrite = prevWrite + differences[d];
            serialize_check( serialize::serialize_int_relative_internal( writeStream, prevWrite, curWrite ) == true );
            writeStream.Flush();

            serialize::ReadStream readStream( buffer, 8 );
            int previous = INT32_MAX;                        // previous + difference exceeds INT32_MAX
            int current = 0;
            serialize_check( serialize::serialize_int_relative_internal( readStream, previous, current ) == true );
            serialize_check( current == int32_t( uint32_t( INT32_MAX ) + uint32_t( differences[d] ) ) );
        }
    }
}

inline void test_compressed_float_validation()
{
    // a malicious packet can encode integer values above maxIntegerValue in the bit headroom. reads must reject them.
    {
        uint8_t buffer[8 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

        serialize::WriteStream writeStream( buffer, 8 );
        uint32_t out_of_range = 1023;                       // maxIntegerValue is 1000 for [0,10] at res 0.01 -> 10 bits
        writeStream.SerializeBits( out_of_range, 10 );
        writeStream.Flush();

        serialize::ReadStream readStream( buffer, 8 );
        float value = 0.0f;
        serialize_check( serialize::serialize_compressed_float_internal( readStream, value, 0.0f, 10.0f, 0.01f ) == false );
    }

    // huge delta / res ratios must not overflow the uint32 quantization range (undefined behavior)
    {
        uint8_t buffer[8 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

        serialize::WriteStream writeStream( buffer, 8 );
        float written = 5000000000.0f;
        serialize_check( serialize::serialize_compressed_float_internal( writeStream, written, 0.0f, 10000000000.0f, 1.0f ) == true );
        writeStream.Flush();

        serialize::ReadStream readStream( buffer, 8 );
        float value = 0.0f;
        serialize_check( serialize::serialize_compressed_float_internal( readStream, value, 0.0f, 10000000000.0f, 1.0f ) == true );
        serialize_check( fabs( value - written ) <= 4096.0f );
    }

    // a NaN value must not reach the uint32 cast (clamp comparisons are all false for NaN)
    {
        uint8_t buffer[8 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader

        serialize::WriteStream writeStream( buffer, 8 );
        const uint32_t nan_bits = 0x7fc00000;           // quiet NaN bit pattern, built without the NAN macro (finite-math builds reject it)
        float written = std::bit_cast<float>( nan_bits );
        serialize_check( serialize::serialize_compressed_float_internal( writeStream, written, 0.0f, 10.0f, 0.01f ) == true );
        writeStream.Flush();

        serialize::ReadStream readStream( buffer, 8 );
        float value = -1.0f;
        serialize_check( serialize::serialize_compressed_float_internal( readStream, value, 0.0f, 10.0f, 0.01f ) == true );
        serialize_check( value >= 0.0f && value <= 10.0f );      // NaN clamps to the low end of the range
    }
}

// Schemas promise byte-identical wire output to the equivalent serialize methods. These tests pin
// that promise: a schema and a macro-written serialize method for the same struct must produce the
// same bytes, cross-read each other, and reject the same malformed input.

struct SchemaTestPacket
{
    int32_t a;
    uint32_t bits9;
    bool extended;
    float fx;
    int64_t range34;
    uint32_t small;
    uint8_t blob[11];
    uint32_t tail;

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_int( stream, a, -50, +150 );
        serialize_bits( stream, bits9, 9 );
        serialize_bool( stream, extended );
        if ( extended )
        {
            serialize_float( stream, fx );
            serialize_int64( stream, range34, -5000000000LL, +5000000000LL );
        }
        serialize_bits( stream, small, 13 );
        serialize_bytes( stream, blob, (int) sizeof( blob ) );
        serialize_uint32( stream, tail );
        return true;
    }
};

using SchemaTestSchema = serialize::schema<
    serialize::int_<&SchemaTestPacket::a, -50, +150>,
    serialize::bits<&SchemaTestPacket::bits9, 9>,
    serialize::branch<&SchemaTestPacket::extended,
        serialize::fields<
            serialize::float_<&SchemaTestPacket::fx>,
            serialize::int64<&SchemaTestPacket::range34, -5000000000LL, +5000000000LL> >,
        serialize::fields<> >,
    serialize::bits<&SchemaTestPacket::small, 13>,
    serialize::bytes<&SchemaTestPacket::blob, 11>,
    serialize::bits<&SchemaTestPacket::tail, 32> >;

// the longest path: 8 + 9 + 1 + 32 + 34 + 13 = 97 bits, align to 104, + 88 blob + 32 tail = 224
static_assert( SchemaTestSchema::MaxBits == 224 );
static_assert( SchemaTestSchema::MaxBytes == 28 );

inline void test_schema()
{
    for ( int variant = 0; variant < 2; variant++ )
    {
        SchemaTestPacket packet;
        memset( (void*) &packet, 0, sizeof( packet ) );
        packet.a = -37;
        packet.bits9 = 300;
        packet.extended = ( variant == 0 );
        packet.fx = 3.25f;
        packet.range34 = 4123456789LL;
        packet.small = 5000;
        for ( int i = 0; i < (int) sizeof( packet.blob ); i++ )
            packet.blob[i] = (uint8_t) ( i * 31 + 7 );
        packet.tail = 0xDEADBEEF;

        uint8_t macro_buffer[64 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader
        serialize::WriteStream writeStream( macro_buffer, 64 );
        serialize_check( packet.Serialize( writeStream ) == true );
        writeStream.Flush();
        const int64_t macroBytes = writeStream.GetBytesProcessed();

        uint8_t schema_buffer[64 + 8] = { 0 };
        const int64_t schemaBytes = SchemaTestSchema::Write( schema_buffer, 64, packet );
        serialize_check( schemaBytes == macroBytes );
        serialize_check( memcmp( schema_buffer, macro_buffer, (size_t) macroBytes ) == 0 );

        // schema reads the macro bytes
        SchemaTestPacket out;
        memset( (void*) &out, 0, sizeof( out ) );
        serialize_check( SchemaTestSchema::Read( macro_buffer, macroBytes, out ) == true );
        serialize_check( out.a == packet.a );
        serialize_check( out.bits9 == packet.bits9 );
        serialize_check( out.extended == packet.extended );
        if ( packet.extended )
        {
            serialize_check( out.fx == packet.fx );
            serialize_check( out.range34 == packet.range34 );
        }
        serialize_check( out.small == packet.small );
        serialize_check( memcmp( out.blob, packet.blob, sizeof( packet.blob ) ) == 0 );
        serialize_check( out.tail == packet.tail );

        // the macros read the schema bytes
        SchemaTestPacket out2;
        memset( (void*) &out2, 0, sizeof( out2 ) );
        serialize::ReadStream readStream( schema_buffer, schemaBytes );
        serialize_check( out2.Serialize( readStream ) == true );
        serialize_check( out2.a == packet.a && out2.bits9 == packet.bits9 && out2.extended == packet.extended );
        serialize_check( out2.small == packet.small && out2.tail == packet.tail );

        // truncated packets must fail cleanly
        SchemaTestPacket truncated;
        memset( (void*) &truncated, 0, sizeof( truncated ) );
        serialize_check( SchemaTestSchema::Read( macro_buffer, 2, truncated ) == false );
    }

    // a malicious packet can encode out of range values in the bit headroom: [-50,150] is 201 values
    // in 8 bits, so 255 fits in the headroom. reads must reject it, exactly like the read stream.
    {
        uint8_t buffer[SchemaTestSchema::MaxBytes + 8] = { 0 };
        buffer[0] = 0xFF;
        SchemaTestPacket out;
        serialize_check( SchemaTestSchema::Read( buffer, SchemaTestSchema::MaxBytes, out ) == false );
    }

    // nonzero alignment padding must be rejected, exactly like ReadAlign. with extended false the
    // pad bit before the byte block is bit 31.
    {
        SchemaTestPacket packet;
        memset( (void*) &packet, 0, sizeof( packet ) );
        packet.a = 0;
        uint8_t buffer[64 + 8] = { 0 };
        const int64_t bytes = SchemaTestSchema::Write( buffer, 64, packet );
        buffer[3] |= 0x80;
        SchemaTestPacket out;
        serialize_check( SchemaTestSchema::Read( buffer, bytes, out ) == false );
    }
}

// object is the schema equivalent of serialize_object: inner schemas splice in place at
// compile time, so a schema built from nested schemas must be byte-identical to serialize methods
// built from serialize_object calls.

struct SchemaVec
{
    float x, y, z;

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_float( stream, x );
        serialize_float( stream, y );
        serialize_float( stream, z );
        return true;
    }
};

struct SchemaBody
{
    SchemaVec position;
    bool atRest;
    SchemaVec velocity;

    template <typename Stream> bool Serialize( Stream & stream )
    {
        serialize_object( stream, position );
        serialize_bool( stream, atRest );
        if ( !atRest )
        {
            serialize_object( stream, velocity );
        }
        return true;
    }
};

using SchemaVecSchema = serialize::schema<
    serialize::float_<&SchemaVec::x>,
    serialize::float_<&SchemaVec::y>,
    serialize::float_<&SchemaVec::z> >;

using SchemaBodySchema = serialize::schema<
    serialize::object<&SchemaBody::position, SchemaVecSchema>,
    serialize::branch<&SchemaBody::atRest,
        serialize::fields<>,
        serialize::fields< serialize::object<&SchemaBody::velocity, SchemaVecSchema> > > >;

static_assert( SchemaBodySchema::MaxBits == 96 + 1 + 96 );

inline void test_schema_object()
{
    for ( int variant = 0; variant < 2; variant++ )
    {
        SchemaBody body;
        memset( (void*) &body, 0, sizeof( body ) );
        body.position.x = 1.5f;
        body.position.y = -3.25f;
        body.position.z = 100.125f;
        body.atRest = ( variant == 0 );
        body.velocity.x = 5.0f;
        body.velocity.y = -6.5f;
        body.velocity.z = 7.75f;

        uint8_t macro_buffer[32 + 8] = { 0 };          // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader
        serialize::WriteStream writeStream( macro_buffer, 32 );
        serialize_check( body.Serialize( writeStream ) == true );
        writeStream.Flush();
        const int64_t macroBytes = writeStream.GetBytesProcessed();

        uint8_t schema_buffer[32 + 8] = { 0 };
        const int64_t schemaBytes = SchemaBodySchema::Write( schema_buffer, 32, body );
        serialize_check( schemaBytes == macroBytes );
        serialize_check( memcmp( schema_buffer, macro_buffer, (size_t) macroBytes ) == 0 );

        SchemaBody out;
        memset( (void*) &out, 0, sizeof( out ) );
        serialize_check( SchemaBodySchema::Read( macro_buffer, macroBytes, out ) == true );
        serialize_check( out.position.x == body.position.x );
        serialize_check( out.position.y == body.position.y );
        serialize_check( out.position.z == body.position.z );
        serialize_check( out.atRest == body.atRest );
        if ( !body.atRest )
        {
            serialize_check( out.velocity.x == body.velocity.x );
            serialize_check( out.velocity.y == body.velocity.y );
            serialize_check( out.velocity.z == body.velocity.z );
        }
    }
}

// Golden wire format test. The exact bytes produced by the serializer are pinned down here and must never change.
// If this test fails, the wire format has changed and previously written data no longer decodes: a breaking change.
// These are the same golden bytes as classic serialize: passing this test proves the modern port is wire compatible.
// The values below are chosen so every platform quantizes identically (see the compressed float: 5.0 in [0,10]
// normalizes to exactly 0.5, so fp contraction differences between compilers cannot shift the quantized integer).

struct GoldenWireData
{
    uint32_t bits4;
    uint32_t bits11;
    uint32_t bits24;
    uint32_t bits32;
    int32_t int_small;
    int32_t int_full;
    bool flag;
    float float_value;
    float compressed_float_value;
    double double_value;
    uint8_t uint8_value;
    uint16_t uint16_value;
    uint32_t uint32_value;
    uint64_t uint64_value;
    int relative_near;
    int relative_far;
    uint8_t bytes[7];
    char string[16];
    wchar_t wstring[8];
};

inline void GoldenWireInit( GoldenWireData & data )
{
    memset( (void*) &data, 0, sizeof( GoldenWireData ) );
    data.bits4 = 13;
    data.bits11 = 1445;
    data.bits24 = 11259375;
    data.bits32 = 0xDEADBEEF;
    data.int_small = -37;
    data.int_full = -123456789;
    data.flag = true;
    data.float_value = 3.1415926f;
    data.compressed_float_value = 5.0f;
    data.double_value = 1.0 / 3.0;
    data.uint8_value = 0x7F;
    data.uint16_value = 0x1234;
    data.uint32_value = 0x12345678;
    data.uint64_value = 0x123456789ABCDEF0ULL;
    data.relative_near = 101;                   // difference of 1 from the base: exercises the one bit branch
    data.relative_far = 2100;                   // difference of 2000 from the base: exercises the twelve bit bucket
    const uint8_t golden_byte_data[7] = { 0xDE, 0xAD, 0xBE, 0xEF, 0xCA, 0xFE, 0x01 };
    memcpy( data.bytes, golden_byte_data, sizeof( golden_byte_data ) );
    serialize_copy_string( data.string, "golden", sizeof( data.string ) );
    // built from explicit code points so the source file encoding can never change the golden bytes
    const wchar_t golden_wide_string[4] = { 0x043C, 0x0438, 0x0440, 0 };            // cyrillic, BMP only
    serialize_copy_wstring( data.wstring, golden_wide_string, sizeof( data.wstring ) / sizeof( wchar_t ) );
}

template <typename Stream> bool GoldenWireSerialize( Stream & stream, GoldenWireData & data )
{
    const int relative_base = 100;
    serialize_bits( stream, data.bits4, 4 );
    serialize_bits( stream, data.bits11, 11 );
    serialize_bits( stream, data.bits24, 24 );
    serialize_bits( stream, data.bits32, 32 );
    serialize_int( stream, data.int_small, -100, +100 );
    serialize_int( stream, data.int_full, INT32_MIN, INT32_MAX );
    serialize_bool( stream, data.flag );
    serialize_float( stream, data.float_value );
    serialize_compressed_float( stream, data.compressed_float_value, 0.0f, 10.0f, 0.01f );
    serialize_double( stream, data.double_value );
    serialize_uint8( stream, data.uint8_value );
    serialize_uint16( stream, data.uint16_value );
    serialize_uint32( stream, data.uint32_value );
    serialize_uint64( stream, data.uint64_value );
    serialize_int_relative( stream, relative_base, data.relative_near );
    serialize_int_relative( stream, relative_base, data.relative_far );
    serialize_align( stream );
    serialize_bytes( stream, data.bytes, (int) sizeof( data.bytes ) );
    serialize_string( stream, data.string, (int) sizeof( data.string ) );
    serialize_wstring( stream, data.wstring, (int) ( sizeof( data.wstring ) / sizeof( wchar_t ) ) );
    return true;
}

static const uint8_t golden_wire_bytes[] =
{
    0x5D, 0xDA, 0xF7, 0xE6, 0xD5, 0x77, 0xDF, 0x56, 0xEF, 0x9F, 0x75, 0x19,
    0x52, 0xBC, 0xDA, 0x0F, 0x49, 0x40, 0xF4, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0xFF, 0xFC, 0xD1, 0x48, 0xE0, 0x59, 0xD1, 0x48, 0xC0, 0x7B,
    0xF3, 0x6A, 0xE2, 0x59, 0xD1, 0x48, 0x84, 0xB7, 0x06, 0xDE, 0xAD, 0xBE,
    0xEF, 0xCA, 0xFE, 0x01, 0x06, 0x67, 0x6F, 0x6C, 0x64, 0x65, 0x6E, 0xE3,
    0x21, 0x00, 0x00, 0xC0, 0x21, 0x00, 0x00, 0x00, 0x22, 0x00, 0x00, 0x00
};

inline void test_golden_wire_format()
{
    // write side: serializing the golden values must produce exactly the golden bytes
    {
        uint8_t buffer[256 + 8];            // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader
        memset( buffer, 0, sizeof( buffer ) );
        serialize::WriteStream stream( buffer, 256 );
        GoldenWireData data;
        GoldenWireInit( data );
        serialize_check( GoldenWireSerialize( stream, data ) == true );
        stream.Flush();
        serialize_check( stream.GetBytesProcessed() == (int) sizeof( golden_wire_bytes ) );
        serialize_check( memcmp( buffer, golden_wire_bytes, sizeof( golden_wire_bytes ) ) == 0 );
    }

    // read side: the golden bytes must decode to the expected values, on every platform, forever
    {
        uint8_t buffer[256 + 8];            // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader
        memset( buffer, 0, sizeof( buffer ) );
        memcpy( buffer, golden_wire_bytes, sizeof( golden_wire_bytes ) );
        serialize::ReadStream stream( buffer, (int) sizeof( golden_wire_bytes ) );
        GoldenWireData data;
        memset( (void*) &data, 0, sizeof( GoldenWireData ) );
        serialize_check( GoldenWireSerialize( stream, data ) == true );

        GoldenWireData expected;
        GoldenWireInit( expected );
        serialize_check( data.bits4 == expected.bits4 );
        serialize_check( data.bits11 == expected.bits11 );
        serialize_check( data.bits24 == expected.bits24 );
        serialize_check( data.bits32 == expected.bits32 );
        serialize_check( data.int_small == expected.int_small );
        serialize_check( data.int_full == expected.int_full );
        serialize_check( data.flag == expected.flag );
        serialize_check( data.float_value == expected.float_value );
        serialize_check( fabs( data.compressed_float_value - expected.compressed_float_value ) <= 0.01f );
        serialize_check( data.double_value == expected.double_value );
        serialize_check( data.uint8_value == expected.uint8_value );
        serialize_check( data.uint16_value == expected.uint16_value );
        serialize_check( data.uint32_value == expected.uint32_value );
        serialize_check( data.uint64_value == expected.uint64_value );
        serialize_check( data.relative_near == expected.relative_near );
        serialize_check( data.relative_far == expected.relative_far );
        serialize_check( memcmp( data.bytes, expected.bytes, sizeof( data.bytes ) ) == 0 );
        serialize_check( strcmp( data.string, expected.string ) == 0 );
        serialize_check( wcscmp( data.wstring, expected.wstring ) == 0 );
    }
}

inline void test_unaligned_writer()
{
    // the bit writer stores each word with memcpy, so the write buffer needs no particular alignment.
    // exercise every offset within a word, covering the WriteBits, WriteBytes and FlushBits store paths.

    alignas( uint64_t ) uint8_t storage[256 + 16];              // 256 byte buffer + up to 7 byte offset + the 8 byte allocation contract

    for ( int offset = 0; offset < 8; offset++ )
    {
        memset( storage, 0, sizeof( storage ) );

        uint8_t * buffer = storage + offset;

        uint8_t data[13];
        for ( int i = 0; i < (int) sizeof( data ); i++ )
            data[i] = (uint8_t) ( i * 47 + offset );

        serialize::WriteStream writeStream( buffer, 256 );
        writeStream.SerializeBits( 0x12345678, 32 );
        writeStream.SerializeBits( 123, 7 );
        writeStream.SerializeBytes( data, (int) sizeof( data ) );
        writeStream.SerializeBits( 0xDEADBEEF, 32 );
        writeStream.Flush();

        const int bytesWritten = int( writeStream.GetBytesProcessed() );

        serialize::ReadStream readStream( buffer, bytesWritten );
        uint32_t a = 0;
        serialize_check( readStream.SerializeBits( a, 32 ) == true );
        serialize_check( a == 0x12345678 );
        uint32_t b = 0;
        serialize_check( readStream.SerializeBits( b, 7 ) == true );
        serialize_check( b == 123 );
        uint8_t read_data[13];
        memset( read_data, 0, sizeof( read_data ) );
        serialize_check( readStream.SerializeBytes( read_data, (int) sizeof( read_data ) ) == true );
        serialize_check( memcmp( read_data, data, sizeof( data ) ) == 0 );
        uint32_t c = 0;
        serialize_check( readStream.SerializeBits( c, 32 ) == true );
        serialize_check( c == 0xDEADBEEF );
    }
}

inline void test_large_buffer()
{
    // bit counts are 64 bit, so buffers larger than the old 256 MB limit work. write a bulk
    // block that carries the stream past the 2^31 bit boundary (256 MB), then verify that
    // bitpacked values round trip on the far side of it.

    const int64_t bufferSize = int64_t( 320 ) * 1024 * 1024;
    uint8_t * buffer = (uint8_t*) malloc( (size_t) bufferSize + 8 );        // + 8: buffer allocations extend 8 bytes past the end, for the writer and the reader
    if ( !buffer )
    {
        printf( "(skipped test_large_buffer: could not allocate the buffer)\n" );
        return;
    }

    static uint8_t chunk[1024*1024];
    for ( int i = 0; i < (int) sizeof( chunk ); i++ )
        chunk[i] = (uint8_t) ( i * 37 );

    const int numChunks = 300;                                              // 300 MB of bulk data: past the 256 MB boundary

    int64_t bytesWritten = 0;
    {
        serialize::WriteStream writeStream( buffer, bufferSize );
        for ( int i = 0; i < numChunks; i++ )
            serialize_check( writeStream.SerializeBytes( chunk, sizeof( chunk ) ) == true );
        uint32_t sentinel = 0xDEADBEEF;
        serialize_check( writeStream.SerializeBits( sentinel, 32 ) == true );
        int32_t value = -12345;
        serialize_check( writeStream.SerializeInteger( value, -100000, +100000 ) == true );
        writeStream.Flush();
        bytesWritten = writeStream.GetBytesProcessed();
        serialize_check( writeStream.GetBitsProcessed() > int64_t( 1 ) << 31 );     // the bit count really did cross the old 32 bit boundary
    }

    {
        serialize::ReadStream readStream( buffer, bytesWritten );
        static uint8_t readChunk[1024*1024];
        for ( int i = 0; i < numChunks; i++ )
            serialize_check( readStream.SerializeBytes( readChunk, sizeof( readChunk ) ) == true );
        serialize_check( memcmp( readChunk, chunk, sizeof( chunk ) ) == 0 );        // the final chunk, decoded from past the boundary
        uint32_t sentinel = 0;
        serialize_check( readStream.SerializeBits( sentinel, 32 ) == true );
        serialize_check( sentinel == 0xDEADBEEF );
        int32_t value = 0;
        serialize_check( readStream.SerializeInteger( value, -100000, +100000 ) == true );
        serialize_check( value == -12345 );
        serialize_check( readStream.GetBitsProcessed() > int64_t( 1 ) << 31 );
    }

    free( buffer );
}

#define SERIALIZE_RUN_TEST( test_function )                                 \
    do                                                                      \
    {                                                                       \
        printf( #test_function "\n" );                                      \
        test_function();                                                    \
    }                                                                       \
    while (0)

inline void serialize_test()
{
    // while ( 1 )
    {
        SERIALIZE_RUN_TEST( test_endian );
        SERIALIZE_RUN_TEST( test_bitpacker );
        SERIALIZE_RUN_TEST( test_bits64 );
        SERIALIZE_RUN_TEST( test_schema );
        SERIALIZE_RUN_TEST( test_schema_object );
        SERIALIZE_RUN_TEST( test_bits_required );
        SERIALIZE_RUN_TEST( test_bits_required64 );
        SERIALIZE_RUN_TEST( test_zigzag );
        SERIALIZE_RUN_TEST( test_serialize );
        SERIALIZE_RUN_TEST( test_read_write );
        SERIALIZE_RUN_TEST( test_serialize_integer_validation );
        SERIALIZE_RUN_TEST( test_serialize_integer_full_range );
        SERIALIZE_RUN_TEST( test_serialize_int64_full_range );
        SERIALIZE_RUN_TEST( test_serialize_int64_validation );
        SERIALIZE_RUN_TEST( test_serialize_bytes_validation );
        SERIALIZE_RUN_TEST( test_int_relative_validation );
        SERIALIZE_RUN_TEST( test_compressed_float_validation );
        SERIALIZE_RUN_TEST( test_golden_wire_format );
        SERIALIZE_RUN_TEST( test_unaligned_writer );
        SERIALIZE_RUN_TEST( test_large_buffer );
    }
}

#endif // #if SERIALIZE_ENABLE_TESTS

#endif // #ifndef SERIALIZE_H
