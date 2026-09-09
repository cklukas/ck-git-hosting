// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Included at the end of the client's anonymous namespace, after SSH helpers.

std::string setupQuote(std::string_view value) {
  std::string quoted = "'";
  for (const char c : value) quoted += c == '\'' ? "'\\''" : std::string(1, c);
  return quoted + "'";
}

std::filesystem::path setupPath(const std::string& value) {
  std::filesystem::path path = value;
  if (value == "~" || value.starts_with("~/")) {
    const char* home = std::getenv("HOME");
    if (!home || !*home) throw std::runtime_error("HOME is unavailable; enter an absolute path");
    path = std::filesystem::path(home) / (value == "~" ? "" : value.substr(2));
  }
  return std::filesystem::absolute(path).lexically_normal();
}

unsigned short setupPort(const std::string& value, const std::string& option) {
  unsigned int port = 0;
  const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), port);
  if (error != std::errc{} || end != value.data() + value.size() || port == 0 || port > 65535)
    throw std::invalid_argument(option + " requires a port between 1 and 65535");
  return static_cast<unsigned short>(port);
}

std::string setupPrompt(const std::string& label, const std::string& current, bool required = false) {
  while (true) {
    std::cout << label << (current.empty() ? "" : " [" + current + "]") << ": " << std::flush;
    std::string answer;
    if (!std::getline(std::cin, answer)) throw std::runtime_error("setup cancelled before saving configuration");
    if (answer.empty()) answer = current;
    if (!required || !answer.empty()) return answer;
    std::cout << "A value is required.\n";
  }
}

int setupCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> requested;
  std::map<std::string, std::string> values;
  std::vector<std::filesystem::path> roots;
  bool yes = false, dry_run = false, overwrite = false;
  try {
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const auto& option = arguments[index];
      if (option == "--yes") yes = true;
      else if (option == "--dry-run") dry_run = true;
      else if (option == "--overwrite") overwrite = true;
      else if ((option == "--config" || option == "--server" || option == "--client-id" ||
                option == "--display-name" || option == "--scan-root" || option == "--remote-name" ||
                option == "--public-path-mode" || option == "--web-host" || option == "--remote-port") &&
               index + 1 < arguments.size()) {
        const auto value = arguments[++index];
        if (option == "--config") requested = setupPath(value);
        else if (option == "--scan-root") {
          const auto root = setupPath(value);
          if (std::find(roots.begin(), roots.end(), root) == roots.end()) roots.push_back(root);
        } else values[option] = value;
      } else throw std::invalid_argument("unknown setup option or missing value: " + option);
    }
    const auto path = requested ? *requested : defaultClientConfigPath();
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    const bool exists = status.type() != std::filesystem::file_type::not_found && error != std::errc::no_such_file_or_directory;
    if (error && error != std::errc::no_such_file_or_directory)
      throw std::runtime_error("cannot inspect configuration: " + path.string());
    if (exists && (!std::filesystem::is_regular_file(status) || std::filesystem::is_symlink(status)))
      throw std::runtime_error("existing configuration must be a regular non-symlink file: " + path.string());
    if (exists && !overwrite) {
      std::cout << "Existing configuration kept: " << path.string() << "\n"
                << "Use ckgit setup --config " << setupQuote(path.string())
                << " --overwrite to review and replace it, or ckgit doctor to check the connection.\n";
      return values.empty() && roots.empty() ? 0 : kUsage;
    }
    ckgit::ClientConfig config;
    if (exists) {
      try { config = ckgit::loadClientConfig(path); }
      catch (const std::exception& failure) {
        std::cout << "Existing configuration needs repair: " << failure.what()
                  << "\nIts contents remain unchanged until you confirm the replacement.\n";
      }
    }
    char host[256]{};
    if (gethostname(host, sizeof(host) - 1) != 0) std::strcpy(host, "device");
    if (config.display_name.empty()) config.display_name = host;
    if (config.client_id.empty()) {
      for (const unsigned char c : std::string(host)) {
        if (config.client_id.size() == 64) break;
        config.client_id += std::isalnum(c) != 0 || c == '_' || c == '-' ? static_cast<char>(c) : '-';
      }
      if (!ckgit::isValidClientId(config.client_id)) config.client_id = "device";
    }
    if (config.remote_name.empty()) config.remote_name = "ckgit";
    const auto assign = [&](const std::string& option, std::string& field) {
      if (values.contains(option)) field = values.at(option);
    };
    assign("--server", config.server); assign("--client-id", config.client_id);
    assign("--display-name", config.display_name); assign("--remote-name", config.remote_name);
    assign("--web-host", config.web_host);
    if (values.contains("--remote-port")) config.web_port = setupPort(values.at("--remote-port"), "--remote-port");
    if (values.contains("--public-path-mode")) {
      const auto& mode = values.at("--public-path-mode");
      if (mode != "basename" && mode != "full") throw std::invalid_argument("--public-path-mode must be basename or full");
      config.expose_full_paths = mode == "full";
    }
    if (!roots.empty()) config.scan_roots = roots;
    const bool guided = !yes && !dry_run && isatty(STDIN_FILENO) != 0;
    if (guided) {
      std::cout << "Connect this device to ck-git-hosting. Existing Git repositories and SSH keys are unchanged.\n"
                   "Git uses the restricted ckgit account; dashboard forwarding uses your ordinary SSH login.\n";
      if (!values.contains("--server")) config.server = setupPrompt("Git server (user@host or user@SSH-alias)", config.server, true);
      if (!values.contains("--client-id")) config.client_id = setupPrompt("Device ID (must match the paired server key)", config.client_id, true);
      if (!values.contains("--display-name")) config.display_name = setupPrompt("Device display name", config.display_name, true);
      if (!values.contains("--remote-name")) config.remote_name = setupPrompt("Git remote name", config.remote_name, true);
      if (!values.contains("--web-host")) config.web_host = setupPrompt("Dashboard SSH login (SSH alias or user@host)",
          config.web_host.empty() ? config.server.substr(config.server.find('@') + 1) : config.web_host, true);
      if (!values.contains("--remote-port")) config.web_port = setupPort(setupPrompt("Dashboard port on server", std::to_string(config.web_port), true), "dashboard port");
      if (!values.contains("--public-path-mode")) {
        const auto mode = setupPrompt("Report checkout paths as basename or full", config.expose_full_paths ? "full" : "basename", true);
        if (mode != "basename" && mode != "full") throw std::invalid_argument("path privacy must be basename or full");
        config.expose_full_paths = mode == "full";
      }
      if (roots.empty() && config.scan_roots.empty()) {
        const auto root = setupPrompt("Folder to discover additional projects (optional)", "");
        if (!root.empty()) config.scan_roots.push_back(setupPath(root));
      }
    }
    const auto rendered = ckgit::renderClientConfig(config);
    std::cout << (exists ? "Replace configuration: " : "Create configuration: ") << path.string()
              << "\nPrivate file permissions: 0600\n\n" << rendered
              << "\nOrdinary sync uploads committed local branches and tags. Path privacy does not limit local management.\n";
    if (dry_run || (!yes && !guided)) {
      std::cout << "Preview only; no files changed. Run again with --yes to save, or use an interactive terminal.\n";
      return 0;
    }
    if (!yes) {
      const auto answer = setupPrompt(exists ? "Replace this configuration? [y/N]" : "Save this configuration? [y/N]", "");
      if (answer != "y" && answer != "Y" && answer != "yes" && answer != "YES") {
        std::cout << "Cancelled; configuration unchanged.\n";
        return 0;
      }
    }
    ckgit::saveClientConfig(path, config, overwrite);
    std::cout << "Saved " << path.string() << ".\n"
              << "Next: ckgit doctor --config " << setupQuote(path.string()) << "\n"
              << "Your server administrator must pair this device's public SSH key with client ID " << config.client_id << ".\n"
              << "If using a dedicated key, set IdentityFile for the server in ~/.ssh/config and load a passphrase-protected key into your SSH agent.\n";
    return 0;
  } catch (const std::invalid_argument& failure) {
    std::cerr << "ckgit: " << failure.what() << "\nTry 'ckgit help setup'.\n";
    return kUsage;
  }
}

