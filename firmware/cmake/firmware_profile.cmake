set(_profile_root "${CMAKE_CURRENT_LIST_DIR}/..")
include("$ENV{IDF_PATH}/tools/cmake/version.cmake")
if(NOT ((IDF_VERSION_MAJOR EQUAL 5 AND IDF_VERSION_MINOR EQUAL 5) OR
        (IDF_VERSION_MAJOR EQUAL 6 AND IDF_VERSION_MINOR EQUAL 1)))
    message(FATAL_ERROR "Agent Link supports ESP-IDF 5.5.x and 6.1.x")
endif()
if(IDF_VERSION_MAJOR GREATER_EQUAL 6)
    set(DEPENDENCIES_LOCK "${_profile_root}/dependencies.idf6.lock")
else()
    set(DEPENDENCIES_LOCK "${_profile_root}/dependencies.lock")
endif()
if(NOT DEFINED SDKCONFIG OR "${SDKCONFIG}" STREQUAL "")
    set(SDKCONFIG "${_profile_root}/sdkconfig")
endif()
get_filename_component(SDKCONFIG "${SDKCONFIG}" ABSOLUTE BASE_DIR "${_profile_root}")

if(PYTHON)
    set(_profile_python "${PYTHON}")
else()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(_profile_python "${Python3_EXECUTABLE}")
endif()
execute_process(
    COMMAND "${_profile_python}" "${_profile_root}/tools/build_firmware.py"
            --resolve "--sdkconfig=${SDKCONFIG}" "--fallback=${AGENT_LINK_PROFILE}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _selected_profile ERROR_VARIABLE _error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "${_error}")
endif()
set(AGENT_LINK_PROFILE "${_selected_profile}" CACHE STRING "Agent Link firmware profile" FORCE)

file(READ "${_profile_root}/profiles/profiles.json" _profile_catalog)
string(JSON _profile GET "${_profile_catalog}" "${AGENT_LINK_PROFILE}")
string(JSON _profile_target GET "${_profile}" target)
set(ENV{AGENT_LINK_PROFILE_FILE} "${_profile_root}/profiles/profiles.json")
set(ENV{AGENT_LINK_BUILD_PROFILE} "${AGENT_LINK_PROFILE}")
string(JSON _profile_backend GET "${_profile}" backend)
set(ENV{AGENT_LINK_BUILD_BACKEND} "${_profile_backend}")
if(DEFINED IDF_TARGET AND NOT "${IDF_TARGET}" STREQUAL "${_profile_target}")
    message(FATAL_ERROR "Selected ${AGENT_LINK_PROFILE} requires ${_profile_target}. Run python tools/build_firmware.py and select it from the menu to change chips.")
endif()
set(IDF_TARGET "${_profile_target}" CACHE STRING "Profile target" FORCE)

execute_process(
    COMMAND "${_profile_python}" "${_profile_root}/tools/build_firmware.py"
            "${AGENT_LINK_PROFILE}" --prepare "--sdkconfig=${SDKCONFIG}"
    RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error
    OUTPUT_STRIP_TRAILING_WHITESPACE)
if(NOT _result EQUAL 0)
    message(FATAL_ERROR "${_error}")
endif()
if(_output)
    message(STATUS "${_output}")
endif()

set(SDKCONFIG_DEFAULTS "${_profile_root}/sdkconfig.defaults")
string(JSON _profile_defaults_count LENGTH "${_profile}" defaults)
math(EXPR _profile_defaults_last "${_profile_defaults_count} - 1")
foreach(_index RANGE ${_profile_defaults_last})
    string(JSON _defaults GET "${_profile}" defaults ${_index})
    list(APPEND SDKCONFIG_DEFAULTS "${_profile_root}/${_defaults}")
endforeach()
if(EXISTS "${_profile_root}/sdkconfig.private")
    list(APPEND SDKCONFIG_DEFAULTS "${_profile_root}/sdkconfig.private")
endif()
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${_profile_root}/profiles/profiles.json"
    "${_profile_root}/tools/build_firmware.py" ${SDKCONFIG_DEFAULTS})
message(STATUS "Agent Link firmware profile: ${AGENT_LINK_PROFILE} (${_profile_target})")

function(agent_link_validate_profile)
    idf_build_get_property(_python PYTHON)
    execute_process(
        COMMAND "${_python}" "${_profile_root}/tools/build_firmware.py"
                "${AGENT_LINK_PROFILE}" --validate "--sdkconfig=${SDKCONFIG}"
        RESULT_VARIABLE _result OUTPUT_VARIABLE _output ERROR_VARIABLE _error)
    if(NOT _result EQUAL 0)
        message(FATAL_ERROR "${_output}${_error}")
    endif()
    message(STATUS "${_output}")
endfunction()
