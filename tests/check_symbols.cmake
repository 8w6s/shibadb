# Shared-library ABI symbol gate.
#
# Compares the public symbols exported by the shared library against the
# frozen allowlist in abi/symbols-v1.txt, exactly and in both directions: a
# symbol exported but absent from the allowlist fails (unreviewed ABI growth),
# and an allowlisted symbol no longer exported fails too (ABI break).
# docs/ABI.md calls that list the authoritative ABI v1 surface; unless this
# runs as a test the claim is decorative, which is how 20 symbols drifted out
# of the allowlist while every gate stayed green.
#
# Required inputs:
#   LIBRARY   shared library to inspect ($<TARGET_FILE:shibadb_shared>)
#   EXPECTED  path to the allowlist
# Optional input:
#   NM        symbol dumper; when omitted, llvm-nm, nm and dumpbin are tried in
#             turn so Linux, macOS and MSVC need no per-platform wiring here.
#
# Only defined, external symbols matching ^_?sdb_ are compared. Synthesised
# runtime symbols (_init/_fini/__bss_start on ELF, __mh_execute_header on
# Mach-O) are not part of the public contract and vary by toolchain and link
# mode. The prefix filter is also what makes the plain-nm fallback safe:
# internal functions are sdb_-prefixed too, but hidden visibility makes them
# locals, and the type-aware parse below keeps only uppercase defined symbols.

if(NOT DEFINED LIBRARY OR NOT DEFINED EXPECTED)
    message(FATAL_ERROR "LIBRARY and EXPECTED are required")
endif()
if(NOT EXISTS "${LIBRARY}")
    message(FATAL_ERROR "Shared library not found: ${LIBRARY}")
endif()
if(NOT EXISTS "${EXPECTED}")
    message(FATAL_ERROR "Allowlist not found: ${EXPECTED}")
endif()

# An explicitly supplied NM always wins so a cross-build can point at its own
# binutils.
if(NOT DEFINED NM OR NM STREQUAL "")
    foreach(sdb_candidate llvm-nm nm dumpbin)
        find_program(SDB_DUMPER_${sdb_candidate} ${sdb_candidate})
        if(SDB_DUMPER_${sdb_candidate})
            set(NM "${SDB_DUMPER_${sdb_candidate}}")
            break()
        endif()
    endforeach()
endif()
if(NOT DEFINED NM OR NM STREQUAL "")
    message(FATAL_ERROR
        "No symbol dumper found (searched: llvm-nm, nm, dumpbin). "
        "Pass -DNM=<tool> to run the ABI symbol gate."
    )
endif()
get_filename_component(sdb_nm_name "${NM}" NAME)

# GNU/llvm nm accept -D --defined-only on ELF and PE; Apple's cctools nm
# rejects -D and wants -gU; MSVC has no nm and needs dumpbin /EXPORTS. Each
# style is tried until one exits 0 AND its output mentions an sdb_ symbol, so a
# tool that accepts a flag but misreads the format falls through instead of
# silently yielding an empty set -- an empty set would otherwise look like a
# library with no public API and pass against an empty allowlist.
set(sdb_dump_style "")
set(sdb_nm_output "")
set(sdb_nm_error "")
if(sdb_nm_name MATCHES "dumpbin")
    execute_process(
        COMMAND "${NM}" /EXPORTS "${LIBRARY}"
        RESULT_VARIABLE sdb_nm_status
        OUTPUT_VARIABLE sdb_nm_output
        ERROR_VARIABLE sdb_nm_error
    )
    if(sdb_nm_status EQUAL 0 AND sdb_nm_output MATCHES "sdb_")
        set(sdb_dump_style "dumpbin")
    endif()
else()
    foreach(sdb_style "-D;--defined-only" "-gU" "--dynamic;--defined-only" "")
        if(sdb_style STREQUAL "")
            set(sdb_style_args "")
        else()
            set(sdb_style_args "${sdb_style}")
        endif()
        execute_process(
            COMMAND "${NM}" ${sdb_style_args} "${LIBRARY}"
            RESULT_VARIABLE sdb_nm_status
            OUTPUT_VARIABLE sdb_nm_output
            ERROR_VARIABLE sdb_nm_error
        )
        if(sdb_nm_status EQUAL 0 AND sdb_nm_output MATCHES "sdb_")
            set(sdb_dump_style "nm")
            break()
        endif()
    endforeach()
endif()

