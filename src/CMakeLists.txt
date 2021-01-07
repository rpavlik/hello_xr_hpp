# Copyright (c) 2017 The Khronos Group Inc.
#
# SPDX-License-Identifier: Apache-2.0

if(POLICY CMP0075)
    cmake_policy(SET CMP0075 NEW)
endif()

# Entire project uses C++14
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")

include(GNUInstallDirs)
include(StdFilesystemFlags)

string(TOUPPER "${CMAKE_GENERATOR_PLATFORM}" CMAKE_GENERATOR_PLATFORM_UPPER)

### Dependencies

set(OPENGLES_INCOMPATIBLE TRUE)
set(OPENGL_INCOMPATIBLE FALSE)
set(VULKAN_INCOMPATIBLE FALSE)

# CMake will detect OpenGL/Vulkan which are not compatible with UWP and ARM/ARM64 on Windows so skip it in these cases.
if(CMAKE_SYSTEM_NAME STREQUAL "WindowsStore" OR (WIN32 AND CMAKE_GENERATOR_PLATFORM_UPPER MATCHES "ARM.*"))
    set(OPENGL_INCOMPATIBLE TRUE)
    set(VULKAN_INCOMPATIBLE TRUE)
    message(STATUS "OpenGL/Vulkan disabled due to incompatibility")
elseif(ANDROID)
    set(OPENGL_INCOMPATIBLE TRUE)
    find_path(ANDROID_NATIVE_APP_GLUE android_native_app_glue.h PATHS ${ANDROID_NDK}/sources/android/native_app_glue)
    if(ANDROID_NATIVE_APP_GLUE)
        # Needed by gfxwrapper
        set(OPENGLES_INCOMPATIBLE FALSE)
    endif()
    if(ANDROID_PLATFORM_LEVEL LESS 24)
        set(VULKAN_INCOMPATIBLE TRUE)
        message(STATUS "Vulkan disabled due to incompatibility: need to target at least API 24")
    endif()
endif()

if(NOT OPENGL_INCOMPATIBLE)
    set(OpenGL_GL_PREFERENCE GLVND)
    find_package(OpenGL)

    if(OPENGL_FOUND)
        add_definitions(-DXR_USE_GRAPHICS_API_OPENGL)
        message(STATUS "Enabling OpenGL support")
    elseif(BUILD_ALL_EXTENSIONS)
        message(FATAL_ERROR "OpenGL not found")
    endif()
endif()

if(NOT OPENGLES_INCOMPATIBLE)
    find_package(OpenGLES COMPONENTS V3 V2)
    find_package(EGL)
    if(OPENGLES_FOUND AND EGL_FOUND)
        add_definitions(-DXR_USE_GRAPHICS_API_OPENGL_ES)
        message(STATUS "Enabling OpenGL|ES support")
    elseif(BUILD_ALL_EXTENSIONS)
        message(FATAL_ERROR "OpenGL|ES not found")
    endif()
endif()

if(NOT VULKAN_INCOMPATIBLE)
    # Find the Vulkan headers
    find_package(Vulkan)
    if(Vulkan_FOUND)
        add_definitions(-DXR_USE_GRAPHICS_API_VULKAN)
        message(STATUS "Enabling Vulkan support")
    elseif(BUILD_ALL_EXTENSIONS)
        message(FATAL_ERROR "Vulkan headers not found")
    endif()
endif()

if(MSVC)
    # Threads get confused on MSVC
    if(NOT TARGET Threads::Threads)
        add_library(Threads::Threads IMPORTED INTERFACE)
    endif()
else()
    find_package(Threads REQUIRED)
endif()

find_package(OpenXR REQUIRED CONFIG)

### All options defined here
option(BUILD_ALL_EXTENSIONS "Build loader and layers with all extensions" OFF)

include(CMakeDependentOption)

cmake_dependent_option(
    BUILD_WITH_STD_FILESYSTEM "Use std::[experimental::]filesystem." ON
    "HAVE_FILESYSTEM_WITHOUT_LIB OR HAVE_FILESYSTEM_NEEDING_LIBSTDCXXFS OR HAVE_FILESYSTEM_NEEDING_LIBCXXFS"
    OFF
)

