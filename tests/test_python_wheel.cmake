if(NOT DEFINED PYTHON OR NOT DEFINED SOURCE_DIR OR NOT DEFINED BUILD_DIR
   OR NOT DEFINED LIBRARY)
    message(FATAL_ERROR "Missing Python wheel test arguments")
endif()
set(dist "${BUILD_DIR}/python-dist")
set(venv "${BUILD_DIR}/python-venv")
file(REMOVE_RECURSE "${dist}" "${venv}")
execute_process(
    COMMAND "${PYTHON}" "${SOURCE_DIR}/python/build_wheel.py" "${dist}"
    RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Wheel build failed")
endif()
file(GLOB wheels "${dist}/shibadb-*.whl")
list(LENGTH wheels wheel_count)
if(NOT wheel_count EQUAL 1)
    message(FATAL_ERROR "Expected exactly one wheel")
endif()
list(GET wheels 0 wheel)
execute_process(
    COMMAND "${PYTHON}" -m venv "${venv}"
    RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Virtual environment creation failed")
endif()
if(WIN32)
    set(venv_python "${venv}/Scripts/python.exe")
else()
    set(venv_python "${venv}/bin/python")
endif()
execute_process(
    COMMAND "${venv_python}" -m pip install --no-deps "${wheel}"
    RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Wheel installation failed")
endif()
execute_process(
    COMMAND "${CMAKE_COMMAND}" -E env
        "SHIBADB_LIBRARY=${LIBRARY}"
        "${venv_python}" "${SOURCE_DIR}/tests/test_python_binding.py"
    WORKING_DIRECTORY "${BUILD_DIR}"
    RESULT_VARIABLE status
)
if(NOT status EQUAL 0)
    message(FATAL_ERROR "Installed wheel conformance failed")
endif()
