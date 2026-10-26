cmake_minimum_required(VERSION 3.24)

project(WineMetalGL VERSION 3.7.0 LANGUAGES C CXX OBJC OBJCXX)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT APPLE)
    message(FATAL_ERROR "WineMetalGL builds only on macOS.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_OSX_DEPLOYMENT_TARGET "15.0" CACHE STRING "Minimum macOS deployment target" FORCE)
# The default build is x86_64, the architecture of the Wine host processes
# that load the core. WMGL_HOST_ONLY builds the core and its tests for the
# build machine's architecture (arm64 on Apple silicon) so they run without
# Rosetta.
option(WMGL_HOST_ONLY "Build the core and tests for the build machine only" OFF)
if(NOT WMGL_HOST_ONLY)
    set(CMAKE_OSX_ARCHITECTURES "x86_64" CACHE STRING "Host architecture" FORCE)
    if(NOT CMAKE_OSX_ARCHITECTURES STREQUAL "x86_64")
        message(FATAL_ERROR "WineMetalGL only supports x86_64 Mach-O output; universal and ARM targets are not supported.")
    endif()
endif()

set(CMAKE_POLICY_VERSION_MINIMUM 3.5)
set(SPIRV_CROSS_CLI OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_GLSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_MSL ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_CPP OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_REFLECT OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_C_API ON CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_UTIL OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_ENABLE_TESTS OFF CACHE BOOL "" FORCE)
set(SPIRV_CROSS_SKIP_INSTALL ON CACHE BOOL "" FORCE)

set(ENABLE_GLSLANG_BINARIES OFF CACHE BOOL "" FORCE)
set(ENABLE_SPVREMAPPER OFF CACHE BOOL "" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "" FORCE)
set(ENABLE_OPT OFF CACHE BOOL "" FORCE)
set(ENABLE_HLSL OFF CACHE BOOL "" FORCE)
set(GLSLANG_TESTS OFF CACHE BOOL "" FORCE)
set(GLSLANG_ENABLE_INSTALL OFF CACHE BOOL "" FORCE)

add_subdirectory(vendor/SPIRV-Cross)
add_subdirectory(vendor/glslang)

find_library(FOUNDATION_FRAMEWORK Foundation REQUIRED)
find_library(METAL_FRAMEWORK Metal REQUIRED)
find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
find_library(METALFX_FRAMEWORK MetalFX REQUIRED)

# ---------------------------------------------------------------------------
# WineMetalGL core: the OpenGL implementation on Metal. Built as
# libwinemetalgl.dylib, which the Wine module's winemetalgl.so loads from its
# own directory (src/wine, scripts/build-wine-module.sh).

find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(WMGL_GEN_DIR "${CMAKE_CURRENT_BINARY_DIR}/generated")
set(WMGL_GEN_OUTPUTS
    "${WMGL_GEN_DIR}/gl_enums.gen.h"
    "${WMGL_GEN_DIR}/gl_api.gen.h"
    "${WMGL_GEN_DIR}/gl_entry.gen.cpp"
    "${WMGL_GEN_DIR}/gl_stubs.gen.cpp"
    "${WMGL_GEN_DIR}/gl_commands.gen.json"
    "${WMGL_GEN_DIR}/gl_pfn.gen.h"
    "${WMGL_GEN_DIR}/gl_list.gen.cpp"
    "${WMGL_GEN_DIR}/gl_list.gen.h")
add_custom_command(
    OUTPUT ${WMGL_GEN_OUTPUTS}
    COMMAND "${Python3_EXECUTABLE}" -I "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gl_gen.py"
            --registry "${CMAKE_CURRENT_SOURCE_DIR}/third_party/khronos/gl.xml"
            --extensions "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/extensions.txt"
            --out "${WMGL_GEN_DIR}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/gl_gen.py"
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/khronos/gl.xml"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen/extensions.txt"
    COMMENT "Generating OpenGL front end from the Khronos registry")
add_custom_target(wmgl_generated DEPENDS ${WMGL_GEN_OUTPUTS})

# Platform-independent pieces (host-testable): formats, pixel transfer, shader compiler.
add_library(wmgl_portable STATIC
    src/core/format/block_encode.cpp
    src/core/format/formats.cpp
    src/core/format/es_formats.cpp
    src/core/format/pixel_transfer.cpp
    src/core/format/vertex_decode.cpp
    src/core/shader/border.cpp
    src/core/shader/compiler.cpp
    src/core/shader/fp64.cpp
    src/core/shader/geometry.cpp
    src/core/shader/link_file_cache.cpp
    src/core/shader/transform_feedback.cpp
    src/core/shader/glsl_upgrade.cpp
    src/core/shader/tessellation.cpp
    src/core/tess/tessellator.cpp
    src/core/shader/spirv_input.cpp
    src/core/shader/spirv_passes.cpp
    src/core/shader/spirv_validate.cpp
    src/core/shader/subroutines.cpp
    src/core/compat/arb_program.cpp
    src/core/util/crash.cpp
    src/core/util/log.cpp
    src/core/util/metalfx_config.cpp
    src/core/util/settings.cpp
    src/core/util/thread_qos.cpp
    vendor/tessellator/tessellator.cpp
)
add_dependencies(wmgl_portable wmgl_generated)
target_include_directories(wmgl_portable PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/vendor/tessellator")
# The vendored reference tessellator (vendor/tessellator) is kept as it is.
set_source_files_properties(vendor/tessellator/tessellator.cpp PROPERTIES COMPILE_OPTIONS "-w")
target_include_directories(wmgl_portable PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
    "${CMAKE_CURRENT_SOURCE_DIR}/include"
    "${WMGL_GEN_DIR}")
target_link_libraries(wmgl_portable PUBLIC
    glslang glslang-default-resource-limits SPIRV spirv-cross-core spirv-cross-glsl spirv-cross-msl)
set_target_properties(wmgl_portable PROPERTIES POSITION_INDEPENDENT_CODE ON)

file(GLOB WMGL_CORE_SOURCES CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/*.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/api/*.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/compat/*.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/metal/*.mm")
add_library(winemetalgl_core SHARED
    ${WMGL_CORE_SOURCES}
    "${WMGL_GEN_DIR}/gl_entry.gen.cpp"
    "${WMGL_GEN_DIR}/gl_stubs.gen.cpp"
    "${WMGL_GEN_DIR}/gl_list.gen.cpp")
# The commit the core is built from, for the log's banner (core/util/log.h).
execute_process(COMMAND git -C "${CMAKE_CURRENT_SOURCE_DIR}" describe --always --dirty --abbrev=12
                OUTPUT_VARIABLE WMGL_COMMIT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
target_compile_definitions(winemetalgl_core PRIVATE WMGL_VERSION="${PROJECT_VERSION}" WMGL_COMMIT="${WMGL_COMMIT}")
add_dependencies(winemetalgl_core wmgl_generated)
# Generated and other .cpp sources include Metal-using headers: build them as Objective-C++.
set_source_files_properties(
    "${WMGL_GEN_DIR}/gl_entry.gen.cpp" "${WMGL_GEN_DIR}/gl_stubs.gen.cpp" "${WMGL_GEN_DIR}/gl_list.gen.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/src/core/proc_table.cpp"
    TARGET_DIRECTORY winemetalgl_core PROPERTIES LANGUAGE OBJCXX)
target_link_libraries(winemetalgl_core PRIVATE
    wmgl_portable
    "${FOUNDATION_FRAMEWORK}" "${METAL_FRAMEWORK}" "${QUARTZCORE_FRAMEWORK}" "${METALFX_FRAMEWORK}")
target_compile_options(winemetalgl_core PRIVATE
    $<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>
    -fvisibility=hidden -Wall -Wno-unused-parameter -Werror=missing-prototypes)
# ThinLTO in release builds: the core's many small helpers (enum mappers,
# Device::get, stats) inline across files, which matters more under Rosetta,
# where every call and indirect branch is translated (issue #12).
option(WMGL_LTO "Build the core with ThinLTO in release builds" ON)
if(WMGL_LTO)
    target_compile_options(winemetalgl_core PRIVATE $<$<CONFIG:Release>:-flto=thin>)
    target_compile_options(wmgl_portable PRIVATE $<$<CONFIG:Release>:-flto=thin>)
    target_link_options(winemetalgl_core PRIVATE $<$<CONFIG:Release>:-flto=thin>)
endif()
set_target_properties(winemetalgl_core PROPERTIES
    OUTPUT_NAME "winemetalgl"
    PREFIX "lib"
    INSTALL_NAME_DIR "@rpath")
if(NOT WMGL_HOST_ONLY)
    set_target_properties(winemetalgl_core PROPERTIES OSX_ARCHITECTURES "x86_64")
endif()
# Only the driver ABI is exported; GL entry points are reached via wmgl_get_proc_address.
target_link_options(winemetalgl_core PRIVATE "LINKER:-exported_symbol,_wmgl_*")
install(TARGETS winemetalgl_core LIBRARY DESTINATION lib)

# Host tests (no GPU, no Wine).
add_executable(wmgl_host_tests tests/host/main.cpp tests/host/shader_tests.cpp tests/host/format_tests.cpp
    tests/host/fp64_tests.cpp tests/host/metalfx_tests.cpp tests/host/arb_program_tests.cpp tests/host/log_tests.cpp tests/host/settings_tests.cpp tests/host/version_tests.cpp)
target_link_libraries(wmgl_host_tests PRIVATE wmgl_portable)

# GPU tests: drive the core through the driver ABI on pbuffers (Metal, no Wine).
file(GLOB WMGL_GPU_TEST_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tests/gpu/*.cpp")
add_executable(wmgl_gpu_tests ${WMGL_GPU_TEST_SOURCES})
add_dependencies(wmgl_gpu_tests wmgl_generated)
target_include_directories(wmgl_gpu_tests PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/src" "${CMAKE_CURRENT_SOURCE_DIR}/include" "${WMGL_GEN_DIR}")
target_link_libraries(wmgl_gpu_tests PRIVATE winemetalgl_core)
