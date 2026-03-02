cmake_minimum_required(VERSION 3.20)
project(OpenALSpatialAudio VERSION 1.0.0 LANGUAGES CXX)

# ──────────────────────────────────────────────────────────────
# Compiler requirements
# ──────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if (NOT WIN32)
    message(FATAL_ERROR "This project targets Windows only (WASAPI/ISpatialAudioClient).")
endif()

# ──────────────────────────────────────────────────────────────
# Options
# ──────────────────────────────────────────────────────────────
option(OPENAL_SPATIAL_BUILD_EXAMPLES  "Build the example applications" ON)
option(OPENAL_SPATIAL_BUILD_REGISTER  "Build the provider registration tool" ON)
option(OPENAL_SPATIAL_BUILD_SHARED    "Build as a shared DLL (for COM server)" ON)

# ──────────────────────────────────────────────────────────────
# OpenAL Soft location
# Accepts OPENAL_INCLUDE_DIR and OPENAL_LIBRARY from CMake
# command line, or falls back to vcpkg/environment defaults.
# ──────────────────────────────────────────────────────────────
find_package(OpenAL CONFIG QUIET)
if (NOT OpenAL_FOUND)
    # Manual fallback – set OPENAL_ROOT on the command line
    if (NOT DEFINED OPENAL_ROOT)
        set(OPENAL_ROOT "$ENV{OPENAL_ROOT}" CACHE PATH "OpenAL Soft root directory")
    endif()
    find_path(OPENAL_INCLUDE_DIR
        NAMES al.h alext.h efx.h efx-presets.h
        PATHS "${OPENAL_ROOT}/include/AL"
              "${OPENAL_ROOT}/include"
              "C:/Program Files/OpenAL/include/AL"
              "C:/OpenAL/include/AL"
    )
    find_library(OPENAL_LIBRARY
        NAMES OpenAL32 openal soft_oal
        PATHS "${OPENAL_ROOT}/lib"
              "${OPENAL_ROOT}/lib64"
              "C:/Program Files/OpenAL/lib"
              "C:/OpenAL/lib"
    )
    if (NOT OPENAL_INCLUDE_DIR OR NOT OPENAL_LIBRARY)
        message(FATAL_ERROR
            "OpenAL Soft not found. Install it from https://github.com/kcat/openal-soft/releases\n"
            "or set OPENAL_ROOT to your installation prefix.")
    endif()
    add_library(OpenAL::OpenAL UNKNOWN IMPORTED)
    set_target_properties(OpenAL::OpenAL PROPERTIES
        IMPORTED_LOCATION "${OPENAL_LIBRARY}"
        INTERFACE_INCLUDE_DIRECTORIES "${OPENAL_INCLUDE_DIR}")
endif()

# ──────────────────────────────────────────────────────────────
# Windows SDK / WASAPI
# ──────────────────────────────────────────────────────────────
set(WINDOWS_SDK_AUDIO_LIBS
    ole32
    avrt
    uuid
    ksuser
    mmdevapi
)

# ──────────────────────────────────────────────────────────────
# Core library sources
# ──────────────────────────────────────────────────────────────
set(OPENAL_SPATIAL_SOURCES
    src/OpenALSpatialAudioClient.cpp
    src/SpatialAudioObject.cpp
    src/SpatialAudioStream.cpp
)

set(OPENAL_SPATIAL_HEADERS
    include/OpenALSpatialAudioClient.h
    include/SpatialAudioObject.h
    include/SpatialAudioStream.h
)

# ──────────────────────────────────────────────────────────────
# Main library target
# ──────────────────────────────────────────────────────────────
if (OPENAL_SPATIAL_BUILD_SHARED)
    add_library(openal_spatial SHARED
        ${OPENAL_SPATIAL_SOURCES}
        ${OPENAL_SPATIAL_HEADERS}
    )
    target_compile_definitions(openal_spatial PRIVATE
        OPENAL_SPATIAL_EXPORTS
        _USRDLL
    )
    set_target_properties(openal_spatial PROPERTIES
        OUTPUT_NAME "openal_spatial"
        WINDOWS_EXPORT_ALL_SYMBOLS ON
    )
else()
    add_library(openal_spatial STATIC
        ${OPENAL_SPATIAL_SOURCES}
        ${OPENAL_SPATIAL_HEADERS}
    )
endif()

target_include_directories(openal_spatial
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(openal_spatial
    PUBLIC
        OpenAL::OpenAL
    PRIVATE
        ${WINDOWS_SDK_AUDIO_LIBS}
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

# ──────────────────────────────────────────────────────────────
# Registration tool
# ──────────────────────────────────────────────────────────────
if (OPENAL_SPATIAL_BUILD_REGISTER)
    add_executable(RegisterProvider
        register/RegisterProvider.cpp
    )
    target_compile_definitions(RegisterProvider PRIVATE
        WIN32_LEAN_AND_MEAN NOMINMAX _WIN32_WINNT=0x0A00
    )
    target_link_libraries(RegisterProvider PRIVATE
        advapi32 shlwapi
    )
    set_target_properties(RegisterProvider PROPERTIES
        WIN32_EXECUTABLE FALSE   # Console app (not GUI)
    )
endif()

# ──────────────────────────────────────────────────────────────
# Examples
# ──────────────────────────────────────────────────────────────
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

# ──────────────────────────────────────────────────────────────
# Install rules
# ──────────────────────────────────────────────────────────────
install(TARGETS openal_spatial
    RUNTIME DESTINATION bin
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)
install(FILES ${OPENAL_SPATIAL_HEADERS}
    DESTINATION include/openal-spatial
)
if (OPENAL_SPATIAL_BUILD_REGISTER)
    install(TARGETS RegisterProvider RUNTIME DESTINATION bin)
endif()

# ──────────────────────────────────────────────────────────────
# Summary
# ──────────────────────────────────────────────────────────────
message(STATUS "────────────────────────────────────────")
message(STATUS "  OpenAL Spatial Audio Renderer")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "  Shared DLL:   ${OPENAL_SPATIAL_BUILD_SHARED}")
message(STATUS "  Examples:     ${OPENAL_SPATIAL_BUILD_EXAMPLES}")
message(STATUS "  Register:     ${OPENAL_SPATIAL_BUILD_REGISTER}")
message(STATUS "────────────────────────────────────────")
