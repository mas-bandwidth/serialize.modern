/*
    serialize.modern codegen audit self test fixture

    Deliberately violates every codegen audit rule: a runtime-bound loop (backward branch), an
    out-of-line call, and enough instructions to blow any budget. codegen_audit.py runs in
    --self-test mode against this object and passes only if it detects the violations — proving
    the gate can actually fail. See codegen_audit.py.
*/

#include <stdint.h>
#include <stddef.h>

extern "C" uint64_t audit_bad_helper( uint64_t );       // defined nowhere: forces a real call

extern "C" uint64_t audit_bad_function( const uint8_t * data, int64_t bytes )
{
    uint64_t sum = 0;
    for ( int64_t i = 0; i < bytes; i++ )               // runtime loop: backward branch
        sum += data[i];
    const uint64_t result = audit_bad_helper( sum );    // out of line call (not a tail call: the result is used below)
    return result ^ sum;
}
