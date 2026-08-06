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

# --- misuse: non-zero, not a crash ---
run(1 out get "${DB}")
run(1 out bogus-command)
run(1 out scan "${DB}" users --nope)

file(REMOVE_RECURSE "${WORKDIR}")
message(STATUS "shibadb CLI conformance: OK")
