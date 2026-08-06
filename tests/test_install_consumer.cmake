if(NOT DEFINED SOURCE_DIR OR NOT DEFINED BUILD_DIR
   OR NOT DEFINED INSTALL_PREFIX OR NOT DEFINED GENERATOR)
    message(FATAL_ERROR "Missing install-consumer arguments")
endif()

set(consumer_build "${BUILD_DIR}/external-consumer")
file(REMOVE_RECURSE "${INSTALL_PREFIX}" "${consumer_build}")

set(install_command
    "${CMAKE_COMMAND}" --install "${BUILD_DIR}" --prefix "${INSTALL_PREFIX}"
)
if(DEFINED CONFIG AND NOT CONFIG STREQUAL "")
    list(APPEND install_command --config "${CONFIG}")
endif()
execute_process(
    COMMAND ${install_command}
    RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "ShibaDB install failed")
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}"
        -S "${SOURCE_DIR}/tests/consumer"
        -B "${consumer_build}"
        -G "${GENERATOR}"
        "-DCMAKE_PREFIX_PATH=${INSTALL_PREFIX}"
        "-DCMAKE_BUILD_TYPE=Release"
    RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "External consumer configure failed")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" --build "${consumer_build}" --config Release
    RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "External consumer build failed")
endif()

if(WIN32)
    # Multi-config generators (Visual Studio) place binaries under a per-config
    # subdirectory; single-config generators (Ninja, Makefiles) place them
    # directly in the build tree. Pick whichever actually exists.
    if(EXISTS "${consumer_build}/Release/consumer_shared.exe")
        set(consumer_bindir "${consumer_build}/Release")
    else()
        set(consumer_bindir "${consumer_build}")
    endif()
    set(shared_executable "${consumer_bindir}/consumer_shared.exe")
    set(static_executable "${consumer_bindir}/consumer_static.exe")
    set(cpp_executable "${consumer_bindir}/consumer_cpp.exe")
    # The shared consumer links shibadb.dll, installed to <prefix>/bin. Windows
    # has no rpath; the loader searches the executable's own directory first, so
    # stage the DLL beside the consumer rather than mutating PATH.
    file(COPY "${INSTALL_PREFIX}/bin/shibadb.dll" DESTINATION "${consumer_bindir}")
else()
    set(shared_executable "${consumer_build}/consumer_shared")
    set(static_executable "${consumer_build}/consumer_static")
    set(cpp_executable "${consumer_build}/consumer_cpp")
endif()
execute_process(COMMAND "${shared_executable}" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Installed shared consumer failed")
endif()
execute_process(COMMAND "${static_executable}" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Installed static consumer failed")
endif()
execute_process(COMMAND "${cpp_executable}" RESULT_VARIABLE status)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Installed C++ header consumer failed")
endif()

if(NOT EXISTS "${INSTALL_PREFIX}/@PC_RELATIVE_PATH@")
    message(FATAL_ERROR "pkg-config metadata was not installed")
endif()