# Several files use these compile-time OS switches
if(WIN32)
    add_definitions(-DXR_OS_WINDOWS)
    add_definitions(-DNOMINMAX)
elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    add_definitions(-DXR_OS_LINUX)
elseif(ANDROID)
    add_definitions(-DXR_OS_ANDROID)
endif()

# /EHsc (support for C++ exceptions) is default in most configurations but seems missing when building arm/arm64.
if(MSVC)
    set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} /EHsc")
endif()

# This is a little helper library for setting up OpenGL
if((OPENGL_FOUND OR OpenGLES_FOUND) AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/common/gfxwrapper_opengl.c")
    add_library(openxr-gfxwrapper STATIC common/gfxwrapper_opengl.c common/gfxwrapper_opengl.h)
    target_include_directories(openxr-gfxwrapper PUBLIC ${PROJECT_SOURCE_DIR}/external/include)
    if(OPENGL_FOUND)
        if(TARGET OpenGL::OpenGL)
            target_link_libraries(openxr-gfxwrapper PUBLIC OpenGL::OpenGL)
        elseif(TARGET OpenGL::GL)
            target_link_libraries(openxr-gfxwrapper PUBLIC OpenGL::GL)
        else()
            target_link_libraries(openxr-gfxwrapper PUBLIC ${OPENGL_LIBRARIES})
        endif()
    endif()
    if(OpenGLES_FOUND)
        if(TARGET OpenGLES::OpenGLESv3)
            target_link_libraries(openxr-gfxwrapper PUBLIC OpenGLES::OpenGLESv3)
        elseif(TARGET OpenGLES::OpenGLESv2)
            target_link_libraries(openxr-gfxwrapper PUBLIC OpenGLES::OpenGLESv2)
        else()
            message(FATAL_ERROR "Should not get here!")
        endif()
        target_link_libraries(openxr-gfxwrapper PUBLIC EGL::EGL)
    endif()
    if(ANDROID)
        target_include_directories(openxr-gfxwrapper PUBLIC ${ANDROID_NATIVE_APP_GLUE})

        # Note: For some reason, just adding this to the gfxwrapper library results in failure at load time.
        # So, each consuming target must add $<TARGET_OBJECTS:android_native_app_glue> to their sources
        add_library(android_native_app_glue OBJECT "${ANDROID_NATIVE_APP_GLUE}/android_native_app_glue.c")
        target_include_directories(android_native_app_glue PUBLIC ${ANDROID_NATIVE_APP_GLUE})
        target_compile_options(android_native_app_glue PRIVATE -Wno-unused-parameter)
    endif()
    message(STATUS "Enabling OpenGL support")
endif()


# Determine the presentation backend for Linux systems.
# Use an include because the code is pretty big.
if(CMAKE_SYSTEM_NAME STREQUAL "Linux")
    include(presentation)
endif()

# Several files use these compile-time platform switches
if(WIN32)
    add_definitions(-DXR_USE_PLATFORM_WIN32)
elseif(ANDROID)
    add_definitions(-DXR_USE_PLATFORM_ANDROID)
elseif(PRESENTATION_BACKEND MATCHES "xlib")
    add_definitions(-DXR_USE_PLATFORM_XLIB)
elseif(PRESENTATION_BACKEND MATCHES "xcb")
    add_definitions(-DXR_USE_PLATFORM_XCB)
elseif(PRESENTATION_BACKEND MATCHES "wayland")
    add_definitions(-DXR_USE_PLATFORM_WAYLAND)
endif()

# Find glslc shader compiler.
# On Android, the NDK includes the binary, so no external dependency.
if(ANDROID)
    file(GLOB glslc_folders ${ANDROID_NDK}/shader-tools/*)
    find_program(
        GLSL_COMPILER glslc
        PATHS ${glslc_folders}
        NO_DEFAULT_PATH
    )
else()
    file(GLOB glslc_folders $ENV{VULKAN_SDK}/*)
    find_program(GLSL_COMPILER glslc PATHS ${glslc_folders})
endif()
find_program(GLSLANG_VALIDATOR glslangValidator)
if(GLSL_COMPILER)
    message(STATUS "Found glslc: ${GLSL_COMPILER}")
elseif(GLSLANG_VALIDATOR)
    message(STATUS "Found glslangValidator: ${GLSLANG_VALIDATOR}")
else()
    message(STATUS "Could NOT find glslc, using precompiled .spv files")
endif()

function(compile_glsl run_target_name)
    set(glsl_output_files "")
    foreach(in_file IN LISTS ARGN)
        get_filename_component(glsl_stage ${in_file} NAME_WE)
        set(out_file ${CMAKE_CURRENT_BINARY_DIR}/${glsl_stage}.spv)
        if(GLSL_COMPILER)
            # Run glslc if we can find it
            add_custom_command(
                OUTPUT ${out_file}
                COMMAND ${GLSL_COMPILER} -mfmt=c -fshader-stage=${glsl_stage} ${in_file} -o ${out_file}
                DEPENDS ${in_file}
            )
        elseif(GLSLANG_VALIDATOR)
            # Run glslangValidator if we can find it
            add_custom_command(
                OUTPUT ${out_file}
                COMMAND ${GLSLANG_VALIDATOR} -V -S ${glsl_stage} ${in_file} -x -o ${out_file}
                DEPENDS ${in_file}
                VERBATIM
            )
        else()
            # Use the precompiled .spv files
            get_filename_component(glsl_src_dir ${in_file} DIRECTORY)
            set(precompiled_file ${glsl_src_dir}/${glsl_stage}.spv)
            configure_file(${precompiled_file} ${out_file} COPYONLY)
        endif()
        list(APPEND glsl_output_files ${out_file})
    endforeach()
    add_custom_target(${run_target_name} ALL DEPENDS ${glsl_output_files})
    set_target_properties(${run_target_name} PROPERTIES FOLDER ${HELPER_FOLDER})

endfunction()

if(WIN32)
    add_definitions(-DXR_USE_GRAPHICS_API_D3D11)
    if(MSVC)
        # Not available in MinGW right now
        add_definitions(-DXR_USE_GRAPHICS_API_D3D12)
    endif()
endif()

# Check for the existence of the secure_getenv or __secure_getenv commands
include(CheckFunctionExists)

check_function_exists(secure_getenv HAVE_SECURE_GETENV)
check_function_exists(__secure_getenv HAVE___SECURE_GETENV)
configure_file(${CMAKE_CURRENT_SOURCE_DIR}/common_config.h.in ${CMAKE_CURRENT_BINARY_DIR}/common_config.h)
add_definitions(-DOPENXR_HAVE_COMMON_CONFIG)

# Be able to find pre-generated files, if used.
include_directories(${CMAKE_CURRENT_SOURCE_DIR}/../include ${CMAKE_CURRENT_SOURCE_DIR})

include(CheckSymbolExists)
check_symbol_exists(timespec_get time.h HAVE_TIMESPEC_GET)
if(HAVE_TIMESPEC_GET)
    add_definitions(-DXR_USE_TIMESPEC)
endif()

if(NOT MSVC)
    include(CheckCXXCompilerFlag)
    include(CheckCCompilerFlag)
    foreach(FLAG -Wall -Werror=unused-parameter -Werror=unused-argument -Wpointer-arith)
        string(REGEX REPLACE "[^A-Za-z0-9]" "" _flagvar "${FLAG}")
        check_cxx_compiler_flag(${FLAG} SUPPORTS_${_flagvar})
        if(SUPPORTS_${_flagvar})
            set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${FLAG}")
        endif()
        check_c_compiler_flag(${FLAG} SUPPORTS_C_${_flagvar})
        if(SUPPORTS_C_${_flagvar})
            set(CMAKE_C_FLAGS "${CMAKE_C_FLAGS} ${FLAG}")
        endif()
    endforeach()
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--no-undefined")
endif()

if(ANDROID)
    find_library(ANDROID_LIBRARY NAMES android)
    find_library(ANDROID_LOG_LIBRARY NAMES log)
endif()

add_subdirectory(hello_xr)
