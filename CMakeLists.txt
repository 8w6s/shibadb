cmake_minimum_required(VERSION 3.20)
project(shibadb_c VERSION 1.0.0 LANGUAGES C)

include(GNUInstallDirs)
include(CMakePackageConfigHelpers)

option(SDB_BUILD_TESTS "Build ShibaDB C tests" ON)
option(SDB_ENABLE_SANITIZERS "Enable AddressSanitizer and UndefinedBehaviorSanitizer" OFF)
option(SDB_ENABLE_THREAD_SANITIZER "Enable ThreadSanitizer" OFF)
option(SDB_ENABLE_SOAK_TESTS "Enable long-running engine soak tests" OFF)
option(SDB_BUILD_BENCHMARKS "Build the KV benchmark harness" OFF)
option(SDB_BUILD_CLI "Build the shibadb command-line tool" ON)
option(SDB_ENABLE_FUZZERS "Build libFuzzer targets (requires Clang)" OFF)
# A7.7: gate test-hook symbols behind SDB_TESTING. Auto-ON when tests
# build; a release build (SDB_BUILD_TESTS=OFF, SDB_TESTING=OFF) strips
# the fault-injection mutators and struct fields entirely.
option(SDB_TESTING "Compile fault-injection test hooks into shibadb_core" ${SDB_BUILD_TESTS})
# Distribution-hardening flags. Off by default because they interact
# badly with sanitizer presets (FORTIFY_SOURCE + ASan clash on some
# glibc versions) and with -O0 coverage builds. The release preset
# turns this on; adopters distributing binaries should as well.
option(SDB_HARDENED "Enable release-hardening compile + link flags" OFF)

if(SDB_ENABLE_SANITIZERS AND SDB_ENABLE_THREAD_SANITIZER)
    message(FATAL_ERROR "ASan/UBSan and ThreadSanitizer cannot be enabled together")
endif()
if(SDB_ENABLE_FUZZERS AND NOT CMAKE_C_COMPILER_ID STREQUAL "Clang")
    message(FATAL_ERROR "SDB_ENABLE_FUZZERS requires Clang (libFuzzer is a Clang feature)")
endif()

set(SHIBADB_SOURCES
    src/btree.c
    src/btree_page.c
    src/checksum.c
    src/codec.c
    src/encrypted_page.c
    src/engine.c
    src/file.c
    src/key_manager.c
    src/page.c
    src/page_cache.c
    src/pager.c
    src/random.c
    src/replace.c
    src/sha256.c
    src/status.c
    src/superblock.c
    src/superblock_store.c
    src/sync.c
    src/wal.c
    src/wal_index.c
    src/windows_path.c
    src/xchacha20poly1305.c
)

add_library(shibadb_core STATIC ${SHIBADB_SOURCES})
add_library(shibadb_shared SHARED ${SHIBADB_SOURCES})
add_library(ShibaDB::core ALIAS shibadb_core)
add_library(ShibaDB::shared ALIAS shibadb_shared)

if(NOT WIN32)
    find_package(Threads REQUIRED)
endif()

