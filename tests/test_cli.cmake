# Conformance test for the native `shibadb` command-line tool.
#
# Driven by CTest as `-P test_cli.cmake` with -DCLI=<path> -DWORKDIR=<dir>.
# Runs the CLI end to end the way a shell script would and asserts on exit
# codes and stdout. This is the only automated coverage of the CLI's argument
# parsing and status-to-exit-code mapping, which the C unit tests never touch.

if(NOT DEFINED CLI)
    message(FATAL_ERROR "test_cli.cmake requires -DCLI=<path to shibadb>")
endif()
if(NOT DEFINED WORKDIR)
    set(WORKDIR "${CMAKE_CURRENT_BINARY_DIR}/_cli_work")
endif()
file(REMOVE_RECURSE "${WORKDIR}")
file(MAKE_DIRECTORY "${WORKDIR}")

set(DB "${WORKDIR}/t.shiba")
set(ENC "${WORKDIR}/enc.shiba")
set(BAK "${WORKDIR}/bak.shiba")
set(PW "correct horse battery staple")

# run(<expect_rc> <out_var> ARGS...): run the CLI, assert the exit code, and
# return captured stdout in <out_var>.
function(run expect_rc out_var)
    execute_process(
        COMMAND "${CLI}" ${ARGN}
        OUTPUT_VARIABLE _out
        ERROR_VARIABLE _err
        RESULT_VARIABLE _rc
    )
    if(NOT _rc EQUAL expect_rc)
        message(FATAL_ERROR
            "args='${ARGN}' expected rc=${expect_rc} got rc=${_rc}\n"
            "stdout: ${_out}\nstderr: ${_err}")
    endif()
    set(${out_var} "${_out}" PARENT_SCOPE)
endfunction()

function(expect_contains haystack needle label)
    string(FIND "${haystack}" "${needle}" _pos)
    if(_pos EQUAL -1)
        message(FATAL_ERROR "${label}: expected to find '${needle}' in:\n${haystack}")
    endif()
endfunction()

# expect_exact(<actual> <expected> <label>): compare whole stdout, ignoring the
# CR the Windows CRT adds to every '\n' written in text mode. Used where the
# ORDER of lines is the property under test and `expect_contains` would pass
# regardless of it.
function(expect_exact actual expected label)
    string(REPLACE "\r" "" _actual "${actual}")
    if(NOT _actual STREQUAL expected)
        message(FATAL_ERROR
            "${label}: expected\n'${expected}'\ngot\n'${_actual}'")
    endif()
endfunction()

# version
run(0 out version)
expect_contains("${out}" "shibadb " "version banner")

# --- plaintext lifecycle ---
run(0 out create "${DB}")
run(0 out put "${DB}" users alice "{role:admin}")
run(0 out put "${DB}" users bob "v-bob")
run(0 out put "${DB}" users carol "v-carol")

run(0 out get "${DB}" users alice)
if(NOT out STREQUAL "{role:admin}")
    message(FATAL_ERROR "get alice: got '${out}'")
endif()

# missing key -> non-zero, no stdout
run(2 out get "${DB}" users nope)
if(NOT out STREQUAL "")
    message(FATAL_ERROR "missing get printed: '${out}'")
endif()

# scan: ascending key<TAB>value lines
run(0 out scan "${DB}" users)
expect_contains("${out}" "alice\t{role:admin}" "scan alice")
expect_contains("${out}" "bob\tv-bob" "scan bob")
expect_contains("${out}" "carol\tv-carol" "scan carol")

# prefix scan
run(0 out scan "${DB}" users --prefix b)
expect_contains("${out}" "bob\tv-bob" "prefix scan")

# reverse scan: descending key order (carol, bob, alice)
run(0 out scan "${DB}" users --reverse)
expect_exact("${out}"
    "carol\tv-carol\nbob\tv-bob\nalice\t{role:admin}\n" "reverse scan order")

# reverse + limit takes the GREATEST n keys, not the first n flipped
run(0 out scan "${DB}" users --reverse --limit 2)
expect_exact("${out}" "carol\tv-carol\nbob\tv-bob\n" "reverse --limit 2")

