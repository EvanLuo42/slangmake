include_guard(GLOBAL)

include(FetchPackage)

function(configure_zstd target)
    cmake_parse_arguments(FZ "" "VERSION" "" ${ARGN})
    if(NOT FZ_VERSION)
        message(FATAL_ERROR "configure_zstd: VERSION is required")
    endif()

    message(STATUS "Fetching zstd ${FZ_VERSION} ...")
    FetchPackage(zstd URL "https://github.com/facebook/zstd/archive/refs/tags/v${FZ_VERSION}.tar.gz")

    if(NOT TARGET slangmake_zstd)
        file(GLOB _zstd_common     CONFIGURE_DEPENDS ${zstd_SOURCE_DIR}/lib/common/*.c)
        file(GLOB _zstd_compress   CONFIGURE_DEPENDS ${zstd_SOURCE_DIR}/lib/compress/*.c)
        file(GLOB _zstd_decompress CONFIGURE_DEPENDS ${zstd_SOURCE_DIR}/lib/decompress/*.c)

        add_library(slangmake_zstd STATIC
                ${_zstd_common} ${_zstd_compress} ${_zstd_decompress})
        target_include_directories(slangmake_zstd PUBLIC  ${zstd_SOURCE_DIR}/lib)
        target_include_directories(slangmake_zstd PRIVATE ${zstd_SOURCE_DIR}/lib/common)
        target_compile_definitions(slangmake_zstd PRIVATE
                ZSTD_DISABLE_ASM=1
                ZSTD_MULTITHREAD=0)
        set_target_properties(slangmake_zstd PROPERTIES POSITION_INDEPENDENT_CODE ON)
        if (MSVC)
            target_compile_options(slangmake_zstd PRIVATE
                    /wd4127 /wd4244 /wd4245 /wd4267 /wd4334 /wd4701 /wd4703)
        endif()
    endif()
    target_link_libraries(${target} PUBLIC slangmake_zstd)
endfunction()
