# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.24)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
project(ckgit_pinned_archive LANGUAGES C)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "Choose an explicit out-of-tree build directory")
endif()

# This directory can also be built independently for the Make-based hosts.
# The upstream source is bundled so sandboxed builds never need the network.
set(bundle "${CMAKE_CURRENT_LIST_DIR}/libarchive-3.8.10.tar.xz")
set(expected "56cad765459d5ee25a3b761f90c47d99379d28297fbfb72d908880d6b875fb96")
file(SHA256 "${bundle}" actual)
if(NOT actual STREQUAL expected)
  message(FATAL_ERROR "Bundled libarchive source SHA-256 mismatch")
endif()

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Pinned archive is static" FORCE)
set(MSVC_USE_STATIC_CRT ON CACHE BOOL "No redistributable prerequisite" FORCE)
# Never probe third-party regex libraries. Windows has no libc regex, and
# the TAR reader/writer does not require archive_match's regex support.
set(POSIX_REGEX_LIB "LIBC" CACHE STRING "Use only built-in regex when available" FORCE)
foreach(feature MBEDTLS NETTLE OPENSSL MD LIBB2 LZ4 LZO LZMA ZSTD ZLIB BZip2
    LIBXML2 BSDXML EXPAT WIN32_XMLLITE PCREPOSIX PCRE2POSIX LIBGCC CNG
    TAR CPIO CAT UNZIP XATTR ACL ICONV TEST COVERAGE INSTALL CLANG_TIDY WERROR)
  set(ENABLE_${feature} OFF CACHE BOOL "Not needed for uncompressed TAR" FORCE)
endforeach()
# The bundle is already local and verified. FetchContent's population sub-build
# adds long generated paths and another build-tool invocation inside LPAC.
# Extract directly with CMake instead, retaining the CMake 3.24 minimum.
set(ckgit_libarchive_SOURCE_DIR "${CMAKE_BINARY_DIR}/_deps/la-src")
set(extract_stage "${CMAKE_BINARY_DIR}/_deps/la-unpack")
set(extract_stamp "${CMAKE_BINARY_DIR}/_deps/la-source.sha256")
set(extracted "")
if(EXISTS "${extract_stamp}")
  file(READ "${extract_stamp}" extracted)
endif()
if(NOT extracted STREQUAL expected OR NOT EXISTS "${ckgit_libarchive_SOURCE_DIR}/libarchive/archive.h")
  file(REMOVE_RECURSE "${extract_stage}" "${ckgit_libarchive_SOURCE_DIR}")
  file(ARCHIVE_EXTRACT INPUT "${bundle}" DESTINATION "${extract_stage}" TOUCH)
  file(RENAME "${extract_stage}/libarchive-3.8.10" "${ckgit_libarchive_SOURCE_DIR}")
  file(REMOVE_RECURSE "${extract_stage}")
  file(WRITE "${extract_stamp}" "${expected}")
endif()
add_subdirectory("${ckgit_libarchive_SOURCE_DIR}" "${CMAKE_BINARY_DIR}/_deps/la-build")

add_library(ckgit_pinned_archive INTERFACE)
target_link_libraries(ckgit_pinned_archive INTERFACE archive_static)
target_compile_definitions(ckgit_pinned_archive INTERFACE LIBARCHIVE_STATIC)
target_include_directories(ckgit_pinned_archive INTERFACE "${ckgit_libarchive_SOURCE_DIR}/libarchive")

option(CKGIT_ARCHIVE_SMOKE_TEST "Build standalone archive qualification test" ON)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR AND CKGIT_ARCHIVE_SMOKE_TEST)
  add_executable(ckgit-archive-smoke ../../tests/platform/archive_library_smoke.c)
  target_link_libraries(ckgit-archive-smoke PRIVATE ckgit_pinned_archive)
  enable_testing()
  add_test(NAME archive-library COMMAND ckgit-archive-smoke)
endif()