function(shibadb_configure_target target)
    target_include_directories(${target}
        PUBLIC
            $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
            $<INSTALL_INTERFACE:include>
        PRIVATE
            ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_compile_features(${target} PUBLIC c_std_11)
    if(WIN32)
        target_link_libraries(${target} PRIVATE bcrypt)
    else()
        target_link_libraries(${target} PUBLIC Threads::Threads)
        target_compile_definitions(${target} PRIVATE
            _POSIX_C_SOURCE=200809L
            _XOPEN_SOURCE=700
            # Force 64-bit off_t on 32-bit POSIX targets so pread/pwrite/
            # ftruncate offsets do not silently truncate uint64_t page
            # arithmetic. On 64-bit hosts this define is a no-op; on
            # 32-bit hosts it selects the LFS64 variants at libc.
            _FILE_OFFSET_BITS=64
        )
        if(APPLE)
            # _POSIX_C_SOURCE hides Darwin-specific symbols
            # (F_FULLFSYNC, etc.). _DARWIN_C_SOURCE re-exposes them.
            target_compile_definitions(${target} PRIVATE _DARWIN_C_SOURCE)
        endif()
    endif()
    set_target_properties(${target} PROPERTIES
        C_EXTENSIONS OFF
        POSITION_INDEPENDENT_CODE ON
    )
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4 /WX)
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow
            -Wstrict-prototypes -Wmissing-prototypes -Werror
        )
    endif()
    if(SDB_ENABLE_SANITIZERS AND NOT MSVC)
        target_compile_options(${target} PUBLIC
            -fno-omit-frame-pointer -fsanitize=address,undefined
        )
        target_link_options(${target} PUBLIC -fsanitize=address,undefined)
    endif()
    if(SDB_ENABLE_THREAD_SANITIZER AND NOT MSVC)
        target_compile_options(${target} PUBLIC
            -fno-omit-frame-pointer -fsanitize=thread
        )
        target_link_options(${target} PUBLIC -fsanitize=thread)
    endif()
    if(SDB_HARDENED AND NOT MSVC
       AND NOT SDB_ENABLE_SANITIZERS
       AND NOT SDB_ENABLE_THREAD_SANITIZER)
        # Distribution hardening set. -fcf-protection=full instruments
        # indirect branches for Intel CET (x86_64 only); Apple Clang on
        # ARM64 rejects the flag rather than treating it as a no-op, so
        # gate it on x86 architectures. RELRO/BIND_NOW/noexecstack are
        # applied only on ELF platforms below (macOS ld64 refuses them).
        target_compile_options(${target} PRIVATE
            -fstack-protector-strong
        )
        if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|amd64|AMD64|i.86)$")
            target_compile_options(${target} PRIVATE
                -fcf-protection=full
            )
        endif()
        target_compile_definitions(${target} PRIVATE
            $<BUILD_INTERFACE:_FORTIFY_SOURCE=2>
        )
        if(NOT APPLE)
            # macOS ld64 does not accept -z relro / -z now / -z
            # noexecstack; the linker there enforces equivalents by
            # default. Apply on ELF platforms only.
            target_link_options(${target} PRIVATE
                LINKER:-z,relro
                LINKER:-z,now
                LINKER:-z,noexecstack
            )
        endif()
    endif()
    # SDB_TESTING is used inside src/ and tests/*.c linking against
    # ShibaDB::core; we keep the definition visible via BUILD_INTERFACE
    # so in-tree consumers see the same value, but strip it from the
    # INSTALL_INTERFACE so downstream consumers of an installed package
    # never see a build-mode-dependent macro. Same fix for the fuzz
    # sanitizer flags below.
    if(SDB_TESTING)
        target_compile_definitions(${target}
            PUBLIC $<BUILD_INTERFACE:SDB_TESTING=1>
        )
    else()
        target_compile_definitions(${target}
            PUBLIC $<BUILD_INTERFACE:SDB_TESTING=0>
        )
    endif()
    # libFuzzer needs SanitizerCoverage instrumentation baked into
    # every compilation unit reached by a fuzz target. Without this
    # the harness runs blind (cov=1) and effectively random-searches.
    # In OSS-Fuzz / ClusterFuzzLite the harness injects its own flags,
    # so we only add these locally when SDB_FUZZ_LINK_FLAGS is empty.
    if(SDB_ENABLE_FUZZERS AND NOT MSVC
        AND (NOT DEFINED SDB_FUZZ_LINK_FLAGS OR SDB_FUZZ_LINK_FLAGS STREQUAL ""))
        target_compile_options(${target} PUBLIC
            $<BUILD_INTERFACE:-fsanitize=fuzzer-no-link,address,undefined>
            $<BUILD_INTERFACE:-fno-omit-frame-pointer>
        )
        target_link_options(${target} PUBLIC
            $<BUILD_INTERFACE:-fsanitize=fuzzer-no-link,address,undefined>
        )
    endif()
endfunction()

shibadb_configure_target(shibadb_core)
shibadb_configure_target(shibadb_shared)
target_compile_definitions(shibadb_shared
    PRIVATE SDB_SHARED SDB_BUILDING_LIBRARY
    INTERFACE SDB_SHARED
)
set_target_properties(shibadb_shared PROPERTIES
    OUTPUT_NAME shibadb
    VERSION ${PROJECT_VERSION}
    SOVERSION 1
    C_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN YES
)
set_target_properties(shibadb_core PROPERTIES EXPORT_NAME core)
set_target_properties(shibadb_shared PROPERTIES EXPORT_NAME shared)

if(MSVC)
    set_target_properties(shibadb_shared PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)
else()
    set_target_properties(shibadb_core PROPERTIES C_VISIBILITY_PRESET hidden)
endif()

