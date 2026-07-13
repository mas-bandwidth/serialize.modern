cmake_minimum_required(VERSION 3.16)

project(serialize VERSION 0.1.0 LANGUAGES CXX)

include(GNUInstallDirs)

# is this the top level project, or pulled in via add_subdirectory / FetchContent?
if(CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
    set(SERIALIZE_TOP_LEVEL TRUE)
else()
    set(SERIALIZE_TOP_LEVEL FALSE)
endif()

option(SERIALIZE_BUILD_TESTS "Build serialize tests, example and benchmark" ${SERIALIZE_TOP_LEVEL})
option(SERIALIZE_INSTALL "Generate serialize install rules" ${SERIALIZE_TOP_LEVEL})
option(SERIALIZE_FUZZ "Build the libFuzzer harness (requires clang)" OFF)

# header-only library: consumers get the include path and nothing else — no compile flags leak
add_library(serialize INTERFACE)
add_library(serialize::serialize ALIAS serialize)
# serialize.modern requires C++23: <bit>, std::bit_cast, std::byteswap, std::endian, if constexpr macros
target_compile_features(serialize INTERFACE cxx_std_23)
target_include_directories(serialize INTERFACE
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

if(SERIALIZE_INSTALL)
    include(CMakePackageConfigHelpers)
    install(FILES serialize.h DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})
    install(TARGETS serialize EXPORT serialize-targets)
    install(EXPORT serialize-targets
        FILE serialize-config.cmake
        NAMESPACE serialize::
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/serialize
    )
    write_basic_package_version_file(
        ${CMAKE_CURRENT_BINARY_DIR}/serialize-config-version.cmake
        COMPATIBILITY SameMajorVersion
        ARCH_INDEPENDENT
    )
    install(FILES ${CMAKE_CURRENT_BINARY_DIR}/serialize-config-version.cmake
        DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/serialize
    )
endif()

# everything below builds the executables in this repo. flags here are scoped to those targets only.

if(SERIALIZE_TOP_LEVEL AND NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(SERIALIZE_BUILD_TESTS OR SERIALIZE_FUZZ)
    # same flags as the historical premake build: extra warnings, fast float, no rtti
    if(MSVC)
        # 4127/4244 are opted back in here: the header now push/pops them, and this repo's own
        # executables use the same implicit-narrowing style as the header
        set(SERIALIZE_DEV_FLAGS /W4 /fp:fast /GR- /wd4127 /wd4244)
    else()
        set(SERIALIZE_DEV_FLAGS -Wall -Wextra -ffast-math -fno-rtti)
    endif()
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
endif()

if(SERIALIZE_BUILD_TESTS)
    # "test" as a target name is reserved by CTest, so the target is serialize_test but the binary is still bin/test
    add_executable(serialize_test test.cpp serialize.h)
    target_compile_definitions(serialize_test PRIVATE SERIALIZE_ENABLE_TESTS=1)
    set_target_properties(serialize_test PROPERTIES OUTPUT_NAME test)

    add_executable(example example.cpp serialize.h)

    add_executable(bench bench.cpp serialize.h)

    foreach(target serialize_test example bench)
        target_link_libraries(${target} PRIVATE serialize)
        target_compile_features(${target} PRIVATE cxx_std_23)
        target_compile_options(${target} PRIVATE ${SERIALIZE_DEV_FLAGS})
        target_compile_definitions(${target} PRIVATE
            $<$<CONFIG:Debug>:SERIALIZE_DEBUG>
            $<$<NOT:$<CONFIG:Debug>>:SERIALIZE_RELEASE>
        )
    endforeach()

    enable_testing()
    add_test(NAME test COMMAND serialize_test)
endif()

# cross-library wire compatibility gate. point SERIALIZE_WIRE_COMPAT_CLASSIC_DIR at a checkout of
# classic serialize (https://github.com/mas-bandwidth/serialize) to build the wire_compat harness
# against BOTH libraries and register tests that prove the wire formats are identical: the two
# binaries must produce byte-identical corpus streams, and each must read the other's stream.
# the two headers share a header guard, namespace and macros, so the harness includes <serialize.h>
# with angle brackets and each executable selects its library purely via the include path.
# CI runs this on every push and pull request; see .github/workflows/ci.yml.
set(SERIALIZE_WIRE_COMPAT_CLASSIC_DIR "" CACHE PATH "Path to a classic serialize checkout; enables the cross-library wire compatibility tests")

if(SERIALIZE_BUILD_TESTS AND SERIALIZE_WIRE_COMPAT_CLASSIC_DIR)
    if(NOT EXISTS ${SERIALIZE_WIRE_COMPAT_CLASSIC_DIR}/serialize.h)
        message(FATAL_ERROR "SERIALIZE_WIRE_COMPAT_CLASSIC_DIR does not contain serialize.h: ${SERIALIZE_WIRE_COMPAT_CLASSIC_DIR}")
    endif()

    add_executable(wire_compat_modern wire_compat.cpp serialize.h)
    target_include_directories(wire_compat_modern PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

    add_executable(wire_compat_classic wire_compat.cpp ${SERIALIZE_WIRE_COMPAT_CLASSIC_DIR}/serialize.h)
    target_include_directories(wire_compat_classic PRIVATE ${SERIALIZE_WIRE_COMPAT_CLASSIC_DIR})

    foreach(target wire_compat_modern wire_compat_classic)
        target_compile_features(${target} PRIVATE cxx_std_23)
        target_compile_options(${target} PRIVATE ${SERIALIZE_DEV_FLAGS})
        target_compile_definitions(${target} PRIVATE
            $<$<CONFIG:Debug>:SERIALIZE_DEBUG>
            $<$<NOT:$<CONFIG:Debug>>:SERIALIZE_RELEASE>
        )
    endforeach()

    add_test(NAME wire_compat_write_classic COMMAND wire_compat_classic write ${CMAKE_CURRENT_BINARY_DIR}/wire_classic.bin)
    add_test(NAME wire_compat_write_modern COMMAND wire_compat_modern write ${CMAKE_CURRENT_BINARY_DIR}/wire_modern.bin)

    # the corpus streams must be byte-identical, and each library must decode the other's stream
    add_test(NAME wire_compat_bytes_identical COMMAND ${CMAKE_COMMAND} -E compare_files ${CMAKE_CURRENT_BINARY_DIR}/wire_classic.bin ${CMAKE_CURRENT_BINARY_DIR}/wire_modern.bin)
    add_test(NAME wire_compat_modern_reads_classic COMMAND wire_compat_modern read ${CMAKE_CURRENT_BINARY_DIR}/wire_classic.bin)
    add_test(NAME wire_compat_classic_reads_modern COMMAND wire_compat_classic read ${CMAKE_CURRENT_BINARY_DIR}/wire_modern.bin)

    set_tests_properties(wire_compat_write_classic wire_compat_write_modern PROPERTIES FIXTURES_SETUP wire_corpus)
    set_tests_properties(wire_compat_bytes_identical wire_compat_modern_reads_classic wire_compat_classic_reads_modern PROPERTIES FIXTURES_REQUIRED wire_corpus)
endif()

# codegen audit gate. compile time schemas promise zero runtime overhead, and this enforces it
# from PR to PR across the whole compiler suite: codegen_audit.py disassembles the schema
# Read/Write functions in codegen_audit.cpp and fails on call instructions, loops, indirect
# branches or instruction count blowups. GCC and clang objects parse via nm+objdump, MSVC via
# dumpbin /DISASM; multi-config generators register the test for every configuration and the
# script skips anything but Release (the audit is only meaningful optimized). registers on
# non-cross x86-64/arm64 builds; the big-endian s390x job is excluded on purpose — it exists to
# prove wire correctness, and auditing s390x mnemonics would buy nothing more. the self test runs
# the same rules against deliberately bad code and passes only if violations are detected: the
# gate can fail.
if(SERIALIZE_BUILD_TESTS AND NOT CMAKE_CROSSCOMPILING
   AND CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|arm64|aarch64|ARM64)$"
   AND (CMAKE_CONFIGURATION_TYPES OR CMAKE_BUILD_TYPE STREQUAL "Release"))
    find_package(Python3 COMPONENTS Interpreter)
    if(Python3_FOUND)
        set(SERIALIZE_AUDIT_ARGS --config $<CONFIG>)
        if(MSVC)
            get_filename_component(SERIALIZE_MSVC_BIN_DIR ${CMAKE_CXX_COMPILER} DIRECTORY)
            list(APPEND SERIALIZE_AUDIT_ARGS --dumpbin ${SERIALIZE_MSVC_BIN_DIR}/dumpbin.exe)
        endif()
        add_library(codegen_audit_obj OBJECT codegen_audit.cpp serialize.h)
        target_link_libraries(codegen_audit_obj PRIVATE serialize)
        target_compile_features(codegen_audit_obj PRIVATE cxx_std_23)
        target_compile_options(codegen_audit_obj PRIVATE ${SERIALIZE_DEV_FLAGS})
        target_compile_definitions(codegen_audit_obj PRIVATE SERIALIZE_RELEASE)

        add_library(codegen_audit_bad_obj OBJECT codegen_audit_bad.cpp)
        target_compile_options(codegen_audit_bad_obj PRIVATE ${SERIALIZE_DEV_FLAGS})

        add_test(NAME codegen_audit COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/codegen_audit.py ${SERIALIZE_AUDIT_ARGS} $<TARGET_OBJECTS:codegen_audit_obj>)
        add_test(NAME codegen_audit_selftest COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/codegen_audit.py --self-test ${SERIALIZE_AUDIT_ARGS} $<TARGET_OBJECTS:codegen_audit_bad_obj>)
    endif()
endif()

# libFuzzer harness for the read side. requires clang. build in Debug so asserts stay live.
if(SERIALIZE_FUZZ)
    add_executable(fuzz fuzz.cpp serialize.h)
    target_link_libraries(fuzz PRIVATE serialize)
    target_compile_features(fuzz PRIVATE cxx_std_23)
    target_compile_options(fuzz PRIVATE ${SERIALIZE_DEV_FLAGS} -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer)
    target_link_options(fuzz PRIVATE -fsanitize=fuzzer,address,undefined)
    target_compile_definitions(fuzz PRIVATE
        $<$<CONFIG:Debug>:SERIALIZE_DEBUG>
        $<$<NOT:$<CONFIG:Debug>>:SERIALIZE_RELEASE>
    )
endif()
