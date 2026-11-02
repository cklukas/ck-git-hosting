# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.24)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
  message(FATAL_ERROR "Choose an explicit out-of-tree build directory")
endif()
project(ck_git_hosting LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# A portable executable must not require an administrator to install the
# Visual C++ redistributable. Keep Debug and Release runtime choices distinct.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
include(CTest)
if(APPLE)
  enable_language(OBJCXX)
  add_executable(ckgit-macos-vm src/ci-vm-macos/main.mm)
  set_target_properties(ckgit-macos-vm PROPERTIES OBJCXX_STANDARD 20 OBJCXX_STANDARD_REQUIRED YES)
  target_compile_options(ckgit-macos-vm PRIVATE -fobjc-arc)
  target_link_libraries(ckgit-macos-vm PRIVATE "-framework AppKit" "-framework Foundation" "-framework Virtualization")
  add_custom_command(TARGET ckgit-macos-vm POST_BUILD
    COMMAND /usr/bin/codesign --force --sign - --entitlements
      "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/virtualization.entitlements" "$<TARGET_FILE:ckgit-macos-vm>"
    VERBATIM)
  if(BUILD_TESTING)
    add_executable(ckgit-macos-vsock-probe tests/platform/macos_vsock_probe.cpp)
    add_executable(ckgit-macos-vm-process-test tests/platform/macos_vm_process_test.cpp
      src/common/macos_vm_process.cpp)
    target_include_directories(ckgit-macos-vm-process-test PRIVATE include)
    add_test(NAME macos-vm-process COMMAND ckgit-macos-vm-process-test)
    add_test(NAME macos-vm-mount-guard COMMAND /bin/sh
      "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/macos_vm_mount_guard.sh" "$<TARGET_FILE:ckgit-macos-vm>")
    set_tests_properties(macos-vm-mount-guard PROPERTIES
      ENVIRONMENT "TMPDIR=${CMAKE_BINARY_DIR}/test-tmp" TIMEOUT 30)
  endif()
endif()
find_package(Threads REQUIRED)

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
  src/common/ci_workflow.cpp src/common/ci_dependency_lock.cpp src/common/ci_vm_protocol.cpp src/common/cli_help.cpp src/common/control_rpc.cpp
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
add_subdirectory(third_party/libarchive)
add_library(ckgit_archive STATIC src/archive/tar.cpp)
target_link_libraries(ckgit_archive PUBLIC ckgit_portable PRIVATE ckgit_pinned_archive)
if(WIN32)
  target_sources(ckgit_portable PRIVATE src/common/windows.cpp src/common/private_file_windows.cpp)
  target_link_libraries(ckgit_portable PUBLIC bcrypt ws2_32 advapi32)
  add_library(ckgit_process STATIC src/common/process_windows.cpp src/common/windows_sandbox.cpp)
  target_link_libraries(ckgit_process PUBLIC userenv advapi32 user32)
  add_executable(ckgit-job-launcher src/ci-job-launcher/main.cpp)
  target_link_libraries(ckgit-job-launcher PRIVATE ckgit_portable advapi32 user32)
else()
  add_library(ckgit_process STATIC src/common/process.cpp)
endif()
target_link_libraries(ckgit_process PUBLIC ckgit_portable)
target_sources(ckgit_process PRIVATE src/common/process_tools.cpp)
add_library(ckgit_remote_transport STATIC src/common/ci_remote_transport.cpp)
target_link_libraries(ckgit_remote_transport PUBLIC ckgit_process)
add_library(ckgit_client_support STATIC src/common/git_repository.cpp src/common/ci_checkout.cpp)
target_link_libraries(ckgit_client_support PUBLIC ckgit_process)
add_library(ckgit_remote STATIC src/common/ci_remote.cpp)
target_link_libraries(ckgit_remote PUBLIC ckgit_remote_transport ckgit_client_support ckgit_archive)
if(WIN32)
  add_library(ckgit_windows_executor STATIC src/common/windows_ci_executor.cpp)
  target_link_libraries(ckgit_windows_executor PUBLIC ckgit_remote ckgit_archive)
  add_executable(ck-ci-runnerd src/ci-runner-windows/main.cpp)
  target_link_libraries(ck-ci-runnerd PRIVATE ckgit_windows_executor)
  add_dependencies(ck-ci-runnerd ckgit-job-launcher)
  add_executable(ckgit src/client/main.cpp)
  target_link_libraries(ckgit PRIVATE ckgit_client_support shell32)
endif()

if(NOT WIN32)
  file(GLOB HOST_SOURCES CONFIGURE_DEPENDS src/common/*.cpp src/common/web/*.cpp)
  list(TRANSFORM HOST_SOURCES REPLACE "^${CMAKE_CURRENT_SOURCE_DIR}/" "")
  list(REMOVE_ITEM HOST_SOURCES ${PORTABLE_SOURCES} src/common/process.cpp src/common/process_tools.cpp
    src/common/process_windows.cpp src/common/windows.cpp src/common/windows_sandbox.cpp
    src/common/private_file_windows.cpp src/common/git_repository.cpp src/common/docs_site.cpp
    src/common/ci_remote_transport.cpp src/common/ci_checkout.cpp src/common/ci_remote.cpp src/common/windows_ci_executor.cpp)
  add_library(ckgit_host STATIC ${HOST_SOURCES})
  target_link_libraries(ckgit_host PUBLIC ckgit_docs ckgit_remote)
  foreach(pair "ckgit|client" "ckgit-admin|admin" "ck-git-shell|ssh-dispatcher"
      "post-receive|receive-hook" "ck-git-hostingd|server" "ck-ci-runnerd|ci-runner" "ck-pagesd|pages-server")
    string(REPLACE "|" ";" parts "${pair}")
    list(GET parts 0 name)
    list(GET parts 1 directory)
    add_executable(${name} src/${directory}/main.cpp)
    target_link_libraries(${name} PRIVATE ckgit_host)
  endforeach()
  if(APPLE)
    add_executable(ckgit-macos-native src/ci-native-macos/main.cpp src/common/macos_native.cpp)
    target_link_libraries(ckgit-macos-native PRIVATE ckgit_process)
    add_executable(ckgit-macos-vm-guest src/ci-vm-guest-macos/main.cpp
      src/ci-vm-guest-macos/staged_source.cpp src/ci-vm-guest-macos/result_sink.cpp
      src/ci-vm-guest-macos/tool_environment.cpp)
    target_link_libraries(ckgit-macos-vm-guest PRIVATE ckgit_host)
    target_sources(ck-ci-runnerd PRIVATE src/ci-vm-host-macos/run_job.cpp src/ci-vm-host-macos/result_relay.cpp)
    # Keep the helper adjacent to the administrator and runner executables:
    # both resolve it from their own installed executable directory.
    install(TARGETS ckgit-admin ck-ci-runnerd ckgit-macos-vm ckgit-macos-vm-guest
      RUNTIME DESTINATION bin COMPONENT macos-vm-runner)
    install(PROGRAMS packaging/macos/install-vm-guest.sh
      DESTINATION share/ck-git-hosting/macos COMPONENT macos-vm-runner)
    if(BUILD_TESTING)
      add_executable(ckgit-macos-vm-guest-test tests/platform/macos_vm_guest_test.cpp)
      target_link_libraries(ckgit-macos-vm-guest-test PRIVATE ckgit_portable)
      add_test(NAME macos-vm-guest COMMAND ckgit-macos-vm-guest-test "$<TARGET_FILE:ckgit-macos-vm-guest>")
      add_executable(ckgit-macos-vm-staged-source-test tests/platform/macos_vm_staged_source_test.cpp
        src/ci-vm-guest-macos/staged_source.cpp)
      target_link_libraries(ckgit-macos-vm-staged-source-test PRIVATE ckgit_host)
      add_test(NAME macos-vm-staged-source COMMAND ckgit-macos-vm-staged-source-test)
      add_executable(ckgit-macos-vm-tool-environment-test tests/platform/macos_vm_tool_environment_test.cpp
        src/ci-vm-guest-macos/tool_environment.cpp)
      target_link_libraries(ckgit-macos-vm-tool-environment-test PRIVATE ckgit_options)
      add_test(NAME macos-vm-tool-environment COMMAND ckgit-macos-vm-tool-environment-test)
      add_executable(ckgit-macos-vm-result-relay-test tests/platform/macos_vm_result_relay_test.cpp
        src/ci-vm-host-macos/result_relay.cpp src/ci-vm-guest-macos/result_sink.cpp)
      target_link_libraries(ckgit-macos-vm-result-relay-test PRIVATE ckgit_host)
      add_test(NAME macos-vm-result-relay COMMAND ckgit-macos-vm-result-relay-test)
      add_executable(ckgit-macos-vm-run-job-test tests/platform/macos_vm_run_job_test.cpp
        src/ci-vm-host-macos/run_job.cpp src/ci-vm-host-macos/result_relay.cpp src/ci-vm-guest-macos/result_sink.cpp)
      target_link_libraries(ckgit-macos-vm-run-job-test PRIVATE ckgit_host)
      add_test(NAME macos-vm-run-job COMMAND ckgit-macos-vm-run-job-test)
      add_test(NAME macos-vm-startup COMMAND /bin/sh
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/macos_vm_startup.sh" "$<TARGET_FILE:ck-ci-runnerd>" "$<TARGET_FILE:ckgit-admin>")
      add_test(NAME macos-vm-prepare-share COMMAND /bin/sh
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/macos_vm_prepare_share.sh" "$<TARGET_FILE:ckgit-admin>"
        "$<TARGET_FILE:ckgit-macos-vm-guest>" "${CMAKE_CURRENT_SOURCE_DIR}/packaging/macos/install-vm-guest.sh")
      add_executable(ckgit-macos-vm-real-job-test tests/platform/macos_vm_real_job_test.cpp
        src/ci-vm-host-macos/run_job.cpp src/ci-vm-host-macos/result_relay.cpp)
      target_link_libraries(ckgit-macos-vm-real-job-test PRIVATE ckgit_host)
      add_executable(ckgit-macos-vm-guest-fd-probe tests/platform/macos_vm_guest_fd_probe.cpp)
      set_tests_properties(macos-vm-staged-source macos-vm-result-relay macos-vm-run-job macos-vm-startup macos-vm-prepare-share PROPERTIES
        ENVIRONMENT "TMPDIR=${CMAKE_BINARY_DIR}/test-tmp" TIMEOUT 60)
    endif()
    add_dependencies(ckgit-admin ckgit-macos-native ckgit-macos-vm ckgit-macos-vm-guest ck-ci-runnerd)
    if(BUILD_TESTING)
      add_test(NAME macos-native-policy COMMAND /bin/sh
        "${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/macos_native_policy.sh"
        "$<TARGET_FILE:ckgit-macos-native>" "${CMAKE_BINARY_DIR}/test-tmp")
      set_tests_properties(macos-native-policy PROPERTIES TIMEOUT 30)
    endif()
  endif()
  set_target_properties(post-receive PROPERTIES RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/hooks")
  if(BUILD_TESTING)
    file(GLOB UNIT_SOURCES CONFIGURE_DEPENDS tests/unit/*.cpp)
    add_executable(ckgit-unit-tests ${UNIT_SOURCES})
    target_link_libraries(ckgit-unit-tests PRIVATE ckgit_host)
    add_dependencies(ckgit-unit-tests ckgit-admin)
    add_test(NAME unit COMMAND ckgit-unit-tests)
    set_tests_properties(unit PROPERTIES ENVIRONMENT
      "CKGIT_TEST_ROOT=${CMAKE_BINARY_DIR};CKGIT_SOURCE_ROOT=${CMAKE_SOURCE_DIR};TMPDIR=${CMAKE_BINARY_DIR}/test-tmp")
    add_test(NAME ci-distributed COMMAND ckgit-unit-tests --ci-distributed-only)
    set_tests_properties(ci-distributed PROPERTIES ENVIRONMENT
      "CKGIT_TEST_ROOT=${CMAKE_BINARY_DIR};CKGIT_SOURCE_ROOT=${CMAKE_SOURCE_DIR};TMPDIR=${CMAKE_BINARY_DIR}/test-tmp")
    add_test(NAME ci-dependency COMMAND ckgit-unit-tests --ci-dependency-only)
    set_tests_properties(ci-dependency PROPERTIES ENVIRONMENT
      "CKGIT_TEST_ROOT=${CMAKE_BINARY_DIR};CKGIT_SOURCE_ROOT=${CMAKE_SOURCE_DIR};TMPDIR=${CMAKE_BINARY_DIR}/test-tmp" TIMEOUT 60)
    file(MAKE_DIRECTORY "${CMAKE_BINARY_DIR}/test-tmp")
    if(APPLE)
      add_executable(ckgit-macos-vm-offline-probe tests/platform/macos_vm_offline_probe.cpp)
      target_link_libraries(ckgit-macos-vm-offline-probe PRIVATE ckgit_host)
      add_executable(ckgit-macos-vm-disconnect-test tests/platform/macos_vm_disconnect_test.cpp)
      target_link_libraries(ckgit-macos-vm-disconnect-test PRIVATE ckgit_host)
      add_executable(ckgit-macos-vm-owner-crash-test tests/platform/macos_vm_owner_crash_test.cpp)
      target_link_libraries(ckgit-macos-vm-owner-crash-test PRIVATE ckgit_host)
    endif()
  endif()
endif()

add_library(ckgit_docs STATIC src/common/docs_site.cpp)
target_link_libraries(ckgit_docs PUBLIC ckgit_process ckgit_archive)
  file(GLOB MERMAID_SOURCES CONFIGURE_DEPENDS src/ckdocs/mermaid/cdiagram/src/*.cpp
    src/ckdocs/mermaid/cplot/src/*.cpp src/ckdocs/mermaid/libcworks/src/*.cpp)
  add_library(ckdocs_mermaid STATIC ${MERMAID_SOURCES} src/ckdocs/mermaid.cpp)
  # GCC 14 at -O3 reports maybe-uninitialized diagnostics inside std::variant
  # moves. Match the Make build's -O2 for this library while retaining all
  # warning-as-error checks.
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(ckdocs_mermaid PRIVATE -O2)
  endif()
  target_link_libraries(ckdocs_mermaid PUBLIC ckgit_options)
  target_include_directories(ckdocs_mermaid PUBLIC src/ckdocs/mermaid/cdiagram/include
    src/ckdocs/mermaid/cplot/include src/ckdocs/mermaid/libcworks/include)
  add_executable(ckdocs src/ckdocs/main.cpp)
  target_link_libraries(ckdocs PRIVATE ckgit_docs ckdocs_mermaid)

if(BUILD_TESTING)
  if(WIN32)
    add_executable(ckgit-lpac-probe tests/platform/windows_sandbox_probe.cpp)
    target_link_libraries(ckgit-lpac-probe PRIVATE ckgit_windows_executor)
    add_executable(ckgit-windows-self-build tests/platform/windows_self_build.cpp)
    target_link_libraries(ckgit-windows-self-build PRIVATE ckgit_windows_executor)
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
  add_executable(ckgit-archive-tests tests/platform/archive_tests.cpp)
  target_link_libraries(ckgit-archive-tests PRIVATE ckgit_archive ckgit_pinned_archive)
  add_test(NAME archive COMMAND ckgit-archive-tests "${CMAKE_BINARY_DIR}/test-tmp")
  add_executable(ckgit-process-tests tests/platform/process_tests.cpp)
  target_link_libraries(ckgit-process-tests PRIVATE ckgit_remote)
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
  install(TARGETS ckgit ckdocs ck-ci-runnerd ckgit-job-launcher RUNTIME DESTINATION bin)
  install(FILES LICENSE DESTINATION .)
  install(FILES third_party/libarchive/COPYING third_party/libarchive/SOURCE-NOTICES.txt DESTINATION licenses/libarchive)
  set(CPACK_GENERATOR ZIP)
  set(CPACK_PACKAGE_NAME ckgit)
  set(CPACK_PACKAGE_VERSION "${CKGIT_RELEASE_VERSION}")
  set(CPACK_PACKAGE_FILE_NAME "ckgit-${CKGIT_RELEASE_VERSION}-windows-x64")
  include(CPack)
endif()