install(
    TARGETS shibadb_core shibadb_shared
    EXPORT ShibaDBTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(
    FILES
        include/shibadb.h
        include/shibadb_engine.h
        include/shibadb_visibility.h
    DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)
install(
    EXPORT ShibaDBTargets
    FILE ShibaDBTargets.cmake
    NAMESPACE ShibaDB::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ShibaDB
)
configure_package_config_file(
    cmake/ShibaDBConfig.cmake.in
    ${CMAKE_CURRENT_BINARY_DIR}/ShibaDBConfig.cmake
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ShibaDB
)
write_basic_package_version_file(
    ${CMAKE_CURRENT_BINARY_DIR}/ShibaDBConfigVersion.cmake
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)
install(
    FILES
        ${CMAKE_CURRENT_BINARY_DIR}/ShibaDBConfig.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/ShibaDBConfigVersion.cmake
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/ShibaDB
)
if(WIN32)
    set(SHIBADB_PC_PRIVATE_LIBS "-lbcrypt")
else()
    set(SHIBADB_PC_PRIVATE_LIBS "-pthread")
endif()
configure_file(
    cmake/shibadb.pc.in
    ${CMAKE_CURRENT_BINARY_DIR}/shibadb.pc
    @ONLY
)
install(
    FILES ${CMAKE_CURRENT_BINARY_DIR}/shibadb.pc
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/pkgconfig
)

if(SDB_BUILD_CLI)
    # The `shibadb` command-line tool. Links the static core so the produced
    # binary is self-contained (no shared-library path needed at runtime) and
    # exercises exactly the public engine API an embedder would use.
    add_executable(shibadb_cli cli/shibadb_cli.c)
    # The shared library target (shibadb_shared) also uses OUTPUT_NAME
    # "shibadb", so its import lib (shibadb.lib) and PDB (shibadb.pdb) share a
    # basename with this executable's in the same build directory. A parallel
    # link then races on shibadb.pdb and fails with LNK1201. Keep the runtime
    # artifact shibadb.exe but give the auxiliary files distinct names.
    set_target_properties(shibadb_cli PROPERTIES
        OUTPUT_NAME shibadb
        PDB_NAME shibadb_cli
        ARCHIVE_OUTPUT_NAME shibadb_cli
    )
    target_link_libraries(shibadb_cli PRIVATE ShibaDB::core)
    if(MSVC)
        target_compile_options(shibadb_cli PRIVATE /W4 /WX)
        # The CLI reads getenv("SHIBADB_PASSWORD"); MSVC deprecates getenv
        # (C4996) but the call is portable and correct. Scope the suppression
        # to this target only — the library core stays free of it.
        target_compile_definitions(shibadb_cli PRIVATE _CRT_SECURE_NO_WARNINGS)
    else()
        target_compile_options(shibadb_cli PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow
            -Wstrict-prototypes -Wmissing-prototypes -Werror
        )
    endif()
    install(TARGETS shibadb_cli RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
endif()

if(SDB_BUILD_TESTS OR SDB_ENABLE_SOAK_TESTS)
    enable_testing()
endif()

if(SDB_BUILD_TESTS)
    if(MSVC)
        add_compile_options(/UNDEBUG)
    else()
        add_compile_options(-UNDEBUG)
    endif()
    add_executable(test_abi tests/test_abi.c)
    target_link_libraries(test_abi PRIVATE ShibaDB::shared)
    add_executable(test_superblock tests/test_superblock.c)
    target_link_libraries(test_superblock PRIVATE ShibaDB::core)
    add_executable(test_database_info tests/test_database_info.c)
    target_link_libraries(test_database_info PRIVATE ShibaDB::core)
    add_executable(test_convenience tests/test_convenience.c)
    target_link_libraries(test_convenience PRIVATE ShibaDB::core)
    add_executable(test_superblock_field_diag tests/test_superblock_field_diag.c)
    target_link_libraries(test_superblock_field_diag PRIVATE ShibaDB::core)
    add_executable(test_coverage_gap tests/test_coverage_gap.c)
    target_include_directories(test_coverage_gap PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_coverage_gap PRIVATE ShibaDB::core)
    add_executable(test_btree_page_corrupt tests/test_btree_page_corrupt.c)
    target_include_directories(test_btree_page_corrupt PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_btree_page_corrupt PRIVATE ShibaDB::core)
    add_executable(test_codec tests/test_codec.c)
    target_include_directories(test_codec PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_codec PRIVATE ShibaDB::core)
    add_executable(test_file tests/test_file.c)
    target_include_directories(test_file PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_file PRIVATE ShibaDB::core)
    add_executable(test_replace_recover tests/test_replace_recover.c)
    target_include_directories(test_replace_recover PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_replace_recover PRIVATE ShibaDB::core)
    add_executable(test_wal_recover_error tests/test_wal_recover_error.c)
    target_include_directories(test_wal_recover_error PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_wal_recover_error PRIVATE ShibaDB::core)
    add_executable(test_wal_recover_forge tests/test_wal_recover_forge.c)
    target_include_directories(test_wal_recover_forge PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_wal_recover_forge PRIVATE ShibaDB::core)
    add_executable(test_wal_format tests/test_wal_format.c)
    target_include_directories(test_wal_format PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_wal_format PRIVATE ShibaDB::core)
    add_executable(test_wal_index tests/test_wal_index.c)
    target_include_directories(test_wal_index PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_wal_index PRIVATE ShibaDB::core)
    add_executable(test_wal_mode tests/test_wal_mode.c)
    target_include_directories(test_wal_mode PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_wal_mode PRIVATE ShibaDB::core)
    add_executable(test_index_visitor_reentry tests/test_index_visitor_reentry.c)
    target_link_libraries(test_index_visitor_reentry PRIVATE ShibaDB::core)
    add_executable(test_utf8_path tests/test_utf8_path.c)
    target_include_directories(
        test_utf8_path PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(test_utf8_path PRIVATE ShibaDB::core)
    add_executable(test_superblock_store tests/test_superblock_store.c)
    target_include_directories(test_superblock_store PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_superblock_store PRIVATE ShibaDB::core)
    add_executable(test_page tests/test_page.c)
    target_include_directories(test_page PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_page PRIVATE ShibaDB::core)
    add_executable(test_pager tests/test_pager.c)
    target_include_directories(test_pager PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_pager PRIVATE ShibaDB::core)
    add_executable(test_legacy_upgrade_shrink tests/test_legacy_upgrade_shrink.c)
    target_include_directories(test_legacy_upgrade_shrink PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_legacy_upgrade_shrink PRIVATE ShibaDB::core)
    add_executable(test_page_cache tests/test_page_cache.c)
    target_include_directories(test_page_cache PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_page_cache PRIVATE ShibaDB::core)
    add_executable(test_pager_fault tests/test_pager_fault.c)
    target_include_directories(test_pager_fault PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_pager_fault PRIVATE ShibaDB::core)
    add_executable(test_transaction tests/test_transaction.c)
    target_include_directories(test_transaction PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_transaction PRIVATE ShibaDB::core)
    add_executable(test_transaction_crash tests/test_transaction_crash.c)
    target_include_directories(test_transaction_crash PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_transaction_crash PRIVATE ShibaDB::core)
    add_executable(test_btree_page tests/test_btree_page.c)
    target_include_directories(test_btree_page PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_btree_page PRIVATE ShibaDB::core)
    add_executable(test_btree tests/test_btree.c)
    target_include_directories(test_btree PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_btree PRIVATE ShibaDB::core)
    add_executable(test_btree_property tests/test_btree_property.c)
    target_include_directories(test_btree_property PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_btree_property PRIVATE ShibaDB::core)
    add_executable(test_btree_batch tests/test_btree_batch.c)
    target_include_directories(test_btree_batch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_btree_batch PRIVATE ShibaDB::core)
    add_executable(test_btree_reclaim tests/test_btree_reclaim.c)
    target_include_directories(test_btree_reclaim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_btree_reclaim PRIVATE ShibaDB::core)
    add_executable(test_btree_reclaim_overflow tests/test_btree_reclaim_overflow.c)
    target_include_directories(test_btree_reclaim_overflow PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_btree_reclaim_overflow PRIVATE ShibaDB::core)
    add_executable(test_txn_cancel_alloc tests/test_txn_cancel_alloc.c)
    target_include_directories(test_txn_cancel_alloc PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_txn_cancel_alloc PRIVATE ShibaDB::core)
    add_executable(test_crypto tests/test_crypto.c)
    target_include_directories(test_crypto PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_crypto PRIVATE ShibaDB::core)
    add_executable(test_encryption tests/test_encryption.c)
    target_include_directories(test_encryption PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_encryption PRIVATE ShibaDB::core)
    add_executable(test_encryption_fault tests/test_encryption_fault.c)
    target_include_directories(test_encryption_fault PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_encryption_fault PRIVATE ShibaDB::core)
    add_executable(test_engine tests/test_engine.c)
    target_link_libraries(test_engine PRIVATE ShibaDB::core)
    add_executable(test_scan_enumerate tests/test_scan_enumerate.c)
    target_link_libraries(test_scan_enumerate PRIVATE ShibaDB::core)
    add_executable(test_engine_property tests/test_engine_property.c)
    target_link_libraries(test_engine_property PRIVATE ShibaDB::core)
    add_executable(test_engine_fault tests/test_engine_fault.c)
    target_include_directories(test_engine_fault PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_engine_fault PRIVATE ShibaDB::core)
    add_executable(test_engine_concurrency tests/test_engine_concurrency.c)
    target_link_libraries(test_engine_concurrency PRIVATE ShibaDB::core)
    add_executable(test_engine_operations tests/test_engine_operations.c)
    target_include_directories(test_engine_operations PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_engine_operations PRIVATE ShibaDB::core)
    add_executable(test_compact_generation_reseed tests/test_compact_generation_reseed.c)
    target_include_directories(test_compact_generation_reseed PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(test_compact_generation_reseed PRIVATE ShibaDB::core)
    # Whitebox target for BUG#5: the test includes src/engine.c directly to
    # reach the static decoder, so we compile every core source EXCEPT engine.c
    # alongside it. Linking ShibaDB::core here would duplicate engine.c symbols.
    set(SHIBADB_SOURCES_NO_ENGINE ${SHIBADB_SOURCES})
    list(REMOVE_ITEM SHIBADB_SOURCES_NO_ENGINE src/engine.c)
    add_executable(test_object_metadata_bound
        tests/test_object_metadata_bound.c
        ${SHIBADB_SOURCES_NO_ENGINE}
    )
    shibadb_configure_target(test_object_metadata_bound)
    add_executable(test_public_transaction tests/test_public_transaction.c)
    target_link_libraries(test_public_transaction PRIVATE ShibaDB::core)
    add_executable(
        test_public_transaction_property
        tests/test_public_transaction_property.c
    )
    target_link_libraries(
        test_public_transaction_property PRIVATE ShibaDB::core
    )
    add_executable(
        test_public_transaction_concurrency
        tests/test_public_transaction_concurrency.c
    )
    target_link_libraries(
        test_public_transaction_concurrency PRIVATE ShibaDB::core
    )
    add_executable(
        test_public_transaction_fault
        tests/test_public_transaction_fault.c
    )
    target_include_directories(
        test_public_transaction_fault PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(test_public_transaction_fault PRIVATE ShibaDB::core)
    if(UNIX)
        add_executable(
            test_public_transaction_crash
            tests/test_public_transaction_crash.c
        )
        target_include_directories(
            test_public_transaction_crash
            PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
        )
        target_link_libraries(
            test_public_transaction_crash PRIVATE ShibaDB::core
        )
    endif()
    add_executable(test_group_commit tests/test_group_commit.c)
    target_link_libraries(test_group_commit PRIVATE ShibaDB::core)
    add_executable(test_mixed_workload tests/test_mixed_workload.c)
    target_link_libraries(test_mixed_workload PRIVATE ShibaDB::core)
    add_executable(test_compact_encryption tests/test_compact_encryption.c)
    target_link_libraries(test_compact_encryption PRIVATE ShibaDB::core)
    add_executable(test_crash_injection tests/test_crash_injection.c)
    target_link_libraries(test_crash_injection PRIVATE ShibaDB::core)
    add_executable(test_churn_reclaim tests/test_churn_reclaim.c)
    target_link_libraries(test_churn_reclaim PRIVATE ShibaDB::core)
    add_executable(test_index_churn_reclaim tests/test_index_churn_reclaim.c)
    target_link_libraries(test_index_churn_reclaim PRIVATE ShibaDB::core)
    add_executable(test_legacy_index_reclaim tests/test_legacy_index_reclaim.c)
    target_include_directories(
        test_legacy_index_reclaim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(test_legacy_index_reclaim PRIVATE ShibaDB::core)
    add_executable(test_group_commit_batch tests/test_group_commit_batch.c)
    target_include_directories(
        test_group_commit_batch PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src
    )
    target_link_libraries(test_group_commit_batch PRIVATE ShibaDB::core)
    add_executable(
        test_txn_begin_commit_race tests/test_txn_begin_commit_race.c
    )
    target_link_libraries(test_txn_begin_commit_race PRIVATE ShibaDB::core)
    if(MSVC)
        target_compile_options(test_abi PRIVATE /W4 /WX)
        target_compile_options(test_superblock PRIVATE /W4 /WX)
        target_compile_options(test_database_info PRIVATE /W4 /WX)
        target_compile_options(test_convenience PRIVATE /W4 /WX)
        target_compile_options(test_codec PRIVATE /W4 /WX)
        target_compile_options(test_file PRIVATE /W4 /WX)
        target_compile_options(test_utf8_path PRIVATE /W4 /WX)
        target_compile_options(test_superblock_store PRIVATE /W4 /WX)
        target_compile_options(test_page PRIVATE /W4 /WX)
        target_compile_options(test_pager PRIVATE /W4 /WX)
        target_compile_options(test_page_cache PRIVATE /W4 /WX)
        target_compile_options(test_pager_fault PRIVATE /W4 /WX)
        target_compile_options(test_transaction PRIVATE /W4 /WX)
        target_compile_options(test_transaction_crash PRIVATE /W4 /WX)
        target_compile_options(test_btree_page PRIVATE /W4 /WX)
        target_compile_options(test_btree PRIVATE /W4 /WX)
        target_compile_options(test_btree_property PRIVATE /W4 /WX)
        target_compile_options(test_btree_batch PRIVATE /W4 /WX)
        target_compile_options(test_btree_reclaim PRIVATE /W4 /WX)
        target_compile_options(test_btree_reclaim_overflow PRIVATE /W4 /WX)
        target_compile_options(test_txn_cancel_alloc PRIVATE /W4 /WX)
        target_compile_options(test_crypto PRIVATE /W4 /WX)
        target_compile_options(test_encryption PRIVATE /W4 /WX)
        target_compile_options(test_encryption_fault PRIVATE /W4 /WX)
        target_compile_options(test_engine PRIVATE /W4 /WX)
        target_compile_options(test_engine_property PRIVATE /W4 /WX)
        target_compile_options(test_engine_fault PRIVATE /W4 /WX)
        target_compile_options(test_engine_concurrency PRIVATE /W4 /WX)
        target_compile_options(test_engine_operations PRIVATE /W4 /WX)
        target_compile_options(test_public_transaction PRIVATE /W4 /WX)
        target_compile_options(
            test_public_transaction_property PRIVATE /W4 /WX
        )
        target_compile_options(
            test_public_transaction_concurrency PRIVATE /W4 /WX
        )
        target_compile_options(test_public_transaction_fault PRIVATE /W4 /WX)
        target_compile_options(test_group_commit PRIVATE /W4 /WX)
        target_compile_options(test_mixed_workload PRIVATE /W4 /WX)
        target_compile_options(test_compact_encryption PRIVATE /W4 /WX)
        target_compile_options(test_crash_injection PRIVATE /W4 /WX)
        target_compile_options(test_churn_reclaim PRIVATE /W4 /WX)
        target_compile_options(test_index_churn_reclaim PRIVATE /W4 /WX)
        target_compile_options(test_legacy_index_reclaim PRIVATE /W4 /WX)
        target_compile_options(test_group_commit_batch PRIVATE /W4 /WX)
        target_compile_options(test_txn_begin_commit_race PRIVATE /W4 /WX)
        if(UNIX)
            target_compile_options(
                test_public_transaction_crash PRIVATE /W4 /WX
            )
        endif()
    else()
        target_compile_options(test_abi PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_superblock PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_database_info PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_convenience PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_codec PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_file PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_utf8_path PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_superblock_store PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_page PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_pager PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_page_cache PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_pager_fault PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_transaction PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_transaction_crash PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_btree_page PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_btree PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_btree_property PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_btree_batch PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_btree_reclaim PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_btree_reclaim_overflow PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_txn_cancel_alloc PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_crypto PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_encryption PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_encryption_fault PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_engine PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_engine_property PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_engine_fault PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_engine_concurrency PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_engine_operations PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_public_transaction PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_public_transaction_property PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_public_transaction_concurrency PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_public_transaction_fault PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_group_commit PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_mixed_workload PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_compact_encryption PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_crash_injection PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_churn_reclaim PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_index_churn_reclaim PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_legacy_index_reclaim PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_group_commit_batch PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(test_txn_begin_commit_race PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        if(UNIX)
            target_compile_options(test_public_transaction_crash PRIVATE
                -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
            )
        endif()
    endif()
    add_test(NAME abi COMMAND test_abi)
    add_test(NAME superblock COMMAND test_superblock)
    add_test(NAME database_info COMMAND test_database_info)
    add_test(NAME convenience COMMAND test_convenience)
    add_test(NAME superblock_field_diag COMMAND test_superblock_field_diag)
    add_test(NAME coverage_gap COMMAND test_coverage_gap)
    add_test(NAME btree_page_corrupt COMMAND test_btree_page_corrupt)
    add_test(NAME codec COMMAND test_codec)
    add_test(NAME file COMMAND test_file)
    add_test(NAME replace_recover COMMAND test_replace_recover)
    add_test(NAME wal_recover_error COMMAND test_wal_recover_error)
    add_test(NAME wal_recover_forge COMMAND test_wal_recover_forge)
    add_test(NAME wal_format COMMAND test_wal_format)
    add_test(NAME wal_index COMMAND test_wal_index)
    add_test(NAME wal_mode COMMAND test_wal_mode)
    add_test(NAME index_visitor_reentry COMMAND test_index_visitor_reentry)
    add_test(NAME utf8_path COMMAND test_utf8_path)
    add_test(NAME superblock_store COMMAND test_superblock_store)
    add_test(NAME page COMMAND test_page)
    add_test(NAME pager COMMAND test_pager)
    add_test(NAME legacy_upgrade_shrink COMMAND test_legacy_upgrade_shrink)
    add_test(NAME page_cache COMMAND test_page_cache)
    add_test(NAME pager_fault COMMAND test_pager_fault)
    add_test(NAME transaction COMMAND test_transaction)
    add_test(NAME transaction_crash COMMAND test_transaction_crash)
    add_test(NAME btree_page COMMAND test_btree_page)
    add_test(NAME btree COMMAND test_btree)
    add_test(NAME btree_property COMMAND test_btree_property)
    add_test(NAME btree_batch COMMAND test_btree_batch)
    add_test(NAME btree_reclaim COMMAND test_btree_reclaim)
    add_test(NAME btree_reclaim_overflow COMMAND test_btree_reclaim_overflow)
    add_test(NAME txn_cancel_alloc COMMAND test_txn_cancel_alloc)
    add_test(NAME crypto COMMAND test_crypto)
    add_test(NAME encryption COMMAND test_encryption)
    add_test(NAME encryption_fault COMMAND test_encryption_fault)
    add_test(NAME engine COMMAND test_engine)
    add_test(NAME scan_enumerate COMMAND test_scan_enumerate)
    add_test(NAME engine_property COMMAND test_engine_property)
    add_test(NAME engine_fault COMMAND test_engine_fault)
    add_test(NAME engine_concurrency COMMAND test_engine_concurrency)
    add_test(NAME engine_operations COMMAND test_engine_operations)
    add_test(
        NAME compact_generation_reseed COMMAND test_compact_generation_reseed
    )
    add_test(
        NAME object_metadata_bound COMMAND test_object_metadata_bound
    )
    add_test(NAME public_transaction COMMAND test_public_transaction)
    add_test(
        NAME public_transaction_property
        COMMAND test_public_transaction_property
    )
    add_test(
        NAME public_transaction_concurrency
        COMMAND test_public_transaction_concurrency
    )
    add_test(
        NAME public_transaction_fault COMMAND test_public_transaction_fault
    )
    if(UNIX)
        add_test(
            NAME public_transaction_crash
            COMMAND test_public_transaction_crash
        )
    endif()
    add_test(NAME group_commit COMMAND test_group_commit)
    add_test(NAME mixed_workload COMMAND test_mixed_workload)
    add_test(NAME compact_encryption COMMAND test_compact_encryption)
    add_test(NAME crash_injection COMMAND test_crash_injection)
    add_test(NAME churn_reclaim COMMAND test_churn_reclaim)
    add_test(NAME index_churn_reclaim COMMAND test_index_churn_reclaim)
    add_test(NAME legacy_index_reclaim COMMAND test_legacy_index_reclaim)
    add_test(NAME group_commit_batch COMMAND test_group_commit_batch)
    add_test(
        NAME txn_begin_commit_race COMMAND test_txn_begin_commit_race
    )
    # Exercise the installed package, not merely in-tree targets. This catches
    # missing headers, broken exported target names, bad pkg-config metadata,
    # and C/C++ consumer regressions before release.
    set(PC_RELATIVE_PATH
        "${CMAKE_INSTALL_LIBDIR}/pkgconfig/shibadb.pc"
    )
    configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_install_consumer.cmake
        ${CMAKE_CURRENT_BINARY_DIR}/test_install_consumer.cmake
        @ONLY
    )
    add_test(
        NAME install_consumer
        COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -DINSTALL_PREFIX=${CMAKE_CURRENT_BINARY_DIR}/test-install-prefix
            -DGENERATOR=${CMAKE_GENERATOR}
            -DCONFIG=$<CONFIG>
            -P ${CMAKE_CURRENT_BINARY_DIR}/test_install_consumer.cmake
    )

    set_tests_properties(install_consumer PROPERTIES
        LABELS packaging
        RUN_SERIAL TRUE
    )

    # Drive the native CLI end to end (argument parsing, exit codes, KV round
    # trip, encryption, backup/compact) the way a shell script would.
    if(SDB_BUILD_CLI)
        add_test(
            NAME cli
            COMMAND ${CMAKE_COMMAND}
                -DCLI=$<TARGET_FILE:shibadb_cli>
                -DWORKDIR=${CMAKE_CURRENT_BINARY_DIR}/_cli_work
                -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_cli.cmake
        )
    endif()
endif()

if(SDB_ENABLE_SOAK_TESTS)
    # Soak test target is standalone: it does not depend on SDB_TESTING
    # instrumentation hooks and only exercises the public engine API. This
    # lets the `soak` CMake preset ship a Release build (SDB_TESTING=OFF)
    # while still producing a runnable test_engine_soak binary.
    #
    # test_engine_soak uses assert() to check API return codes, so it must
    # be compiled with NDEBUG undefined even under the release preset —
    # otherwise the checks compile out and every sdb_status variable
    # becomes unused-but-set.
    add_executable(test_engine_soak tests/test_engine_soak.c)
    target_link_libraries(test_engine_soak PRIVATE ShibaDB::core)
    if(MSVC)
        target_compile_options(test_engine_soak PRIVATE /W4 /WX /UNDEBUG)
    else()
        target_compile_options(test_engine_soak PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
            -UNDEBUG
        )
    endif()
    add_test(NAME engine_soak COMMAND test_engine_soak)
    set_tests_properties(engine_soak PROPERTIES
        LABELS soak
        TIMEOUT 21600
    )
endif()

if(SDB_BUILD_BENCHMARKS)
    # Benchmark harness is standalone (public engine API only) and is built
    # optimized with NDEBUG for representative numbers; it reports errors via
    # exit() rather than assert(), so NDEBUG is left defined.
    add_executable(bench_kv benchmarks/bench_kv.c)
    target_link_libraries(bench_kv PRIVATE ShibaDB::core)
    add_executable(bench_churn benchmarks/bench_churn.c)
    target_link_libraries(bench_churn PRIVATE ShibaDB::core)
    find_package(Threads REQUIRED)
    add_executable(bench_concurrent benchmarks/bench_concurrent.c)
    target_link_libraries(
        bench_concurrent PRIVATE ShibaDB::core Threads::Threads
    )
    if(MSVC)
        target_compile_options(bench_kv PRIVATE /W4 /WX)
        target_compile_options(bench_churn PRIVATE /W4 /WX)
        target_compile_options(bench_concurrent PRIVATE /W4 /WX)
    else()
        target_compile_options(bench_kv PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(bench_churn PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
        target_compile_options(bench_concurrent PRIVATE
            -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Werror
        )
    endif()
endif()

if(SDB_ENABLE_FUZZERS)
    set(SDB_FUZZ_TARGETS
        fuzz_superblock
        fuzz_page
        fuzz_btree_page
        fuzz_xchacha20poly1305
        fuzz_encrypted_envelope
        fuzz_wal_recover
        fuzz_pager_open
    )
    # In OSS-Fuzz / ClusterFuzzLite builds the harness supplies its own
    # sanitizer flags via $CFLAGS/$CXXFLAGS and the fuzzing engine object
    # via $LIB_FUZZING_ENGINE. Set SDB_FUZZ_LINK_FLAGS to that value in
    # such builds; leave it empty locally to fall back to libFuzzer +
    # ASan/UBSan wired up in this file.
    set(SDB_FUZZ_LINK_FLAGS "" CACHE STRING
        "Link flags for fuzz targets. Empty = local libFuzzer build; \
set to $LIB_FUZZING_ENGINE inside OSS-Fuzz.")
    foreach(target ${SDB_FUZZ_TARGETS})
        add_executable(${target} fuzz/${target}.c)
        target_link_libraries(${target} PRIVATE ShibaDB::core)
        target_include_directories(${target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
        if(SDB_FUZZ_LINK_FLAGS STREQUAL "")
            target_compile_options(${target} PRIVATE
                -fsanitize=fuzzer,address,undefined -fno-omit-frame-pointer
            )
            target_link_options(${target} PRIVATE
                -fsanitize=fuzzer,address,undefined
            )
        else()
            target_link_libraries(${target} PRIVATE ${SDB_FUZZ_LINK_FLAGS})
        endif()
    endforeach()
endif()
