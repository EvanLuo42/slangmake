include_guard(GLOBAL)

include(FetchPackage)
include(DetermineTargetArchitecture)

function(configure_slang target)
    cmake_parse_arguments(FS "" "VERSION" "" ${ARGN})
    if(NOT FS_VERSION)
        message(FATAL_ERROR "configure_slang: VERSION is required")
    endif()

    determine_target_architecture(_slang_arch)

    set(_slang_url "https://github.com/shader-slang/slang/releases/download/v${FS_VERSION}/slang-${FS_VERSION}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        if(_slang_arch MATCHES "x86_64")
            set(_slang_url "${_slang_url}-windows-x86_64.zip")
        elseif(_slang_arch MATCHES "aarch64|arm64")
            set(_slang_url "${_slang_url}-windows-aarch64.zip")
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(_slang_arch MATCHES "x86_64")
            set(_slang_url "${_slang_url}-linux-x86_64.tar.gz")
        elseif(_slang_arch MATCHES "aarch64|arm64")
            set(_slang_url "${_slang_url}-linux-aarch64.tar.gz")
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        if(CMAKE_APPLE_SILICON_PROCESSOR MATCHES "x86_64")
            set(_slang_url "${_slang_url}-macos-x86_64.zip")
        else()
            set(_slang_url "${_slang_url}-macos-aarch64.zip")
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
        set(_slang_url "${_slang_url}-wasm-libs.zip")
    endif()

    message(STATUS "Fetching Slang ${FS_VERSION} ...")
    FetchPackage(slang URL ${_slang_url})
    set(_slang_inc ${slang_SOURCE_DIR}/include)
    set(_slang_bin ${slang_SOURCE_DIR})

    if(NOT TARGET slang)
        if(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
            add_library(slang STATIC IMPORTED GLOBAL)
        else()
            add_library(slang SHARED IMPORTED GLOBAL)
        endif()

        # Slang renamed the compiler library from slang to slang-compiler in v2025.21
        if(CMAKE_SYSTEM_NAME STREQUAL "Windows" AND EXISTS ${_slang_bin}/bin/slang-compiler.dll)
            set(_slang_lib slang-compiler)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux" AND EXISTS ${_slang_bin}/lib/libslang-compiler.so)
            set(_slang_lib slang-compiler)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND EXISTS ${_slang_bin}/lib/libslang-compiler.dylib)
            set(_slang_lib slang-compiler)
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
            set(_slang_lib slang-compiler)
        else()
            set(_slang_lib slang)
        endif()

        if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
            set_target_properties(slang PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES ${_slang_inc}
                IMPORTED_IMPLIB ${_slang_bin}/lib/${_slang_lib}.lib
                IMPORTED_LOCATION ${_slang_bin}/bin/${_slang_lib}.dll
            )
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
            set_target_properties(slang PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES ${_slang_inc}
                IMPORTED_LOCATION ${_slang_bin}/lib/lib${_slang_lib}.so
            )
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
            set_target_properties(slang PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES ${_slang_inc}
                IMPORTED_LOCATION ${_slang_bin}/lib/lib${_slang_lib}.dylib
            )
        elseif(CMAKE_SYSTEM_NAME STREQUAL "Emscripten")
            set_target_properties(slang PROPERTIES
                INTERFACE_INCLUDE_DIRECTORIES ${_slang_inc}
                IMPORTED_LOCATION ${_slang_bin}/lib/lib${_slang_lib}.a
            )
            target_link_libraries(slang INTERFACE
                ${_slang_bin}/lib/libcompiler-core.a
                ${_slang_bin}/lib/libcore.a
                ${_slang_bin}/lib/liblz4.a
                ${_slang_bin}/lib/libminiz.a
            )
        endif()
    endif()

    target_link_libraries(${target} PUBLIC slang)
    target_include_directories(${target} PUBLIC ${_slang_inc})

    set_property(GLOBAL PROPERTY _SLANG_BINARY_DIR ${_slang_bin})
endfunction()

function(slang_copy_runtime_dlls target)
    if(NOT CMAKE_SYSTEM_NAME STREQUAL "Windows")
        return()
    endif()
    get_property(_slang_bin GLOBAL PROPERTY _SLANG_BINARY_DIR)
    if(NOT _slang_bin)
        message(FATAL_ERROR "slang_copy_runtime_dlls: call configure_slang() first")
    endif()
    foreach(_slang_dll slang.dll slang-compiler.dll slang-glslang.dll slang-llvm.dll slang-rt.dll)
        if(EXISTS ${_slang_bin}/bin/${_slang_dll})
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${_slang_bin}/bin/${_slang_dll}
                    $<TARGET_FILE_DIR:${target}>)
        endif()
    endforeach()
endfunction()

# install()s every Slang shared-library file into `destination`. Call after
# configure_slang() so the binary dir property is set.
function(slang_install_runtime_libs destination)
    get_property(_slang_bin GLOBAL PROPERTY _SLANG_BINARY_DIR)
    if(NOT _slang_bin)
        message(FATAL_ERROR "slang_install_runtime_libs: call configure_slang() first")
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        file(GLOB _slang_libs
            ${_slang_bin}/bin/slang*.dll
            ${_slang_bin}/bin/slang*.pdb)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        file(GLOB _slang_libs ${_slang_bin}/lib/libslang*.so*)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        file(GLOB _slang_libs ${_slang_bin}/lib/libslang*.dylib)
    else()
        return()
    endif()

    foreach(_lib ${_slang_libs})
        install(FILES ${_lib} DESTINATION ${destination})
    endforeach()
endfunction()