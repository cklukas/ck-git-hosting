# Copyright (c) 2026 C. Klukas. All rights reserved.
# SPDX-License-Identifier: MIT

# Every build artifact is deliberately kept outside the source tree.  A caller
# must select an explicit, unique build directory beneath the approved build
# root.  On the development Mac that root is /Volumes/PRO-BLADE/tmp; CI and
# package builds pass BUILD_ROOT explicitly (for example $RUNNER_TEMP).
BUILD_ROOT ?= /Volumes/PRO-BLADE/tmp
BUILD_ROOT_ABS := $(abspath $(BUILD_ROOT))
ifeq ($(strip $(BUILD_DIR)),)
$(error Set BUILD_DIR to a unique directory beneath $(BUILD_ROOT_ABS))
endif

BUILD_DIR_ABS := $(abspath $(BUILD_DIR))
ifneq ($(filter $(BUILD_ROOT_ABS)/%,$(BUILD_DIR_ABS)),$(BUILD_DIR_ABS))
$(error BUILD_DIR must be beneath $(BUILD_ROOT_ABS))
endif

CXX ?= c++
CPPFLAGS := -D_GNU_SOURCE -Iinclude
CXXFLAGS ?= -std=c++20 -Wall -Wextra -Wpedantic -Werror -O2
LDFLAGS ?= -pthread

COMMON_SOURCES := \
	src/common/authorized_keys.cpp \
	src/common/client_config.cpp \
	src/common/client_state.cpp \
	src/common/control_rpc.cpp \
	src/common/git_repository.cpp \
	src/common/http_request.cpp \
	src/common/metadata_store.cpp \
	src/common/process.cpp \
	src/common/project_summary.cpp \
	src/common/repository_store.cpp \
	src/common/remote_url.cpp \
	src/common/ref_status.cpp \
	src/common/server_config.cpp \
	src/common/server_identity.cpp \
	src/common/ssh_command.cpp \
	src/common/text.cpp \
	src/common/validation.cpp \
	src/common/web_renderer.cpp
CLIENT_SOURCES := src/client/main.cpp
ADMIN_SOURCES := src/admin/main.cpp
DISPATCHER_SOURCES := src/ssh-dispatcher/main.cpp
RECEIVE_HOOK_SOURCES := src/receive-hook/main.cpp
SERVER_SOURCES := src/server/main.cpp
TEST_SOURCES := tests/unit/test_main.cpp

CKGIT := $(BUILD_DIR_ABS)/bin/ckgit
CKGIT_ADMIN := $(BUILD_DIR_ABS)/bin/ckgit-admin
CK_GIT_SHELL := $(BUILD_DIR_ABS)/bin/ck-git-shell
CK_GIT_POST_RECEIVE := $(BUILD_DIR_ABS)/hooks/post-receive
CK_GIT_HOSTINGD := $(BUILD_DIR_ABS)/bin/ck-git-hostingd
TEST_BIN := $(BUILD_DIR_ABS)/bin/ckgit-unit-tests

.PHONY: all client check test

all: $(CKGIT) $(CKGIT_ADMIN) $(CK_GIT_SHELL) $(CK_GIT_POST_RECEIVE) $(CK_GIT_HOSTINGD)

# The macOS/Linux client alone; used by the Homebrew formula.
client: $(CKGIT)

check: test

$(CKGIT): $(COMMON_SOURCES) $(CLIENT_SOURCES) | $(BUILD_DIR_ABS)/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(COMMON_SOURCES) $(CLIENT_SOURCES) $(LDFLAGS) -o $@

$(CKGIT_ADMIN): $(COMMON_SOURCES) $(ADMIN_SOURCES) | $(BUILD_DIR_ABS)/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(COMMON_SOURCES) $(ADMIN_SOURCES) $(LDFLAGS) -o $@

$(CK_GIT_SHELL): $(COMMON_SOURCES) $(DISPATCHER_SOURCES) | $(BUILD_DIR_ABS)/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(COMMON_SOURCES) $(DISPATCHER_SOURCES) $(LDFLAGS) -o $@

$(CK_GIT_POST_RECEIVE): $(COMMON_SOURCES) $(RECEIVE_HOOK_SOURCES) | $(BUILD_DIR_ABS)/hooks
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(COMMON_SOURCES) $(RECEIVE_HOOK_SOURCES) $(LDFLAGS) -o $@

$(CK_GIT_HOSTINGD): $(COMMON_SOURCES) $(SERVER_SOURCES) | $(BUILD_DIR_ABS)/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(COMMON_SOURCES) $(SERVER_SOURCES) $(LDFLAGS) -o $@

$(TEST_BIN): $(COMMON_SOURCES) $(TEST_SOURCES) | $(BUILD_DIR_ABS)/bin
	$(CXX) $(CPPFLAGS) $(CXXFLAGS) $(COMMON_SOURCES) $(TEST_SOURCES) $(LDFLAGS) -o $@

$(BUILD_DIR_ABS)/bin:
	mkdir -p $@

$(BUILD_DIR_ABS)/hooks:
	mkdir -p $@

test: $(TEST_BIN) $(CKGIT) $(CKGIT_ADMIN) $(CK_GIT_SHELL) $(CK_GIT_POST_RECEIVE) $(CK_GIT_HOSTINGD)
	mkdir -p $(BUILD_DIR_ABS)/test-tmp
	CKGIT_TEST_ROOT=$(BUILD_ROOT_ABS) TMPDIR=$(BUILD_DIR_ABS)/test-tmp $(TEST_BIN)
	CKGIT_TEST_ROOT=$(BUILD_ROOT_ABS) CKGIT=$(CKGIT) CKGIT_ADMIN=$(CKGIT_ADMIN) CK_GIT_SHELL=$(CK_GIT_SHELL) CKGIT_POST_RECEIVE=$(CK_GIT_POST_RECEIVE) CKGIT_HOSTINGD=$(CK_GIT_HOSTINGD) TMPDIR=$(BUILD_DIR_ABS)/test-tmp sh tests/integration/control_socket.sh
	CKGIT_TEST_ROOT=$(BUILD_ROOT_ABS) CKGIT_BUILD_DIR=$(BUILD_DIR_ABS) CKGIT_ADMIN=$(CKGIT_ADMIN) CK_GIT_SHELL=$(CK_GIT_SHELL) CKGIT_HOSTINGD=$(CK_GIT_HOSTINGD) TMPDIR=$(BUILD_DIR_ABS)/test-tmp sh tests/integration/install.sh
