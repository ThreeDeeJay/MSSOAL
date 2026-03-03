cmake_minimum_required(VERSION 3.20)
project(OpenALSpatialAudio VERSION 1.0.0 LANGUAGES CXX)

# -
# Compiler requirements
# -
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT WIN32)
    message(FATAL_ERROR "This project targets Windows only (WASAPI/ISpatialAudioClient).")
endif()

# -
# Options
# -
option(OPENAL_SPATIAL_BUILD_EXAMPLES  "Build the example applications" ON)
option(OPENAL_SPATIAL_BUILD_REGISTER  "Build the provider registration tool" ON)
option(OPENAL_SPATIAL_BUILD_SHARED    "Build as a shared DLL (for COM server)" ON)

# -
# OpenAL Soft location
# Accepts OPENAL_INCLUDE_DIR and OPENAL_LIBRARY from CMake
# command line, or falls back to vcpkg/environment defaults.
# -
# - OpenAL Soft -
# Priority order:
#   1. OPENAL_ROOT cmake/env variable  (CI sets this to the pre-built SDK dir)
#   2. CONFIG-mode find_package        (vcpkg or system install)
#   3. Well-known default install paths
#
# openal-soft is intentionally absent from vcpkg.json so CI can use the
# official pre-built binary zip instead of compiling from source (~4-6 min).
# See .github/workflows/build.yml for how OPENAL_ROOT is set in CI.

# Honour an env-var so callers don't need to pass -DOPENAL_ROOT explicitly
if (NOT DEFINED OPENAL_ROOT AND DEFINED ENV{OPENAL_ROOT})
    set(OPENAL_ROOT "$ENV{OPENAL_ROOT}" CACHE PATH "OpenAL Soft root directory")
endif()

# If OPENAL_ROOT is set, search it first before anything else
if (DEFINED OPENAL_ROOT)
    find_path(OPENAL_INCLUDE_DIR
        NAMES al.h alext.h efx.h efx-presets.h
        PATHS "${OPENAL_ROOT}/include/AL"
              "${OPENAL_ROOT}/include"
        NO_DEFAULT_PATH)
    find_library(OPENAL_LIBRARY
        NAMES OpenAL32 openal soft_oal
        PATHS "${OPENAL_ROOT}/lib"
              "${OPENAL_ROOT}/libs/Win64"
              "${OPENAL_ROOT}/lib64"
        NO_DEFAULT_PATH)
    if (OPENAL_INCLUDE_DIR AND OPENAL_LIBRARY)
        add_library(OpenAL::OpenAL UNKNOWN IMPORTED)
        set_target_properties(OpenAL::OpenAL PROPERTIES
            IMPORTED_LOCATION             "${OPENAL_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${OPENAL_INCLUDE_DIR}")
        message(STATUS "OpenAL Soft (pre-built): ${OPENAL_LIBRARY}")
    endif()
endif()

# Fall back to CONFIG-mode (vcpkg, system) if OPENAL_ROOT didn't resolve
if (NOT TARGET OpenAL::OpenAL)
    find_package(OpenAL CONFIG QUIET)
    if (NOT OpenAL_FOUND)
        find_path(OPENAL_INCLUDE_DIR
            NAMES al.h alext.h efx.h efx-presets.h
            PATHS "C:/Program Files/OpenAL/include/AL"
                  "C:/OpenAL/include/AL")
        find_library(OPENAL_LIBRARY
            NAMES OpenAL32 openal soft_oal
            PATHS "C:/Program Files/OpenAL/lib"
                  "C:/OpenAL/lib")
        if (NOT OPENAL_INCLUDE_DIR OR NOT OPENAL_LIBRARY)
            message(FATAL_ERROR
                "OpenAL Soft not found.\n"
                "Download a pre-built SDK from:\n"
                "  https://github.com/kcat/openal-soft/releases\n"
                "then pass -DOPENAL_ROOT=<path> to CMake, or set the\n"
                "OPENAL_ROOT environment variable.")
        endif()
        add_library(OpenAL::OpenAL UNKNOWN IMPORTED)
        set_target_properties(OpenAL::OpenAL PROPERTIES
            IMPORTED_LOCATION             "${OPENAL_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${OPENAL_INCLUDE_DIR}")
        message(STATUS "OpenAL Soft (fallback paths): ${OPENAL_LIBRARY}")
    else()
        message(STATUS "OpenAL Soft (config): ${OpenAL_DIR}")
    endif()
endif()

# -
# Windows SDK / WASAPI
# -
set(WINDOWS_SDK_AUDIO_LIBS
    ole32
    avrt
    uuid
    ksuser
    mmdevapi
)

# -
# Core library sources
# -
set(OPENAL_SPATIAL_SOURCES
    src/OpenALSpatialAudioClient.cpp
    src/SpatialAudioObject.cpp
    src/SpatialAudioStream.cpp
)

