cmake_minimum_required(VERSION 3.25)
project(slangmake VERSION 0.2.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

option(SLANG_MAKE_BUILD_SHARED "Build slangmake as a shared library" ON)
option(SLANG_MAKE_BUILD_TESTS "Build slangmake unit tests" ON)
set(SLANG_MAKE_FETCH_SLANG_VERSION "2026.7" CACHE STRING "Slang version to fetch")
set(SLANG_MAKE_FETCH_CLI11_VERSION "2.6.2" CACHE STRING "CLI11 version to fetch")
set(SLANG_MAKE_FETCH_DOCTEST_VERSION "2.4.11" CACHE STRING "doctest version to fetch")
set(SLANG_MAKE_FETCH_LZ4_VERSION "1.10.0" CACHE STRING "LZ4 version to fetch")
set(SLANG_MAKE_FETCH_ZSTD_VERSION "1.5.7" CACHE STRING "zstd version to fetch")

if (SLANG_MAKE_BUILD_SHARED)
    add_library(slangmake-lib SHARED)
    set_target_properties(slangmake-lib PROPERTIES
            WINDOWS_EXPORT_ALL_SYMBOLS ON)
else ()
    add_library(slangmake-lib STATIC)
endif ()
set_target_properties(slangmake-lib PROPERTIES OUTPUT_NAME slangmake)
target_sources(slangmake-lib PRIVATE
        src/slangmake_util.cpp
        src/slangmake_blob.cpp
        src/slangmake_reflection.cpp
        src/slangmake_compiler.cpp
        src/slangmake_batch.cpp
        src/slangmake_internal.h
        include/slangmake.h
)
target_include_directories(slangmake-lib PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/generated/include)

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/slangmake_version.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/generated/include/slangmake_version.h
        @ONLY)

add_executable(slangmake)
target_sources(slangmake PRIVATE
        src/main.cpp)
target_link_libraries(slangmake PRIVATE slangmake-lib)

include(ConfigureCLI11)
configure_cli11(slangmake VERSION ${SLANG_MAKE_FETCH_CLI11_VERSION})

include(ConfigureSlang)
configure_slang(slangmake-lib VERSION ${SLANG_MAKE_FETCH_SLANG_VERSION})
slang_copy_runtime_dlls(slangmake)

include(ConfigureLz4)
configure_lz4(slangmake-lib VERSION ${SLANG_MAKE_FETCH_LZ4_VERSION})

include(ConfigureZstd)
configure_zstd(slangmake-lib VERSION ${SLANG_MAKE_FETCH_ZSTD_VERSION})

if (SLANG_MAKE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

include(GNUInstallDirs)

install(TARGETS slangmake
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(TARGETS slangmake-lib
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES include/slangmake.h
        ${CMAKE_CURRENT_BINARY_DIR}/generated/include/slangmake_version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES LICENSE README.md
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/slangmake
        OPTIONAL)

slang_install_runtime_libs(${CMAKE_INSTALL_BINDIR})