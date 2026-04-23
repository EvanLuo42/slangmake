include_guard(GLOBAL)

include(FetchPackage)

function(configure_doctest target)
    cmake_parse_arguments(FD "" "VERSION" "" ${ARGN})
    if(NOT FD_VERSION)
        message(FATAL_ERROR "configure_doctest: VERSION is required")
    endif()

    set(_doctest_url "https://github.com/doctest/doctest/archive/refs/tags/v${FD_VERSION}.tar.gz")

    message(STATUS "Fetching doctest ${FD_VERSION} ...")
    FetchPackage(doctest URL ${_doctest_url})
    # Point at the source root so `#include <doctest/doctest.h>` resolves.
    set(_doctest_inc ${doctest_SOURCE_DIR})

    if(NOT TARGET doctest::doctest)
        add_library(doctest::doctest INTERFACE IMPORTED GLOBAL)
        set_target_properties(doctest::doctest PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES ${_doctest_inc}
        )
    endif()

    target_link_libraries(${target} PUBLIC doctest::doctest)
    target_include_directories(${target} PUBLIC ${_doctest_inc})
endfunction()
