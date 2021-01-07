# Copyright (c) 2017-2020 The Khronos Group Inc.
#
# SPDX-License-Identifier: Apache-2.0

cmake_minimum_required(VERSION 3.10)
project(hello_xr_hpp)

# Enable IDE GUI folders.  "Helper targets" that don't have interesting source code should set their FOLDER property to this
set_property(GLOBAL PROPERTY USE_FOLDERS ON)
set_property(GLOBAL PROPERTY PREDEFINED_TARGETS_FOLDER "CMake predefined targets")
set(HELPER_FOLDER "Helpers")
set(CODEGEN_FOLDER "Generated")
set(TESTS_FOLDER "Tests")
set(SAMPLES_FOLDER "Samples")

add_subdirectory(src)
