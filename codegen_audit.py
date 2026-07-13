#!/usr/bin/env python3
#
# serialize.modern codegen audit
#
# Compile time schemas promise zero runtime overhead. This script proves the generated code keeps
# that promise: it disassembles codegen_audit.cpp's object file and fails if any audited schema
# Read/Write function contains:
#
#   1. a call instruction     - inlining broke: some part of the schema fell out of line
#   2. a backward branch      - a loop appeared: the runtime bit cursor or a byte copy loop is back
#   3. an indirect branch     - a jump table or virtual dispatch appeared
#   4. too many instructions  - a wholesale codegen regression (budgets are ~3x current, so normal
#                               compiler drift passes and structural regressions fail)
#
# Forward branches are expected and allowed: input validation early-outs and compile time if/else.
#
# usage: codegen_audit.py <object file>              run the audit
#        codegen_audit.py --self-test <object file>  verify the rules DETECT violations, against
#                                                    codegen_audit_bad.cpp's object (a gate that
#                                                    cannot fail is decoration)
#
# Supports arm64 and x86-64 objects (ELF and Mach-O), disassembled with objdump.

import re
import subprocess
import sys

# instruction budgets, keyed by unmangled symbol name. roughly 3x the counts measured on Apple
# clang 21 arm64 (fixed_read 57, fixed_write 33, body_read 29, body_write 28), so compiler and
# platform drift passes while a structural regression (the ~150+ instruction stream cursor path
# coming back, per function it is inlined into) fails.
BUDGETS = {
    "audit_fixed_read": 180,
    "audit_fixed_write": 120,
    "audit_body_read": 90,
    "audit_body_write": 90,
}

CALL_MNEMONICS = { "bl", "blr", "call", "callq" }
INDIRECT_BRANCH_MNEMONICS = { "br", "blr", "jmpq" }
BRANCH_PREFIXES = ( "b", "j", "cbz", "cbnz", "tbz", "tbnz", "loop" )


def run( args ):
    return subprocess.run( args, capture_output=True, text=True, check=True ).stdout


def symbol_table( path ):
    """address -> unmangled name for defined text symbols, sorted by address"""
    symbols = []
    for line in run( [ "nm", path ] ).splitlines():
        match = re.match( r"^([0-9a-fA-F]+)\s+[TtSs]\s+_?(\w+)$", line.strip() )
        if match:
            symbols.append( ( int( match.group( 1 ), 16 ), match.group( 2 ) ) )
    symbols.sort()
    return symbols


def disassemble( path ):
    """address -> (mnemonic, operands) for every instruction in the object"""
    instructions = {}
    for line in run( [ "objdump", "-d", "--no-show-raw-insn", path ] ).splitlines():
        match = re.match( r"^\s*([0-9a-fA-F]+):\s+(\S+)\s*(.*)$", line )
        if match:
            instructions[int( match.group( 1 ), 16 )] = ( match.group( 2 ), match.group( 3 ) )
    return instructions


def function_bodies( path, names ):
    """name -> ordered list of (address, mnemonic, operands) for each requested function"""
    symbols = symbol_table( path )
    instructions = disassemble( path )
    addresses = sorted( instructions.keys() )
    # a function ends at the next distinct symbol address: assembler-local symbols (Mach-O ltmp)
    # can share the start address of a real function, so equal addresses never end a function
    boundaries = sorted( { address for address, _ in symbols } )
    bodies = {}
    for start, name in symbols:
        if name not in names:
            continue
        later = [ b for b in boundaries if b > start ]
        end = later[0] if later else float( "inf" )
        bodies[name] = [ ( a, *instructions[a] ) for a in addresses if start <= a < end ]
    return bodies


def branch_target( operands ):
    """the destination address of a direct branch, or None if there is no literal target"""
    match = re.search( r"\b0?x?([0-9a-fA-F]+)\s*(?:<|$)", operands.strip() )
    if match:
        try:
            return int( match.group( 1 ), 16 )
        except ValueError:
            return None
    return None


def audit_function( name, body ):
    violations = []
    for address, mnemonic, operands in body:
        base = mnemonic.split( "." )[0]         # b.ge -> b, jne stays jne
        if base in CALL_MNEMONICS:
            violations.append( f"call instruction at {address:#x}: {mnemonic} {operands}" )
        elif base in INDIRECT_BRANCH_MNEMONICS:
            violations.append( f"indirect branch at {address:#x}: {mnemonic} {operands}" )
        elif mnemonic.startswith( BRANCH_PREFIXES ):
            target = branch_target( operands )
            if target is not None and target <= address:
                violations.append( f"backward branch (loop) at {address:#x}: {mnemonic} {operands}" )
    budget = BUDGETS.get( name )
    if budget is not None and len( body ) > budget:
        violations.append( f"instruction count {len( body )} exceeds budget {budget}" )
    return violations


def main():
    self_test = "--self-test" in sys.argv
    paths = [ a for a in sys.argv[1:] if not a.startswith( "--" ) ]
    if len( paths ) != 1:
        print( "usage: codegen_audit.py [--self-test] <object file>" )
        return 1
    path = paths[0]

    if self_test:
        # the rules must detect the deliberate violations in codegen_audit_bad.cpp
        bodies = function_bodies( path, { "audit_bad_function" } )
        if "audit_bad_function" not in bodies:
            print( "self-test FAILED: audit_bad_function not found in object" )
            return 1
        violations = audit_function( "audit_bad_function", bodies["audit_bad_function"] )
        found_call = any( "call" in v for v in violations )
        found_loop = any( "backward" in v for v in violations )
        if found_call and found_loop:
            print( f"self-test passed: detected {len( violations )} violations in the bad fixture" )
            return 0
        print( f"self-test FAILED: expected a call and a loop to be detected, got: {violations}" )
        return 1

    bodies = function_bodies( path, set( BUDGETS.keys() ) )
    missing = set( BUDGETS.keys() ) - set( bodies.keys() )
    if missing:
        print( f"codegen audit FAILED: functions not found in object: {sorted( missing )}" )
        return 1

    failed = False
    for name in sorted( bodies.keys() ):
        violations = audit_function( name, bodies[name] )
        status = "FAILED" if violations else "ok"
        print( f"{name}: {len( bodies[name] )} instructions (budget {BUDGETS[name]}) ... {status}" )
        for violation in violations:
            print( f"    {violation}" )
            failed = True

    if failed:
        print( "codegen audit FAILED: schema Read/Write no longer compiles to straight-line, call-free code" )
        return 1

    print( "codegen audit passed: schemas compile to straight-line, call-free code" )
    return 0


if __name__ == "__main__":
    sys.exit( main() )
