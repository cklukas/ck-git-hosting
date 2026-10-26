# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "Choose an explicit out-of-tree build directory")
endif()
project(ck_git_hosting LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# A portable executable must not require an administrator to install the
# Visual C++ redistributable. Keep Debug and Release runtime choices distinct.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
find_package(Threads REQUIRED)
include(CTest)

add_library(ckgit_options INTERFACE)
target_include_directories(ckgit_options INTERFACE include)
target_link_libraries(ckgit_options INTERFACE Threads::Threads)
if(MSVC)
  target_compile_options(ckgit_options INTERFACE /W4 /permissive- /utf-8 /EHsc /Zc:__cplusplus)
  target_compile_definitions(ckgit_options INTERFACE NOMINMAX WIN32_LEAN_AND_MEAN _WIN32_WINNT=0x0A00)
  target_link_options(ckgit_options INTERFACE "/MANIFESTINPUT:${CMAKE_CURRENT_SOURCE_DIR}/packaging/windows/utf8.manifest")
else()
  target_compile_options(ckgit_options INTERFACE -Wall -Wextra -Wpedantic -Werror)
  target_compile_definitions(ckgit_options INTERFACE _GNU_SOURCE)
endif()

# Protocol grammars and rendering must not depend on the hosting store or
# Unix socket transport. Windows builds this layer without POSIX headers.
set(PORTABLE_SOURCES
  src/common/checkout_path.cpp src/common/ci_types.cpp src/common/sync_lock.cpp src/common/directory_identity.cpp
  src/common/client_config.cpp src/common/client_state.cpp src/common/runner_config.cpp
  src/common/ci_control.cpp src/common/server_identity.cpp src/common/ci_runner_protocol.cpp
  src/common/release_control.cpp src/common/display_format.cpp
  src/common/ci_workflow.cpp src/common/cli_help.cpp src/common/control_rpc.cpp
  src/common/docs_i18n.cpp src/common/emoji.cpp src/common/hash.cpp
  src/common/tree_file.cpp src/common/temp_directory.cpp src/common/site_file.cpp
  src/common/highlight.cpp src/common/http_request.cpp src/common/http_router.cpp
  src/common/markdown.cpp src/common/math.cpp src/common/ref_status.cpp
  src/common/remote_url.cpp src/common/ssh_command.cpp src/common/text.cpp
  src/common/validation.cpp src/common/windows_command_line.cpp src/common/yaml_subset.cpp)
add_library(ckgit_portable STATIC ${PORTABLE_SOURCES})
target_link_libraries(ckgit_portable PUBLIC ckgit_options)
file(STRINGS VERSION CKGIT_RELEASE_VERSION LIMIT_COUNT 1)
set(CKGIT_BUILD_VERSION "${CKGIT_RELEASE_VERSION}+source" CACHE STRING "Exact version stamped into the binaries")
target_compile_definitions(ckgit_portable PRIVATE CKGIT_BUILD_VERSION="${CKGIT_BUILD_VERSION}")
if(WIN32)
  target_sources(ckgit_portable PRIVATE src/common/windows.cpp src/common/private_file_windows.cpp)
  target_link_libraries(ckgit_portable PUBLIC bcrypt ws2_32 advapi32)
  add_library(ckgit_process STATIC src/common/process_windows.cpp src/common/windows_sandbox.cpp)
  target_link_libraries(ckgit_process PUBLIC userenv advapi32 user32)
  add_executable(ckgit-job-launcher src/ci-job-launcher/main.cpp)
  target_link_libraries(ckgit-job-launcher PRIVATE ckgit_portable advapi32)
else()
  add_library(ckgit_process STATIC src/common/process.cpp)
endif()
target_link_libraries(ckgit_process PUBLIC ckgit_portable)
add_library(ckgit_remote_transport STATIC src/common/ci_remote_transport.cpp)
target_link_libraries(ckgit_remote_transport PUBLIC ckgit_process)
add_library(ckgit_client_support STATIC src/common/git_repository.cpp src/common/ci_checkout.cpp)
target_link_libraries(ckgit_client_support PUBLIC ckgit_process)
if(WIN32)
  add_executable(ckgit src/client/main.cpp)
  target_link_libraries(ckgit PRIVATE ckgit_client_support shell32)
endif()

if(NOT WIN32)
  file(GLOB HOST_SOURCES CONFIGURE_DEPENDS src/common/*.cpp src/common/web/*.cpp)
  list(TRANSFORM HOST_SOURCES REPLACE "^${CMAKE_CURRENT_SOURCE_DIR}/" "")
  list(REMOVE_ITEM HOST_SOURCES ${PORTABLE_SOURCES} src/common/process.cpp
    src/common/process_windows.cpp src/common/windows.cpp src/common/windows_sandbox.cpp
    src/common/private_file_windows.cpp src/common/git_repository.cpp src/common/docs_site.cpp
    src/common/ci_remote_transport.cpp src/common/ci_checkout.cpp)
  add_library(ckgit_host STATIC ${HOST_SOURCES})
  target_link_libraries(ckgit_host PUBLIC ckgit_docs ckgit_client_support ckgit_remote_transport)
  foreach(pair "ckgit|client" "ckgit-admin|admin" "ck-git-shell|ssh-dispatcher"
      "post-receive|receive-hook" "ck-git-hostingd|server" "ck-ci-runnerd|ci-runner" "ck-pagesd|pages-server")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 name)
    list(GET parts 1 directory)
    add_executable(${name} src/${directory}/main.cpp)
    target_link_libraries(${name} PRIVATE ckgit_host)
  endforeach()
  set_target_properties(post-receive PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/hooks")
  if(BUILD_TESTING)
    file(GLOB UNIT_SOURCES CONFIGURE_DEPENDS tests/unit/*.cpp)
    add_executable(ckgit-unit-tests ${UNIT_SOURCES})
    target_link_libraries(ckgit-unit-tests PRIVATE ckgit_host)
    add_test(NAME unit COMMAND ckgit-unit-tests)
    set_tests_properties(unit PROPERTIES ENVIRONMENT
      "CKGIT_TEST_ROOT=${CMAKE_BINARY_DIR};CKGIT_SOURCE_ROOT=${CMAKE_SOURCE_DIR};TMPDIR=${CMAKE_BINARY_DIR}/test-tmp")
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-tmp")
  endif()
endif()

add_library(ckgit_docs STATIC src/common/docs_site.cpp)
target_link_libraries(ckgit_docs PUBLIC ckgit_process)
  file(GLOB MERMAID_SOURCES CONFIGURE_DEPENDS src/ckdocs/mermaid/cdiagram/src/*.cpp
    src/ckdocs/mermaid/cplot/src/*.cpp src/ckdocs/mermaid/libcworks/src/*.cpp)
  add_library(ckdocs_mermaid STATIC ${MERMAID_SOURCES} src/ckdocs/mermaid.cpp)
  target_link_libraries(ckdocs_mermaid PUBLIC ckgit_options)
  target_include_directories(ckdocs_mermaid PUBLIC src/ckdocs/mermaid/cdiagram/include
    src/ckdocs/mermaid/cplot/include src/ckdocs/mermaid/libcworks/include)
  add_executable(ckdocs src/ckdocs/main.cpp)
  target_link_libraries(ckdocs PRIVATE ckgit_docs ckdocs_mermaid)

if(BUILD_TESTING)
  if(WIN32)
    add_executable(ckgit-lpac-probe tests/platform/windows_sandbox_probe.cpp)
    target_link_libraries(ckgit-lpac-probe PRIVATE ckgit_process)
    # Deliberately not a default CTest: the caller must approve Windows-managed
    # profile creation and select explicit staging/toolchain locations.
  endif()
  file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-tmp")
  add_executable(ckgit-repository-tests tests/platform/git_repository_tests.cpp)
  target_link_libraries(ckgit-repository-tests PRIVATE ckgit_client_support ckgit_remote_transport)
  add_test(NAME git-repository COMMAND ckgit-repository-tests "${CMAKE_BINARY_DIR}/test-tmp" "$<TARGET_FILE:ckgit>")
  set_tests_properties(git-repository PROPERTIES TIMEOUT 60)
  add_executable(ckgit-client-storage-tests tests/platform/client_storage_tests.cpp)
  target_link_libraries(ckgit-client-storage-tests PRIVATE ckgit_portable)
  add_test(NAME client-storage COMMAND ckgit-client-storage-tests "${CMAKE_BINARY_DIR}/test-tmp")
  add_executable(ckgit-sync-lock-tests tests/platform/sync_lock_tests.cpp)
  target_link_libraries(ckgit-sync-lock-tests PRIVATE ckgit_process)
  add_test(NAME sync-lock COMMAND ckgit-sync-lock-tests "${CMAKE_BINARY_DIR}/test-tmp")
  set_tests_properties(sync-lock PROPERTIES TIMEOUT 30)
  add_executable(ckgit-tree-file-tests tests/platform/tree_file_tests.cpp)
  target_link_libraries(ckgit-tree-file-tests PRIVATE ckgit_process)
  add_test(NAME tree-file COMMAND ckgit-tree-file-tests "${CMAKE_BINARY_DIR}/test-tmp")
  add_executable(ckgit-process-tests tests/platform/process_tests.cpp)
  target_link_libraries(ckgit-process-tests PRIVATE ckgit_process ckgit_remote_transport)
  add_test(NAME process COMMAND ckgit-process-tests)
  set_tests_properties(process PROPERTIES TIMEOUT 30)
  set_tests_properties(process PROPERTIES ENVIRONMENT "TMPDIR=${CMAKE_BINARY_DIR}/test-tmp")
  file(GLOB MERMAID_TEST_SOURCES CONFIGURE_DEPENDS tests/mermaid/*.cpp)
  add_executable(ckdocs-mermaid-tests ${MERMAID_TEST_SOURCES})
  target_include_directories(ckdocs-mermaid-tests PRIVATE tests/mermaid src/ckdocs src/ckdocs/mermaid/cdiagram/src)
  target_link_libraries(ckdocs-mermaid-tests PRIVATE ckdocs_mermaid)
  add_test(NAME mermaid COMMAND ckdocs-mermaid-tests)
  set_tests_properties(mermaid PROPERTIES WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}")
  if(WIN32)
    add_test(NAME docs-windows COMMAND powershell -NoProfile -ExecutionPolicy Bypass
      -File "${CMAKE_SOURCE_DIR}/tests/integration/docs_site.ps1"
      -Ckdocs "$<TARGET_FILE:ckdocs>" -TestRoot "${CMAKE_BINARY_DIR}/test-tmp")
    set_tests_properties(docs-windows PROPERTIES TIMEOUT 90)
  endif()
endif()

if(WIN32)
  install(TARGETS ckgit ckdocs RUNTIME DESTINATION bin)
  install(FILES LICENSE DESTINATION .)
  set(CPACK_GENERATOR ZIP)
  set(CPACK_PACKAGE_NAME ckgit)
  set(CPACK_PACKAGE_VERSION "${CKGIT_RELEASE_VERSION}")
  set(CPACK_PACKAGE_FILE_NAME "ckgit-${CKGIT_RELEASE_VERSION}-windows-x64")
  include(CPack)
endif()
