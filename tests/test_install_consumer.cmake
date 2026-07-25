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
    set(shared_executable "${consumer_build}/Release/consumer_shared.exe")
    set(static_executable "${consumer_build}/Release/consumer_static.exe")
    set(cpp_executable "${consumer_build}/Release/consumer_cpp.exe")
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
