# Freeze the shared library's exported symbol table against the ABI v1 allowlist
# (abi/symbols-v1.txt). Driven by CTest as `-P check_symbols.cmake` with
# -DLIBRARY=<shared library> -DEXPECTED=<allowlist> plus the extractor the
# library's object format needs: -DNM=<nm> for ELF/Mach-O, -DOBJDUMP=<objdump>
# for PE (a DLL carries no dynamic symbol table for `nm -D` to read, so the
# export directory has to be dumped instead).
#
# Hidden visibility (SDB_HIDE_INTERNAL / -fvisibility=hidden) means every name
# here was deliberately marked SDB_API, so an unexpected entry is a real leak of
# an internal pager/WAL/crypto symbol and a missing one is a break in the
# published ABI. The comparison is therefore exact in both directions.

if(NOT DEFINED LIBRARY OR NOT DEFINED EXPECTED)
    message(FATAL_ERROR "LIBRARY and EXPECTED are required")
endif()
if(NOT EXISTS "${LIBRARY}")
    message(FATAL_ERROR "LIBRARY does not exist: ${LIBRARY}")
endif()
if(NOT EXISTS "${EXPECTED}")
    message(FATAL_ERROR "EXPECTED does not exist: ${EXPECTED}")
endif()

# Pick the extractor from the library's object format rather than from the host,
# so a cross build is read the way its output is actually laid out.
#   PE      exports live in .edata; `nm -D` reports "no symbols".
#   Mach-O  decorates every C symbol with a leading underscore.
#   ELF     the plain case: -D reports the dynamic table.
if(LIBRARY MATCHES "[.](dll|DLL)$")
    set(sdb_format "PE")
elseif(LIBRARY MATCHES "[.]dylib$")
    set(sdb_format "MACHO")
else()
    set(sdb_format "ELF")
endif()

if(sdb_format STREQUAL "PE")
    if(NOT DEFINED OBJDUMP OR OBJDUMP STREQUAL "")
        message(FATAL_ERROR "OBJDUMP is required to read PE exports")
    endif()
    set(sdb_tool "${OBJDUMP}")
    set(sdb_tool_args -p "${LIBRARY}")
else()
    if(NOT DEFINED NM OR NM STREQUAL "")
        message(FATAL_ERROR "NM is required to read ${sdb_format} symbols")
    endif()
    set(sdb_tool "${NM}")
    if(sdb_format STREQUAL "MACHO")
        # -gU: external, defined only. Mach-O has no ELF-style dynamic table.
        set(sdb_tool_args -gU "${LIBRARY}")
    else()
        set(sdb_tool_args -D --defined-only "${LIBRARY}")
    endif()
endif()

# Symbols injected by the toolchain's instrumentation runtimes, not by ShibaDB.
# A coverage or sanitizer build links these into the shared object, so a literal
# comparison fails on those presets even though the ShibaDB surface is correct
# (see docs/COVERAGE.md: the `coverage` preset added
# __llvm_write_custom_profile). They are filtered by prefix rather than
# suppressing the whole check, so a genuine leak on an instrumented build is
# still caught. Everything not matched here must appear in the allowlist.
set(SDB_TOOLCHAIN_SYMBOL_PATTERNS
    "^__llvm_"          # LLVM profile/coverage runtime
    "^__prof[cdnv]"     # LLVM raw profile counters/data/names
    "^__gcov"           # GCC gcov runtime
    "^_?_gcno"          # GCC coverage notes
    "^__asan_"          # AddressSanitizer entry points
    "^__ubsan_"         # UndefinedBehaviorSanitizer
    "^__msan_"          # MemorySanitizer
    "^__tsan_"          # ThreadSanitizer
    "^__lsan_"          # LeakSanitizer
    "^__sanitizer_"     # common sanitizer interface
    # Section-bracketing symbols the sanitizers emit around their instrumented
    # globals/counters metadata. These are NOT __-prefixed -- ASan emits
    # _start_asan_globals / _stop_asan_globals -- so the prefixes above miss
    # them and the sanitize preset failed on exactly this.
    "^_(start|stop)_[a-z]+_(globals|counters|array|sections)$"
)

execute_process(
    COMMAND "${sdb_tool}" ${sdb_tool_args}
    RESULT_VARIABLE tool_status
    OUTPUT_VARIABLE tool_output
    ERROR_VARIABLE tool_error
)
if(NOT tool_status EQUAL 0)
    message(FATAL_ERROR "${sdb_tool} failed: ${tool_error}")
