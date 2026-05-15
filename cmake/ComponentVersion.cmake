# ComponentVersion.cmake
# Usage in a component's CMakeLists.txt:
#
#   include(${CMAKE_SOURCE_DIR}/cmake/ComponentVersion.cmake)
#   get_component_version("binance-md" COMPONENT_VERSION)
#   configure_file(${CMAKE_SOURCE_DIR}/cmake/Version.h.in
#                  ${CMAKE_CURRENT_BINARY_DIR}/Version.h @ONLY)
#
# Tags must follow the convention:  <tag-prefix>/vMAJOR.MINOR.PATCH
# e.g.  binance-md/v1.2.0
#
# If no matching tag exists the version resolves to "unversioned".
# If there are uncommitted changes it appends "-dirty".

find_package(Git QUIET)

function(get_component_version TAG_PREFIX OUT_VAR)
    set(_version "unversioned")
    if(GIT_FOUND)
        execute_process(
            COMMAND ${GIT_EXECUTABLE} describe --tags --match "${TAG_PREFIX}/*" --dirty --always
            WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
            OUTPUT_VARIABLE _raw
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET
            RESULT_VARIABLE _result
        )
        if(_result EQUAL 0 AND _raw)
            # Strip the "<prefix>/" namespace so the binary reports "v1.2.0"
            # rather than "binance-md/v1.2.0".
            string(REGEX REPLACE "^${TAG_PREFIX}/" "" _version "${_raw}")
        endif()
    endif()
    message(STATUS "[${TAG_PREFIX}] component version: ${_version}")
    set(${OUT_VAR} "${_version}" PARENT_SCOPE)
endfunction()