if(sdb_dump_style STREQUAL "")
    message(FATAL_ERROR
        "Could not read any sdb_ symbol from ${LIBRARY} with ${NM}.\n"
        "Last error: ${sdb_nm_error}"
    )
endif()

string(REPLACE "\n" ";" sdb_nm_lines "${sdb_nm_output}")
set(actual)
foreach(line IN LISTS sdb_nm_lines)
    if(sdb_dump_style STREQUAL "dumpbin")
        # dumpbin /EXPORTS rows: ordinal hint RVA name [forwarder]
        if(line MATCHES "^[ \t]*[0-9]+[ \t]+[0-9A-Fa-f]+[ \t]+[0-9A-Fa-f]+[ \t]+([A-Za-z_][A-Za-z0-9_]*)")
            list(APPEND actual "${CMAKE_MATCH_1}")
        endif()
    elseif(line MATCHES "[ \t]([A-Za-z])[ \t]+([A-Za-z_][A-Za-z0-9_]*)$")
        # nm row: [address] type name. Keep external defined symbols only;
        # U/u/v/w are undefined or weak-undefined and lowercase types are
        # local. Capture both groups before the next MATCHES -- any further
        # regex test overwrites CMAKE_MATCH_n in place.
        set(sdb_sym_type "${CMAKE_MATCH_1}")
        set(sdb_sym_name "${CMAKE_MATCH_2}")
        if(NOT sdb_sym_type STREQUAL "U" AND NOT sdb_sym_type STREQUAL "u"
           AND NOT sdb_sym_type STREQUAL "v" AND NOT sdb_sym_type STREQUAL "w")
            list(APPEND actual "${sdb_sym_name}")
        endif()
    elseif(line MATCHES "[ \t]([A-Za-z_][A-Za-z0-9_]*)$")
        # No type column (some llvm-nm formats); the prefix filter covers it.
        list(APPEND actual "${CMAKE_MATCH_1}")
    endif()
endforeach()

# Drop the Mach-O leading underscore, then keep public entry points.
set(sdb_public)
foreach(symbol IN LISTS actual)
    string(REGEX REPLACE "^_" "" symbol "${symbol}")
    if(symbol MATCHES "^sdb_")
        list(APPEND sdb_public "${symbol}")
    endif()
endforeach()
list(REMOVE_DUPLICATES sdb_public)
list(SORT sdb_public)

# Allowlist: one symbol per line, '#' starts a comment, blank lines ignored.
file(STRINGS "${EXPECTED}" sdb_expected_raw)
set(expected)
foreach(line IN LISTS sdb_expected_raw)
    string(STRIP "${line}" line)
    if(line STREQUAL "" OR line MATCHES "^#")
        continue()
    endif()
    string(REGEX REPLACE "^_" "" line "${line}")
    list(APPEND expected "${line}")
endforeach()
list(SORT expected)

list(LENGTH sdb_public sdb_actual_count)
list(LENGTH expected sdb_expected_count)

if(NOT sdb_public STREQUAL expected)
    set(sdb_missing "")
    set(sdb_extra "")
    foreach(symbol IN LISTS expected)
        if(NOT symbol IN_LIST sdb_public)
            list(APPEND sdb_missing "${symbol}")
        endif()
    endforeach()
    foreach(symbol IN LISTS sdb_public)
        if(NOT symbol IN_LIST expected)
            list(APPEND sdb_extra "${symbol}")
        endif()
    endforeach()
    if(sdb_missing STREQUAL "")
        set(sdb_missing "(none)")
    else()
        string(REPLACE ";" "\n  " sdb_missing "${sdb_missing}")
    endif()
    if(sdb_extra STREQUAL "")
        set(sdb_extra "(none)")
    else()
        string(REPLACE ";" "\n  " sdb_extra "${sdb_extra}")
    endif()
    message(FATAL_ERROR
        "Shared ABI symbol mismatch (dumper: ${NM}, style: ${sdb_dump_style}).\n"
        "Allowlist: ${EXPECTED} (${sdb_expected_count} symbols)\n"
        "Library:   ${LIBRARY} (${sdb_actual_count} public symbols exported)\n"
        "Allowlisted but NOT exported -- ABI break:\n  ${sdb_missing}\n"
        "Exported but NOT allowlisted -- unreviewed ABI growth; add to "
        "${EXPECTED} after review:\n  ${sdb_extra}"
    )
endif()

message(STATUS
    "ABI symbol gate OK: ${sdb_actual_count} exported public symbols match "
    "${EXPECTED} (dumper: ${sdb_nm_name}, style: ${sdb_dump_style})"
)