endif()
string(REPLACE "\n" ";" tool_lines "${tool_output}")

set(actual)
set(filtered)
# PE: only the [Ordinal/Name Pointer] Table block names exported symbols; the
# rest of `objdump -p` (headers, the import tables, section dumps) must not be
# scraped, or unrelated words would be read as exports.
set(in_pe_export_names FALSE)
foreach(line IN LISTS tool_lines)
    string(REPLACE "\r" "" line "${line}")
    set(symbol "")
    if(sdb_format STREQUAL "PE")
        if(line MATCHES "[[]Ordinal/Name Pointer[]] Table")
            set(in_pe_export_names TRUE)
            continue()
        endif()
        if(in_pe_export_names)
            # e.g. "	[   0] +base[   1]  0000 sdb_abi_version"
            if(line MATCHES "[+]base[[][ \t]*[0-9]+[]][ \t]+[0-9a-fA-F]+[ \t]+([A-Za-z_][A-Za-z0-9_]*)[ \t]*$")
                set(symbol "${CMAKE_MATCH_1}")
            elseif(line MATCHES "^[ \t]*$")
                set(in_pe_export_names FALSE)
            endif()
        endif()
    elseif(line MATCHES "[ \t][A-Za-z][ \t]+_([A-Za-z_][A-Za-z0-9_]*)$")
        # Mach-O: strip the leading underscore the ABI adds to every C symbol.
        set(symbol "${CMAKE_MATCH_1}")
    elseif(line MATCHES "[ \t]([A-Za-z_][A-Za-z0-9_]*)$")
        set(symbol "${CMAKE_MATCH_1}")
    endif()
    if(NOT symbol STREQUAL "")
        set(is_toolchain FALSE)
        foreach(pattern IN LISTS SDB_TOOLCHAIN_SYMBOL_PATTERNS)
            if(symbol MATCHES "${pattern}")
                set(is_toolchain TRUE)
                break()
            endif()
        endforeach()
        if(is_toolchain)
            list(APPEND filtered "${symbol}")
        else()
            list(APPEND actual "${symbol}")
        endif()
    endif()
endforeach()
list(SORT actual)
list(REMOVE_DUPLICATES actual)
if(NOT actual)
    # An empty read means the extractor did not understand the file, which would
    # otherwise "pass" against an empty allowlist and silently retire the gate.
    message(FATAL_ERROR
        "no exported symbols read from ${LIBRARY} via ${sdb_tool} "
        "(format ${sdb_format}) -- the extractor, not the ABI, is wrong")
endif()

file(STRINGS "${EXPECTED}" expected_lines)
# The allowlist is stored with LF, but a worktree checked out on Windows (or
# with core.autocrlf) carries CRLF; strip any stray CR so the comparison is not
# defeated by a line ending. Blank lines (a trailing newline) are dropped.
set(expected)
foreach(line IN LISTS expected_lines)
    string(REPLACE "\r" "" line "${line}")
    if(NOT line STREQUAL "")
        list(APPEND expected "${line}")
    endif()
endforeach()
list(SORT expected)
list(REMOVE_DUPLICATES expected)

if(NOT actual STREQUAL expected)
    # Report the difference, not two 72-entry lists: "exported but not allowed"
    # is a leak to fix or allowlist, "allowed but not exported" is a symbol that
    # lost its SDB_API or was renamed.
    set(unexpected "${actual}")
    if(expected AND unexpected)
        list(REMOVE_ITEM unexpected ${expected})
    endif()
    set(missing "${expected}")
    if(actual AND missing)
        list(REMOVE_ITEM missing ${actual})
    endif()
    message(FATAL_ERROR
        "Shared ABI symbol mismatch (${sdb_format})\n"
        "Exported but not in the allowlist: ${unexpected}\n"
        "In the allowlist but not exported: ${missing}\n"
        "Ignored toolchain-runtime symbols: ${filtered}"
    )
endif()
if(filtered)
    message(STATUS
        "check_symbols: ignored toolchain-runtime symbols: ${filtered}"
    )
endif()
list(LENGTH actual actual_count)
message(STATUS
    "ShibaDB ABI symbol allowlist: OK (${actual_count} exports, ${sdb_format})")
