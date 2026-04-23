include_guard(GLOBAL)

include(FetchPackage)

function(configure_cli11 target)
    cmake_parse_arguments(FC "" "VERSION" "" ${ARGN})
    if(NOT FC_VERSION)
        message(FATAL_ERROR "configure_cli11: VERSION is required")
    endif()

    set(_cli11_url "https://github.com/CLIUtils/CLI11/archive/refs/tags/v${FC_VERSION}.tar.gz")

    message(STATUS "Fetching CLI11 ${FC_VERSION} ...")
    FetchPackage(cli11 URL ${_cli11_url})
    set(_cli11_inc ${cli11_SOURCE_DIR}/include)

    if(NOT TARGET CLI11::CLI11)
        add_library(CLI11::CLI11 INTERFACE IMPORTED GLOBAL)
        set_target_properties(CLI11::CLI11 PROPERTIES
            INTERFACE_INCLUDE_DIRECTORIES ${_cli11_inc}
        )
    endif()

    target_link_libraries(${target} PUBLIC CLI11::CLI11)
    target_include_directories(${target} PUBLIC ${_cli11_inc})
endfunction()