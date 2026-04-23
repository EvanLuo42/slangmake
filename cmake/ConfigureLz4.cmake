include_guard(GLOBAL)

include(FetchPackage)

function(configure_lz4 target)
    cmake_parse_arguments(FL "" "VERSION" "" ${ARGN})
    if(NOT FL_VERSION)
        message(FATAL_ERROR "configure_lz4: VERSION is required")
    endif()

    message(STATUS "Fetching LZ4 ${FL_VERSION} ...")
    FetchPackage(lz4 URL "https://github.com/lz4/lz4/archive/refs/tags/v${FL_VERSION}.tar.gz")

    if(NOT TARGET slangmake_lz4)
        add_library(slangmake_lz4 STATIC ${lz4_SOURCE_DIR}/lib/lz4.c)
        target_include_directories(slangmake_lz4 PUBLIC ${lz4_SOURCE_DIR}/lib)
        set_target_properties(slangmake_lz4 PROPERTIES POSITION_INDEPENDENT_CODE ON)
        if (MSVC)
            target_compile_options(slangmake_lz4 PRIVATE /wd4127 /wd4244 /wd4267)
        endif()
    endif()
    target_link_libraries(${target} PUBLIC slangmake_lz4)
endfunction()