# Public API header (installed alongside the library)
set(OPENAL_SPATIAL_PUBLIC_HEADERS
    include/OpenALSpatialAudioClient.h
)

# Internal implementation headers (live beside their .cpp files in src/)
set(OPENAL_SPATIAL_PRIVATE_HEADERS
    src/SpatialAudioObject.h
    src/SpatialAudioStream.h
)

# -
# Main library target
# -
if (OPENAL_SPATIAL_BUILD_SHARED)
    add_library(openal_spatial SHARED
        ${OPENAL_SPATIAL_SOURCES}
        ${OPENAL_SPATIAL_PUBLIC_HEADERS}
        ${OPENAL_SPATIAL_PRIVATE_HEADERS}
        # .def file guarantees DllGetClassObject etc. are exported by their
        # exact undecorated names -- required for COM in-process servers.
        src/openal_spatial.def
    )
    target_compile_definitions(openal_spatial PRIVATE
        OPENAL_SPATIAL_EXPORTS
        _USRDLL
    )
    set_target_properties(openal_spatial PROPERTIES
        OUTPUT_NAME "openal_spatial"
        # Do NOT use WINDOWS_EXPORT_ALL_SYMBOLS together with a .def file --
        # the .def file is the authoritative export list.
        WINDOWS_EXPORT_ALL_SYMBOLS OFF
    )
else()
    add_library(openal_spatial STATIC
        ${OPENAL_SPATIAL_SOURCES}
        ${OPENAL_SPATIAL_PUBLIC_HEADERS}
        ${OPENAL_SPATIAL_PRIVATE_HEADERS}
    )
endif()

target_include_directories(openal_spatial
    PUBLIC
        # Consumers only see the public include/ directory
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
    PRIVATE
        # Internal headers are resolved from src/ at build time only
        ${CMAKE_CURRENT_SOURCE_DIR}/src
)

target_link_libraries(openal_spatial
    PUBLIC
        OpenAL::OpenAL
    PRIVATE
        ${WINDOWS_SDK_AUDIO_LIBS}
        advapi32   # RegCreateKeyExW etc. in DllRegisterServer
        shlwapi    # PathFileExistsW
)

target_compile_options(openal_spatial PRIVATE
    /W4
    /WX-         # Warnings-as-errors off during development
    /Zi          # Debug info
    /O2          # Optimizations on
    /MP          # Multi-processor compilation
)

target_compile_definitions(openal_spatial PRIVATE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
    _WIN32_WINNT=0x0A00  # Windows 10
)

# -
# Registration tool
# -
if (OPENAL_SPATIAL_BUILD_REGISTER)
    add_executable(RegisterProvider
        tools/RegisterProvider.cpp
    )
    target_compile_definitions(RegisterProvider PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
    )
    target_link_libraries(RegisterProvider PRIVATE
        advapi32 shlwapi
    )
    set_target_properties(RegisterProvider PROPERTIES
        WIN32_EXECUTABLE FALSE
        # Embed the manifest file directly. This is the reliable approach --
        # the /MANIFESTUAC linker flag has unfixable quote-escaping issues
        # when passed through CMake on all generator types.
        VS_MANIFEST_FILE "${CMAKE_CURRENT_SOURCE_DIR}/tools/RegisterProvider.manifest"
    )
endif()

# -
# Examples
# -
if (OPENAL_SPATIAL_BUILD_EXAMPLES)
    add_executable(BasicExample
        examples/BasicExample.cpp
    )
    target_link_libraries(BasicExample PRIVATE
        openal_spatial
        OpenAL::OpenAL
        ole32
    )
    target_compile_definitions(BasicExample PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
    )
    # Copy openal32.dll / soft_oal.dll next to the example
    add_custom_command(TARGET BasicExample POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            $<TARGET_FILE:OpenAL::OpenAL>
            $<TARGET_FILE_DIR:BasicExample>
        COMMENT "Copying OpenAL Soft DLL"
    )
endif()

# -
# Install rules
# -
install(TARGETS openal_spatial
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(FILES ${OPENAL_SPATIAL_PUBLIC_HEADERS}
    DESTINATION include/openal-spatial
)
if (OPENAL_SPATIAL_BUILD_REGISTER)
    install(TARGETS RegisterProvider RUNTIME DESTINATION bin)
endif()

# -
# Summary
# -
message(STATUS "-")
message(STATUS "  OpenAL Spatial Audio Renderer")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "  Shared DLL:   ${OPENAL_SPATIAL_BUILD_SHARED}")
message(STATUS "  Examples:     ${OPENAL_SPATIAL_BUILD_EXAMPLES}")
message(STATUS "  Register:     ${OPENAL_SPATIAL_BUILD_REGISTER}")
message(STATUS "-")
