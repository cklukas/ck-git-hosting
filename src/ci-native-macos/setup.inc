// Included by main.cpp: operator-only native service installation.
std::string xml(const std::string& text) {
  std::string out;
  for (char c : text) {
    if (c == '&') out += "&amp;"; else if (c == '<') out += "&lt;";
    else if (c == '>') out += "&gt;"; else if (c == '"') out += "&quot;"; else out += c;
  }
  return out;
}
void command(const std::vector<std::string>& args) {
  const auto r = ckgit::runProcess(args,std::chrono::seconds(30));
  if (r.exit_code != 0) throw std::runtime_error(args.front()+": "+r.output);
}
fs::path executablePath() {
  char path[PROC_PIDPATHINFO_MAXSIZE];
  check(::proc_pidpath(::getpid(),path,sizeof(path)) > 0, "cannot locate native helper"); return fs::canonical(path);
}
void installExecutable(const fs::path& source, const fs::path& target) {
  check(fs::is_regular_file(source), "native installation executable missing");
  check(!fs::exists(target), "native executable already installed; refusing uncoordinated replacement");
  check(fs::copy_file(source,target), "cannot install native executable");
  check(::chown(target.c_str(),0,0) == 0 && ::chmod(target.c_str(),0755) == 0, "cannot secure native executable");
}
int setup(int argc, char** argv) {
  fs::path config_path, work;
  std::string broker_name;
  std::vector<fs::path> tools;
  bool dry = false;
  for (int i=2; i<argc; ++i) {
    const std::string option = argv[i];
    if (option == "--dry-run") { dry = true; continue; }
    check(i+1 < argc, "native setup option requires a value");
    const std::string value = argv[++i];
    if (option == "--config") config_path = value;
    else if (option == "--broker-user") broker_name = value;
    else if (option == "--work-root") work = value;
    else if (option == "--toolchain") tools.emplace_back(value);
    else throw std::runtime_error("unknown native setup option: "+option);
  }
  check(config_path.is_absolute() && work.is_absolute() && !broker_name.empty(), "setup requires absolute --config, --work-root and --broker-user");
  auto runner = ckgit::loadRunnerConfig(config_path);
  check(runner.name.size() <= 32 && identifier(runner.name), "native runner name must be a simple identifier, at most 32 characters");
  const auto* broker = ::getpwnam(broker_name.c_str());
  check(broker && broker->pw_uid > 0 && broker->pw_uid <= 0x7fffffff && broker->pw_gid > 0 && broker->pw_gid <= 0x7fffffff,
        "broker account must exist with an unprivileged UID and non-root primary group");
  const uid_t broker_uid = broker->pw_uid; const gid_t broker_gid = broker->pw_gid;
  check(runner.identity && runner.known_hosts && fs::is_regular_file(*runner.identity) && fs::is_regular_file(*runner.known_hosts),
        "native setup requires an existing runner identity and pinned known_hosts file");
  struct stat key{};
  check(::lstat(runner.identity->c_str(), &key) == 0 && S_ISREG(key.st_mode) && key.st_uid == broker_uid && !(key.st_mode & 0077),
        "runner key must be private and owned by the broker account");
  check(!runner.cache_root, "native runner does not support shared caches");
  // Require an explicitly selected scratch volume. Never silently use the system disk.
  fs::path volume;
  for (const fs::path& candidate : {fs::path("/Volumes/S8T"),fs::path("/Volumes/PRO-BLADE")}) {
    const auto prefix = (candidate / "tmp").string()+"/";
    if (work.string().starts_with(prefix)) volume = candidate;
  }
  check(!volume.empty() && fs::canonical(volume) == volume && fs::exists(volume), "native work root must be on mounted S8T/tmp or PRO-BLADE/tmp");
  struct stat volume_stat{}, parent_stat{};
  check(::stat(volume.c_str(),&volume_stat) == 0 && ::stat(volume.parent_path().c_str(),&parent_stat) == 0 &&
        volume_stat.st_dev != parent_stat.st_dev, "selected native scratch volume is not mounted");
  check(work.lexically_normal() == work && fs::canonical(work.parent_path()) == work.parent_path(), "native work parent must exist and be canonical");
  check(!fs::exists(work.string()+".ckgit-owner"), "native work root is reserved by another installation");
  for (auto parent = work.parent_path(); !parent.empty();) {
    check(!fs::exists(parent.string()+".ckgit-owner"), "native work root cannot nest inside another installation");
    struct stat info{};
    check(::stat(parent.c_str(),&info)==0 && (info.st_mode & 0001) && !(info.st_mode & 0002),
          "native work ancestors must be traversable and not world-writable");
    if (parent == parent.parent_path()) break; parent = parent.parent_path();
  }
  check(!fs::exists(work) || (fs::is_directory(fs::symlink_status(work)) && fs::is_empty(work)), "native work root must be a new or empty directory");
  for (auto& tool : tools) {
    check(tool.is_absolute() && fs::canonical(tool) == tool && fs::is_directory(tool), "toolchain must be a canonical directory");
    const auto value = tool.string();
    check(value.starts_with("/Applications/") || value.starts_with("/Library/Developer/") || value.starts_with("/opt/"),
          "approved toolchain roots must be below /Applications, /Library/Developer or /opt");
  }
  const std::string name = runner.name, account = "_ckgit_"+name, label = "org.ckgit.native."+name;
  const fs::path state = fs::path("/Library/Application Support/ckgit-native") / name;
  const fs::path socket = "/var/run/ckgit-native-"+name+".sock";
  const fs::path helper_dir = fs::path("/Library/PrivilegedHelperTools") / label;
  const fs::path helper = helper_dir / "ckgit-macos-native", daemon = helper_dir / "ck-ci-runnerd";
  const fs::path service = fs::path("/Library/LaunchDaemons") / (label+".plist");
  const fs::path broker_service = fs::path("/Library/LaunchDaemons") / ("org.ckgit.runner."+name+".plist");
  check(!fs::exists(state) && !fs::exists(service) && !fs::exists(broker_service) && !fs::exists(helper_dir),
        "native setup already exists; inspect the existing installation rather than replacing an active slot");
  check(::getpwnam(account.c_str()) == nullptr && ::getgrnam(account.c_str()) == nullptr,
        "native build account already exists; refusing to adopt an unrelated identity");
  unsigned id = 61000;
  while (id < 62000 && (::getpwuid(id) || ::getgrgid(id))) ++id;
  check(id < 62000, "no unused native build identity available");
  const auto source = executablePath();
  check(fs::is_regular_file(source.parent_path()/"ck-ci-runnerd"), "build/install ck-ci-runnerd beside native helper before setup");
  std::cout << "Native setup for " << name << ":\n  broker " << broker_name << " (UID " << broker_uid
            << ")\n  exclusive build account " << account << " (UID/GID " << id << ")\n  work " << work
            << "\n  supervisor " << service << "\n  broker " << broker_service
            << "\n  network denied; shared caches disabled; no VM\n";
  if (dry) { std::cout << "Preview only; no files, accounts or services changed.\n"; return 0; }
  check(::geteuid() == 0, "setup needs administrator privileges; rerun this exact command with sudo");
  // Installed state is persistent; intermediate compiler/test data remain on the explicit scratch volume.
  fs::create_directories(state); ::chmod(state.c_str(),0700); rootFile(state,true);
  fs::create_directories(helper_dir); ::chmod(helper_dir.c_str(),0755); rootFile(helper_dir,true);
  writeNew(work.string()+".ckgit-owner",state.string());
  fs::create_directory(work); check(::chown(work.c_str(),broker_uid,broker_gid) == 0 && ::chmod(work.c_str(),0711) == 0, "cannot secure native build root");
  const std::string user = "/Users/"+account, group = "/Groups/"+account;
  const auto ds = [&](const std::vector<std::string>& fields) {
    std::vector<std::string> args{"/usr/bin/dscl",".","-create"}; args.insert(args.end(),fields.begin(),fields.end()); command(args);
  };
  ds({group}); ds({group,"PrimaryGroupID",std::to_string(id)}); ds({group,"Password","*"});
  ds({user}); ds({user,"UniqueID",std::to_string(id)}); ds({user,"PrimaryGroupID",std::to_string(id)});
  ds({user,"UserShell","/usr/bin/false"}); ds({user,"NFSHomeDirectory","/var/empty"}); ds({user,"Password","*"});
  ds({user,"RealName","ckgit exclusive build slot "+name}); ds({user,"IsHidden","1"});
  installExecutable(source,helper); installExecutable(source.parent_path()/"ck-ci-runnerd",daemon);
  std::vector<std::string> fields{"ckgit-native-v1",std::to_string(broker_uid),std::to_string(broker_gid),
      std::to_string(id),std::to_string(id),account,work.string(),socket.string(),"offline"};
  for (const auto& tool : tools) fields.push_back(tool.string());
  writeNew(state / "config",encode(fields));
  // Keep the previous configuration as durable rollback evidence, with credentials still private.
  writeNew(state / "runner-before.ini",readFile(config_path,65536));
  runner.build_root = work; runner.allow_network = false; runner.allow_degraded = false;
  runner.macos_native_socket = socket;
  const fs::path installed_config = state / "runner.ini";
  writeNew(installed_config,ckgit::renderRunnerConfig(runner),0644);
  // Broker needs traversal to its public configuration, never access to root-only journal/config.
  ::chmod(state.c_str(),0711);
  const auto plist = [&](const std::string& service_label, const std::vector<std::string>& args, bool is_broker) {
    std::string out = "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">\n<plist version=\"1.0\"><dict><key>Label</key><string>"+xml(service_label)+"</string><key>ProgramArguments</key><array>";
    for (const auto& arg : args) out += "<string>"+xml(arg)+"</string>";
    out += "</array><key>RunAtLoad</key><true/><key>KeepAlive</key><true/><key>ThrottleInterval</key><integer>10</integer><key>ExitTimeOut</key><integer>30</integer><key>Umask</key><integer>63</integer>";
    if (is_broker) out += "<key>UserName</key><string>"+xml(broker_name)+"</string>";
    out += "<key>EnvironmentVariables</key><dict><key>TMPDIR</key><string>"+xml((work.parent_path()/("ckgit-service-"+name)).string())+"</string></dict></dict></plist>\n";
    return out;
  };
  const auto service_tmp = work.parent_path()/("ckgit-service-"+name);
  check(!fs::exists(service_tmp), "service temporary directory already exists");
  fs::create_directory(service_tmp); ::chown(service_tmp.c_str(),broker_uid,broker_gid); ::chmod(service_tmp.c_str(),0700);
  writeNew(service,plist(label,{helper.string(),"serve",state.string()},false),0644);
  writeNew(broker_service,plist("org.ckgit.runner."+name,{daemon.string(),"serve","--remote","--config",installed_config.string()},true),0644);
  command({"/bin/launchctl","bootstrap","system",service.string()});
  qualify(configuration(state));
  std::cout << "Supervisor installed and started. Runner configuration: " << installed_config << "\n"
            << "Broker launchd file prepared but NOT started. Run native qualification before enabling claims.\n"
            << "After qualification: sudo launchctl bootstrap system " << broker_service << "\n";
  return 0;
}
