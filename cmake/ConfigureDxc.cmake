include_guard(GLOBAL)

include(FetchPackage)
include(DetermineTargetArchitecture)

# DirectX Shader Compiler binary releases on GitHub use a YYYY_MM_DD date
# stamp in the asset filename that doesn't follow from the version tag, so
# we take both as inputs.
function(configure_dxc target)
    cmake_parse_arguments(FD "" "VERSION;DATE" "" ${ARGN})
    if(NOT FD_VERSION)
        message(FATAL_ERROR "configure_dxc: VERSION is required (e.g. 1.9.2602)")
    endif()
    if(NOT FD_DATE)
        message(FATAL_ERROR "configure_dxc: DATE is required (e.g. 2026_02_20)")
    endif()

    determine_target_architecture(_dxc_arch)

    set(_dxc_url "https://github.com/microsoft/DirectXShaderCompiler/releases/download/v${FD_VERSION}")
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(_dxc_url "${_dxc_url}/dxc_${FD_DATE}.zip")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        if(NOT _dxc_arch MATCHES "x86_64")
            message(WARNING "configure_dxc: only linux-x86_64 is published upstream; skipping DXC fetch")
            return()
        endif()
        set(_dxc_url "${_dxc_url}/linux_dxc_${FD_DATE}.x86_64.tar.gz")
    else()
        # macOS / other: DXC is a Windows/Linux story; Slang doesn't link DXIL
        # there anyway. Skip silently.
        return()
    endif()

    message(STATUS "Fetching DXC ${FD_VERSION} ...")
    FetchPackage(dxc URL ${_dxc_url})

    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        if(_dxc_arch MATCHES "aarch64|arm64")
            set(_dxc_bin ${dxc_SOURCE_DIR}/bin/arm64)
        else()
            set(_dxc_bin ${dxc_SOURCE_DIR}/bin/x64)
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_dxc_bin ${dxc_SOURCE_DIR}/lib)
    endif()

    set_property(GLOBAL PROPERTY _DXC_BINARY_DIR ${_dxc_bin})
endfunction()

# Copy DXC's runtime libraries next to `target` so Slang can LoadLibrary them
# at compile time when emitting DXIL.
function(dxc_copy_runtime_dlls target)
    get_property(_dxc_bin GLOBAL PROPERTY _DXC_BINARY_DIR)
    if(NOT _dxc_bin)
        return()
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(_dxc_libs dxcompiler.dll dxil.dll)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(_dxc_libs libdxcompiler.so libdxil.so)
    else()
        return()
    endif()

    foreach(_lib ${_dxc_libs})
        if(EXISTS ${_dxc_bin}/${_lib})
            add_custom_command(TARGET ${target} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                    ${_dxc_bin}/${_lib}
                    $<TARGET_FILE_DIR:${target}>)
        endif()
    endforeach()
endfunction()

# install()s every DXC runtime library file into `destination`.
function(dxc_install_runtime_libs destination)
    get_property(_dxc_bin GLOBAL PROPERTY _DXC_BINARY_DIR)
    if(NOT _dxc_bin)
        return()
    endif()

    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        file(GLOB _dxc_libs ${_dxc_bin}/dxcompiler.dll ${_dxc_bin}/dxil.dll)
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        file(GLOB _dxc_libs ${_dxc_bin}/libdxcompiler.so* ${_dxc_bin}/libdxil.so*)
    else()
        return()
    endif()

    foreach(_lib ${_dxc_libs})
        install(FILES ${_lib} DESTINATION ${destination})
    endforeach()
endfunction()