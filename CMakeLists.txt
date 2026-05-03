cmake_minimum_required(VERSION 3.25)
project(slangmake VERSION 0.4.0 LANGUAGES C CXX)

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
set(SLANG_MAKE_FETCH_DXC_VERSION "1.9.2602" CACHE STRING "DirectX Shader Compiler version to fetch")
set(SLANG_MAKE_FETCH_DXC_DATE "2026_02_20" CACHE STRING "DirectX Shader Compiler release date stamp (used in asset filename)")

configure_file(
        ${CMAKE_CURRENT_SOURCE_DIR}/cmake/slangmake_version.h.in
        ${CMAKE_CURRENT_BINARY_DIR}/generated/include/slangmake_version.h
        @ONLY)

# ----- Runtime-only library (slangmake-rt) ---------------------------------
# The read side: BlobReader / BlobWriter / ReflectionView / parsers / hashing /
# compression. Does NOT link slang or DXC, so consumers shipping only the read
# side don't have to redistribute slang.dll / dxcompiler.dll.

set(SLANG_MAKE_RT_SOURCES
        src/slangmake_util.cpp
        src/slangmake_blob.cpp
        src/slangmake_reflection.cpp
        src/slangmake_c.cpp
        src/slangmake_internal_rt.h
        src/slangmake_c_internal.h
        include/slangmake.h
        include/slangmake.hpp)

if (SLANG_MAKE_BUILD_SHARED)
    add_library(slangmake-rt-lib SHARED)
    set_target_properties(slangmake-rt-lib PROPERTIES
            WINDOWS_EXPORT_ALL_SYMBOLS ON)
else ()
    add_library(slangmake-rt-lib STATIC)
endif ()
set_target_properties(slangmake-rt-lib PROPERTIES OUTPUT_NAME slangmake-rt)
target_sources(slangmake-rt-lib PRIVATE ${SLANG_MAKE_RT_SOURCES})
target_compile_definitions(slangmake-rt-lib PRIVATE SLANGMAKE_C_BUILDING)
if (NOT SLANG_MAKE_BUILD_SHARED)
    target_compile_definitions(slangmake-rt-lib PUBLIC SLANGMAKE_C_STATIC)
endif ()
target_include_directories(slangmake-rt-lib PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/generated/include)

include(ConfigureLz4)
configure_lz4(slangmake-rt-lib VERSION ${SLANG_MAKE_FETCH_LZ4_VERSION})

include(ConfigureZstd)
configure_zstd(slangmake-rt-lib VERSION ${SLANG_MAKE_FETCH_ZSTD_VERSION})

# ----- Full library (slangmake-lib) ----------------------------------------
# Compiles the runtime sources + Slang/DXC-using sources into one DLL. The
# overlap with slangmake-rt is intentional: the two are alternative consumer
# choices, not chained DLL dependencies.

if (SLANG_MAKE_BUILD_SHARED)
    add_library(slangmake-lib SHARED)
    set_target_properties(slangmake-lib PROPERTIES
            WINDOWS_EXPORT_ALL_SYMBOLS ON)
else ()
    add_library(slangmake-lib STATIC)
endif ()
set_target_properties(slangmake-lib PROPERTIES OUTPUT_NAME slangmake)
target_sources(slangmake-lib PRIVATE
        ${SLANG_MAKE_RT_SOURCES}
        src/slangmake_util_compiler.cpp
        src/slangmake_reflection_serialize.cpp
        src/slangmake_compiler.cpp
        src/slangmake_batch.cpp
        src/slangmake_c_compiler.cpp
        src/slangmake_internal.h
)
target_compile_definitions(slangmake-lib PRIVATE SLANGMAKE_C_BUILDING)
if (NOT SLANG_MAKE_BUILD_SHARED)
    target_compile_definitions(slangmake-lib PUBLIC SLANGMAKE_C_STATIC)
endif ()
target_include_directories(slangmake-lib PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
        ${CMAKE_CURRENT_BINARY_DIR}/generated/include)

add_executable(slangmake)
target_sources(slangmake PRIVATE
        src/main.cpp)
target_link_libraries(slangmake PRIVATE slangmake-lib)

include(ConfigureCLI11)
configure_cli11(slangmake VERSION ${SLANG_MAKE_FETCH_CLI11_VERSION})

include(ConfigureSlang)
configure_slang(slangmake-lib VERSION ${SLANG_MAKE_FETCH_SLANG_VERSION})
slang_copy_runtime_dlls(slangmake)

include(ConfigureDxc)
configure_dxc(slangmake-lib
        VERSION ${SLANG_MAKE_FETCH_DXC_VERSION}
        DATE ${SLANG_MAKE_FETCH_DXC_DATE})
dxc_copy_runtime_dlls(slangmake)

configure_lz4(slangmake-lib VERSION ${SLANG_MAKE_FETCH_LZ4_VERSION})
configure_zstd(slangmake-lib VERSION ${SLANG_MAKE_FETCH_ZSTD_VERSION})

if (SLANG_MAKE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif ()

include(GNUInstallDirs)

install(TARGETS slangmake
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(TARGETS slangmake-lib slangmake-rt-lib
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

install(FILES include/slangmake.h
        include/slangmake.hpp
        ${CMAKE_CURRENT_BINARY_DIR}/generated/include/slangmake_version.h
        DESTINATION ${CMAKE_INSTALL_INCLUDEDIR})

install(FILES LICENSE README.md
        DESTINATION ${CMAKE_INSTALL_DATAROOTDIR}/slangmake
        OPTIONAL)

slang_install_runtime_libs(${CMAKE_INSTALL_BINDIR})
dxc_install_runtime_libs(${CMAKE_INSTALL_BINDIR})