class DoctorSignals {
 public:
  DoctorSignals() {
    tunnel_stop_requested = 0;
    struct sigaction action{};
    sigemptyset(&action.sa_mask);
    action.sa_handler = requestTunnelStop;
    sigaction(SIGINT, &action, &interrupt_); sigaction(SIGTERM, &action, &terminate_);
    sigaction(SIGHUP, &action, &hangup_);
    action.sa_handler = SIG_IGN; sigaction(SIGPIPE, &action, &pipe_);
  }
  ~DoctorSignals() {
    sigaction(SIGINT, &interrupt_, nullptr); sigaction(SIGTERM, &terminate_, nullptr);
    sigaction(SIGHUP, &hangup_, nullptr); sigaction(SIGPIPE, &pipe_, nullptr);
  }
 private:
  struct sigaction interrupt_{}, terminate_{}, hangup_{}, pipe_{};
};

int spawnDoctorTunnel(const std::vector<std::string>& command) {
  std::vector<char*> argv;
  for (const auto& argument : command) argv.push_back(const_cast<char*>(argument.c_str()));
  argv.push_back(nullptr);
  const auto child = fork();
  if (child < 0) throw std::runtime_error("could not start dashboard SSH check");
  if (child == 0) {
    if (setsid() < 0) _exit(127);
    const int input = open("/dev/null", O_RDONLY);
    if (input < 0 || dup2(input, STDIN_FILENO) < 0) _exit(127);
    if (input != STDIN_FILENO) close(input);
    execvp(argv.front(), argv.data());
    _exit(127);
  }
  return child;
}

class DoctorTunnel {
 public:
  explicit DoctorTunnel(int pid) : pid_(pid) {}
  ~DoctorTunnel() {
    if (reaped_) { kill(-pid_, SIGKILL); return; }
    kill(-pid_, SIGTERM); kill(pid_, SIGTERM);
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
    while (std::chrono::steady_clock::now() < deadline) {
      if (childHasExited(pid_, &status_)) { kill(-pid_, SIGKILL); return; }
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    kill(-pid_, SIGKILL); kill(pid_, SIGKILL);
    while (waitpid(pid_, &status_, 0) < 0 && errno == EINTR) {}
  }
  bool exited() {
    if (!reaped_) reaped_ = childHasExited(pid_, &status_);
    return reaped_;
  }
  int status() const { return childExitCode(status_); }
 private:
  int pid_, status_{0};
  bool reaped_{false};
};

// Keep the HTTP exchange within the same wall-clock deadline as SSH startup.
// Per-read socket timeouts alone can be extended indefinitely by a slow peer.
std::optional<std::string> probeDoctorDashboard(unsigned short port,
    std::chrono::steady_clock::time_point deadline) {
  const auto remaining = [&]() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now()).count();
  };
  if (tunnel_stop_requested || remaining() <= 0) return std::nullopt;
  const auto waitMilliseconds = [&]() {
    return static_cast<int>(std::clamp<std::chrono::milliseconds::rep>(remaining(), 1, 100));
  };
  const int descriptor = connectLoopback(port, waitMilliseconds());
  if (descriptor < 0) return std::nullopt;
  struct Connection {
    int descriptor;
    ~Connection() { close(descriptor); }
  } connection{descriptor};
  const int flags = fcntl(descriptor, F_GETFL, 0);
  if (flags < 0 || fcntl(descriptor, F_SETFL, flags | O_NONBLOCK) < 0) return std::nullopt;
  const auto waitFor = [&](short event) {
    while (!tunnel_stop_requested && remaining() > 0) {
      pollfd ready{descriptor, event, 0};
      const auto result = poll(&ready, 1, waitMilliseconds());
      if (result > 0) return (ready.revents & (event | POLLHUP)) != 0;
      if (result < 0 && errno != EINTR) return false;
    }
    return false;
  };
  const std::string request = "HEAD / HTTP/1.1\r\nHost: 127.0.0.1\r\nConnection: close\r\n\r\n";
  std::size_t sent = 0;
  while (sent < request.size()) {
    if (!waitFor(POLLOUT)) return std::nullopt;
    const auto count = send(descriptor, request.data() + sent, request.size() - sent, 0);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (count <= 0) return std::nullopt;
    sent += static_cast<std::size_t>(count);
  }
  std::string response;
  char buffer[256];
  while (response.size() < 1024 && waitFor(POLLIN)) {
    const auto count = recv(descriptor, buffer, sizeof(buffer), 0);
    if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) continue;
    if (count <= 0) return std::nullopt;
    response.append(buffer, static_cast<std::size_t>(count));
    const auto end = response.find("\r\n");
    if (end != std::string::npos) return response.substr(0, end);
  }
  return std::nullopt;
}