# forward --limit 2 for contrast: the smallest two keys
run(0 out scan "${DB}" users --limit 2)
expect_exact("${out}" "alice\t{role:admin}\nbob\tv-bob\n" "forward --limit 2")

# --reverse composes with --prefix
run(0 out scan "${DB}" users --prefix c --reverse)
expect_exact("${out}" "carol\tv-carol\n" "reverse prefix scan")

# namespaces
run(0 out namespaces "${DB}")
expect_contains("${out}" "users" "namespaces")

# delete then miss
run(0 out del "${DB}" users bob)
run(2 out get "${DB}" users bob)

# verify + info
run(0 out verify "${DB}")
expect_contains("${out}" "verify: OK" "verify")
run(0 out info "${DB}")
expect_contains("${out}" "encrypted:      no" "info plaintext")

# health opens the engine and reports the sdb_database_info snapshot
run(0 out health "${DB}")
expect_contains("${out}" "health: OK" "health")
expect_contains("${out}" "encrypted:      no" "health plaintext")

# backup produces a readable copy
run(0 out backup "${DB}" "${BAK}")
run(0 out get "${BAK}" users alice)
if(NOT out STREQUAL "{role:admin}")
    message(FATAL_ERROR "backup copy get: '${out}'")
endif()

# compact keeps data
run(0 out compact "${DB}")
run(0 out scan "${DB}" users)
expect_contains("${out}" "alice\t{role:admin}" "post-compact")

# convenience verbs: exists / count / incr
run(0 out exists "${DB}" users alice)
expect_contains("${out}" "yes" "exists yes")
run(3 out exists "${DB}" users ghost)   # exit 3 = absent
expect_contains("${out}" "no" "exists no")
run(0 out count "${DB}" users)
expect_contains("${out}" "2" "count users")
run(0 out incr "${DB}" counters hits)
expect_contains("${out}" "1" "incr default 1")
run(0 out incr "${DB}" counters hits 5)
expect_contains("${out}" "6" "incr by 5")

# --- encrypted lifecycle ---
run(0 out create "${ENC}" --password "${PW}")
run(0 out put "${ENC}" s k1 topsecret --password "${PW}")
run(0 out get "${ENC}" s k1 --password "${PW}")
if(NOT out STREQUAL "topsecret")
    message(FATAL_ERROR "encrypted get: '${out}'")
endif()
# wrong password fails
run(2 out get "${ENC}" s k1 --password wrong)
# info without password reports encryption
run(0 out info "${ENC}")
expect_contains("${out}" "encrypted:      yes" "info encrypted")
# compact + verify encrypted
run(0 out compact "${ENC}" --password "${PW}")
run(0 out verify "${ENC}" --password "${PW}")
expect_contains("${out}" "verify: OK" "encrypted verify")
run(0 out health "${ENC}" --password "${PW}")
expect_contains("${out}" "encrypted:      yes" "health encrypted")

# --- document store: mkindex / docput (+index) / find / docget / docdel ---
run(0 out mkindex "${DB}" people by_role)
run(0 out docput "${DB}" people u1 "{\"n\":1}" --index by_role=admin)
run(0 out docput "${DB}" people u2 "{\"n\":2}" --index by_role=admin)
run(0 out docput "${DB}" people u3 "{\"n\":3}" --index by_role=user)
run(0 out docget "${DB}" people u1)
expect_contains("${out}" "{\"n\":1}" "docget body")
run(0 out find "${DB}" people by_role admin)
expect_contains("${out}" "u1" "find match u1")
expect_contains("${out}" "u2" "find match u2")
run(0 out docdel "${DB}" people u1)
run(2 out docget "${DB}" people u1)   # deleted -> not found (exit 2)

# --- misuse: non-zero, not a crash ---
run(1 out get "${DB}")
run(1 out bogus-command)
run(1 out scan "${DB}" users --nope)

file(REMOVE_RECURSE "${WORKDIR}")
message(STATUS "shibadb CLI conformance: OK")
