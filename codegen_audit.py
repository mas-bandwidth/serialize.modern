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
# usage: codegen_audit.py [--config CFG] [--dumpbin PATH] <object file>
#        codegen_audit.py --self-test [...] <object file>
#
#   --config CFG    skip (exit 0) unless CFG is Release: multi-config generators (MSVC) register
#                   the test for every configuration, and the audit is only meaningful optimized
#   --dumpbin PATH  disassemble with MSVC dumpbin /DISASM instead of nm+objdump
#   --self-test     verify the rules DETECT violations, against codegen_audit_bad.cpp's object
#                   (a gate that cannot fail is decoration)
#
# Supports arm64 and x86-64 objects: ELF and Mach-O via objdump, COFF via dumpbin.

import re
import subprocess
import sys

# audited functions: (instruction budget, profile). budgets are roughly 3x the counts measured on
# Apple clang 21 arm64, so compiler and platform drift passes while a structural regression (the
# ~150+ instruction stream cursor path coming back, per function it is inlined into) fails.
#
# profiles:
#   fixed   - the zero overhead promise: no calls, no loops, no indirect branches, budget
#   dynamic - runtime-length sections may loop and call (copies, strlen), but the function must
#             stay within budget and may not use indirect branches
AUDITS = {
    "audit_fixed_read": ( 180, "fixed" ),
    "audit_fixed_write": ( 120, "fixed" ),
    "audit_body_read": ( 90, "fixed" ),
    "audit_body_write": ( 90, "fixed" ),
    "audit_dynamic_read": ( 600, "dynamic" ),      # measured: 175 clang arm64, 191 gcc arm64
    "audit_dynamic_write": ( 600, "dynamic" ),     # measured: 204 clang arm64, 241 gcc arm64
    "audit_fixed_measure": ( 24, "fixed" ),        # measured: 2 clang arm64 (mov #const; ret)
    "audit_body_measure": ( 24, "fixed" ),         # measured: 6 clang arm64
    "audit_dynamic_measure": ( 150, "dynamic" ),   # measured: 19 clang arm64 (strlen for the string field)
}

CALL_MNEMONICS = { "bl", "blr", "call", "callq" }
INDIRECT_BRANCH_MNEMONICS = { "br", "blr", "jmpq" }


def is_branch( mnemonic, base ):
    # arm64: b and b.<cond> (base is the part before the dot), cbz/cbnz/tbz/tbnz.
    # x86: j<cc>/jmp and loop. deliberately NOT a bare startswith("b"): x86 ALU instructions
    # like bts/bsf/bswap begin with b and are not branches (a bts once false-positived this gate).
    return base == "b" or mnemonic in ( "cbz", "cbnz", "tbz", "tbnz" ) \
        or mnemonic.startswith( "j" ) or mnemonic.startswith( "loop" )


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


def function_bodies_dumpbin( path, names, dumpbin ):
    """name -> ordered list of (address, mnemonic, operands), parsed from dumpbin /DISASM"""
    bodies = {}
    current = None
    for line in run( [ dumpbin, "/NOLOGO", "/DISASM", path ] ).splitlines():
        label = re.match( r"^(\S+):\s*$", line )
        if label:
            name = label.group( 1 )
            current = name if name in names else None
            if current is not None:
                bodies[current] = []
            continue
        if current is None:
            continue
        insn = re.match( r"^  ([0-9a-fA-F]+):(?:\s+[0-9a-fA-F]{2})+\s{2,}(\S+)\s*(.*)$", line )
        if insn:
            bodies[current].append( ( int( insn.group( 1 ), 16 ), insn.group( 2 ), insn.group( 3 ) ) )
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


def audit_function( name, body, profile="fixed" ):
    violations = []
    for address, mnemonic, operands in body:
        base = mnemonic.split( "." )[0]         # b.ge -> b, jne stays jne
        if base in CALL_MNEMONICS:
            if profile == "fixed":
                violations.append( f"call instruction at {address:#x}: {mnemonic} {operands}" )
        elif base in INDIRECT_BRANCH_MNEMONICS:
            violations.append( f"indirect branch at {address:#x}: {mnemonic} {operands}" )
        elif is_branch( mnemonic, base ):
            target = branch_target( operands )
            if target is None:
                violations.append( f"indirect branch at {address:#x}: {mnemonic} {operands}" )
            elif target <= address:
                if profile == "fixed":
                    violations.append( f"backward branch (loop) at {address:#x}: {mnemonic} {operands}" )
    budget = AUDITS.get( name, ( None, ) )[0]
    if budget is not None and len( body ) > budget:
        violations.append( f"instruction count {len( body )} exceeds budget {budget}" )
    return violations


def get_flag( flag ):
    if flag in sys.argv:
        index = sys.argv.index( flag )
        if index + 1 < len( sys.argv ):
            return sys.argv[index + 1]
    return None


def main():
    self_test = "--self-test" in sys.argv
    config = get_flag( "--config" )
    dumpbin = get_flag( "--dumpbin" )
    consumed = { "--self-test" }
    if config is not None: consumed |= { "--config", config }
    if dumpbin is not None: consumed |= { "--dumpbin", dumpbin }
    paths = [ a for a in sys.argv[1:] if a not in consumed ]
    if len( paths ) != 1:
        print( "usage: codegen_audit.py [--self-test] [--config CFG] [--dumpbin PATH] <object file>" )
        return 1
    path = paths[0]

    if config is not None and config != "Release":
        print( f"codegen audit skipped: only meaningful in Release builds (this is {config})" )
        return 0

    def bodies_for( names ):
        if dumpbin is not None:
            return function_bodies_dumpbin( path, names, dumpbin )
        return function_bodies( path, names )

    if self_test:
        # the rules must detect the deliberate violations in codegen_audit_bad.cpp
        bodies = bodies_for( { "audit_bad_function" } )
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

    bodies = bodies_for( set( AUDITS.keys() ) )
    missing = set( AUDITS.keys() ) - set( bodies.keys() )
    if missing:
        print( f"codegen audit FAILED: functions not found in object: {sorted( missing )}" )
        return 1

    failed = False
    for name in sorted( bodies.keys() ):
        budget, profile = AUDITS[name]
        violations = audit_function( name, bodies[name], profile )
        status = "FAILED" if violations else "ok"
        print( f"{name}: {len( bodies[name] )} instructions (budget {budget}, {profile}) ... {status}" )
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