int doctorCommand(const std::vector<std::string>& arguments) {
  std::optional<std::filesystem::path> requested;
  std::optional<std::string> web_host;
  std::optional<unsigned short> web_port;
  unsigned int seconds = 10;
  try {
    for (std::size_t index = 0; index < arguments.size(); ++index) {
      const auto& option = arguments[index];
      if (index + 1 == arguments.size()) throw std::invalid_argument(option + " requires a value");
      const auto value = arguments[++index];
      if (option == "--config") requested = value;
      else if (option == "--web-host") {
        if (!isValidSshTarget(value)) throw std::invalid_argument("--web-host requires an SSH alias or user@host");
        web_host = value;
      } else if (option == "--remote-port") web_port = setupPort(value, option);
      else if (option == "--timeout") {
        const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), seconds);
        if (error != std::errc{} || end != value.data() + value.size() || seconds < 1 || seconds > 60)
          throw std::invalid_argument("--timeout must be between 1 and 60 seconds");
      } else throw std::invalid_argument("unknown doctor option: " + option);
    }
  } catch (const std::invalid_argument& failure) {
    std::cerr << "ckgit: " << failure.what() << "\nTry 'ckgit help doctor'.\n";
    return kUsage;
  }
  bool failed = false;
  const auto report = [&](bool success, const std::string& check, const std::string& detail,
                          const std::string& recovery = "") {
    std::cout << (success ? "OK   " : "FAIL ") << check << ": " << detail << "\n";
    if (!success) {
      failed = true;
      if (!recovery.empty()) std::cout << "     Next: " << recovery << "\n";
    }
  };
  std::cout << "ckgit doctor · build " << ckgit::buildVersion() << "\n";
  std::optional<ckgit::ClientConfig> config;
  try {
    const auto path = requested ? *requested : defaultClientConfigPath();
    config = ckgit::loadClientConfig(path);
    report(true, "Configuration", path.string() + " · " + config->display_name + " (" + config->client_id + ") → " + config->server);
  } catch (const std::exception& failure) {
    report(false, "Configuration", failure.what(), "run ckgit setup, or repair the displayed configuration file");
  }
  try {
    const auto git = ckgit::runProcess({"git", "--version"}, std::chrono::seconds(seconds), 4096);
    report(git.exit_code == 0 && !git.timed_out && !git.output_truncated && git.output.starts_with("git version "),
           "Git", git.exit_code == 0 ? git.output.substr(0, git.output.find('\n')) : describeProcessFailure(git, std::chrono::seconds(seconds)),
           "install Git and ensure git is available on PATH");
  } catch (const std::exception& failure) {
    report(false, "Git", failure.what(), "install Git and ensure git is available on PATH");
  }
  const std::vector<std::string> ssh_options{"ssh", "-o", "BatchMode=yes", "-o", "StrictHostKeyChecking=yes",
      "-o", "UpdateHostKeys=no", "-o", "ConnectTimeout=" + std::to_string(seconds),
      "-o", "ConnectionAttempts=1", "-o", "RequestTTY=no", "-o", "ControlMaster=no", "-o", "ControlPath=none",
      "-o", "PermitLocalCommand=no"};
  if (config) {
    try {
      auto command = ssh_options;
      command.insert(command.end(), {"-o", "ClearAllForwardings=yes", config->server, "ckgit-rpc 1 ping"});
      const auto response = ckgit::runProcess(command, std::chrono::seconds(seconds), 8192);
      const bool connected = response.exit_code == 0 && !response.timed_out && !response.output_truncated && response.output == "ok\n";
      std::string detail = connected ? "restricted SSH control answered on " + config->server
                                      : describeProcessFailure(response, std::chrono::seconds(seconds));
      if (connected) {
        // Best-effort: an older paired server may not recognize this operation
        // yet, so its absence never turns a working connection into a failure.
        try {
          auto version_command = ssh_options;
          version_command.insert(version_command.end(), {"-o", "ClearAllForwardings=yes", config->server, "ckgit-rpc 1 version"});
          const auto version_response = ckgit::runProcess(version_command, std::chrono::seconds(seconds), 8192);
          if (version_response.exit_code == 0 && !version_response.timed_out && !version_response.output_truncated &&
              version_response.output.starts_with("ok ") && version_response.output.ends_with("\n")) {
            const auto server_version = version_response.output.substr(3, version_response.output.size() - 4);
            if (!server_version.empty()) detail += " (server " + server_version + ")";
          }
        } catch (const std::exception&) { /* Version reporting is optional. */ }
      }
      report(connected, "Git server", detail,
          "verify the host key through a trusted channel, pair this device's public key, and configure IdentityFile/ssh-agent for " + config->server);
    } catch (const std::exception& failure) {
      report(false, "Git server", failure.what(), "check SSH installation, network access, and the paired device key");
    }
  } else {
    report(false, "Git server", "not checked because configuration is invalid", "complete setup before checking restricted SSH access");
  }
  const std::string target = web_host.value_or(config ? (config->web_host.empty() ? config->server.substr(config->server.find('@') + 1) : config->web_host) : "");
  if (target.empty()) {
    report(false, "Dashboard", "no forwarding-capable SSH host is configured", "set web_host with setup or pass doctor --web-host user@host");
  } else {
    try {
      const DoctorSignals signals;
      const auto local = pickLocalPort(std::nullopt);
      const auto remote = web_port.value_or(config ? config->web_port : kDefaultDashboardPort);
      auto command = ssh_options;
      command.insert(command.end(), {"-N", "-o", "ExitOnForwardFailure=yes", "-o", "ServerAliveInterval=5",
          "-o", "ServerAliveCountMax=1", "-L", "127.0.0.1:" + std::to_string(local) + ":127.0.0.1:" + std::to_string(remote), target});
      DoctorTunnel tunnel(spawnDoctorTunnel(command));
      const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
      bool ready = false;
      while (!tunnel_stop_requested && std::chrono::steady_clock::now() < deadline && !tunnel.exited()) {
        const int connection = connectLoopback(local, 100);
        if (connection >= 0) { close(connection); ready = true; break; }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      const auto http = ready && !tunnel_stop_requested ? probeDoctorDashboard(local, deadline) : std::nullopt;
      const bool success = http && !tunnel_stop_requested && !tunnel.exited() &&
          (http->starts_with("HTTP/1.1 200 ") || http->starts_with("HTTP/1.0 200 "));
      const auto detail = success ? "HTTP answered through " + target + " on server port " + std::to_string(remote) :
          tunnel_stop_requested ? std::string("check interrupted") : http ? "dashboard returned " + *http :
          tunnel.exited() ? "SSH forwarding exited with code " + std::to_string(tunnel.status()) : "SSH tunnel or dashboard did not answer within the check deadline";
      report(success, "Dashboard", detail,
          "use an ordinary SSH login with forwarding permission (not the restricted Git account), enable http_port on the server, and check --remote-port");
    } catch (const std::exception& failure) {
      report(false, "Dashboard", failure.what(), "check web_host, SSH forwarding permission, and the server dashboard port");
    }
  }
  std::cout << (failed ? "Some checks need attention. No configuration or server data was changed.\n"
                       : "All checks passed. Git access and dashboard forwarding both work.\n");
  return failed ? 1 : 0;
}
