// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT

#pragma once

#include <string_view>

namespace ckgit {

// The packaged server layout.  packaging/install.sh creates exactly these
// paths; ckgit-admin uses them as defaults so a generated forced command
// matches the installed service without retyping.
inline constexpr std::string_view kInstalledShellPath{"/usr/bin/ck-git-shell"};
inline constexpr std::string_view kInstalledRepositoryRoot{"/srv/ck-git-hosting/repos"};
inline constexpr std::string_view kInstalledControlSocket{"/run/ck-git-hosting/control.sock"};
inline constexpr std::string_view kInstalledStateRoot{"/var/lib/ck-git-hosting/state"};
inline constexpr std::string_view kInstalledHookDirectory{"/usr/lib/ck-git-hosting/hooks"};
inline constexpr std::string_view kInstalledServerConfig{"/etc/ck-git-hosting/server.ini"};

}  // namespace ckgit
