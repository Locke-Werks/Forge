cmake_minimum_required(VERSION 3.28)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(LwiVersion)

project(lwi VERSION ${LWI_VERSION} LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "lwi targets Windows only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# x64 only. This is not a portability oversight: it removes WOW64 registry and
# Program Files redirection from the problem entirely, and UAC Installer
# Detection heuristics, which auto-elevate anything whose name looks like a
# setup program, only reach 32-bit images.
if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "lwi is x64 only. Configure with an x64 toolchain.")
endif()

option(LWI_BUILD_TESTS "Build the test suite" ON)
option(LWI_BUILD_TOOLS "Build lwforge" ON)
option(LWI_BUILD_STUB  "Build lwstub and lwuninst" ON)
option(LWI_WERROR      "Treat warnings as errors" OFF)
option(LWI_HARDENING   "Apply exploit-mitigation compiler and linker flags" ON)

include(LwiWarnings)
include(LwiHardening)
include(LwiManifest)

add_subdirectory(src/core)

if(LWI_BUILD_STUB)
    add_subdirectory(src/stub)
endif()

if(LWI_BUILD_TOOLS)
    add_subdirectory(src/forge)
endif()

if(LWI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

message(STATUS "lwi ${LWI_VERSION}  tests=${LWI_BUILD_TESTS} tools=${LWI_BUILD_TOOLS} "
               "stub=${LWI_BUILD_STUB} hardening=${LWI_HARDENING} werror=${LWI_WERROR}")
