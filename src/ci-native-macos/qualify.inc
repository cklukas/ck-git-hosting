// Qualification runs the installed supervisor as the broker; only account and
// service recovery assertions remain in the privileged driver.
std::string qualificationIdentity(const Config& c) {
  char version[256]; std::size_t size = sizeof(version);
  check(::sysctlbyname("kern.osversion",version,&size,nullptr,0) == 0, "cannot identify macOS build");
  return ckgit::sha256Hex(readFile(c.state/"config",65536)+ckgit::sha256HexOfFile(executablePath())+std::string(version));
}
void assertQualified(const Config& c) {
  rootFile(c.state/"qualified",false);
  check(readFile(c.state/"qualified",128) == qualificationIdentity(c), "native setup needs qualification for this helper/config/macOS build");
}
int jobProbe(int argc, char** argv) {
  check(argc >= 3, "missing job probe mode");
  ::alarm(30);
  const std::string mode = argv[2];
  if (mode == "orphan") {
    check(argc == 4, "missing orphan handshake");
    const int ready = static_cast<int>(number(argv[3],3,1048576));
    std::ofstream("orphan.pid") << ::getpid() << '\n';
    char ok = '1'; check(::write(ready,&ok,1) == 1, "orphan handshake failed"); ::close(ready);
    ::close(0); ::close(1); ::close(2); ::signal(SIGTERM,SIG_IGN);
    for (;;) ::pause();
  }
  if (mode == "detach" || mode == "disconnect") {
    int gate[2]; check(::pipe(gate) == 0, "probe pipe failed");
    const pid_t child = ::fork(); check(child >= 0, "probe fork failed");
    if (!child) {
      ::close(gate[0]); check(::setsid() >= 0, "probe setsid failed");
      const pid_t grandchild = ::fork(); check(grandchild >= 0, "probe double fork failed");
      if (grandchild) _exit(0);
      const auto fd = std::to_string(gate[1]);
      ::execl(argv[0],argv[0],"job-probe","orphan",fd.c_str(),nullptr); _exit(127);
    }
    ::close(gate[1]); char ready; check(::read(gate[0],&ready,1) == 1, "detached worker failed"); ::close(gate[0]);
    int status; ::waitpid(child,&status,0);
    if (mode == "disconnect") for (;;) ::pause();
    return 0;
  }
  if (mode == "linger" || mode == "immediate" || mode == "closed-output" || mode == "stopped") {
    std::ofstream("worker.pid") << ::getpid() << '\n';
    ::signal(SIGTERM,SIG_IGN);
    if (mode == "closed-output") { ::close(1); ::close(2); }
    if (mode == "stopped") ::raise(SIGSTOP);
    for (;;) ::pause();
  }
  if (mode == "handoff") {
    // Every generation exits as soon as its replacement exists. Cleanup must
    // not infer emptiness from an old PID disappearing after enumeration.
    for (int i=0; i<64; ++i) {
      const auto pid = ::fork(); check(pid >= 0, "handoff fork failed");
      if (pid) _exit(0);
      ::alarm(30);
      ::usleep(1000);
    }
    for (;;) ::pause();
  }
  if (mode == "burst") {
    for (int i=0; i<32; ++i) {
      const auto pid = ::fork(); check(pid >= 0, "bounded fork burst failed");
      if (!pid) { ::setsid(); ::alarm(20); for (;;) ::pause(); }
    }
    return 0;
  }
  if (mode == "restrictions") {
    check(argc == 5, "missing restriction probe arguments");
    check(::getenv("CKGIT_MACOS_SANDBOX") == nullptr, "unqualified probe received a forged backend marker");
    check(::getuid() == number(argv[3],1,0x7fffffff) && ::geteuid() == ::getuid(), "wrong job identity");
    check(::setuid(0) == -1 && ::setgid(0) == -1, "job regained privileged identity");
    errno = 0; Fd outside(::open(argv[4],O_RDONLY));
    check(outside.value < 0 && (errno == EPERM || errno == EACCES), "outside read not denied");
    errno = 0; Fd write_outside(::open(argv[4],O_WRONLY));
    check(write_outside.value < 0 && (errno == EPERM || errno == EACCES), "outside write not denied");
    check(::symlink(argv[4], "outside-link") == 0, "cannot create symlink restriction fixture");
    errno = 0; Fd through_link(::open("outside-link",O_RDONLY));
    check(through_link.value < 0 && (errno == EPERM || errno == EACCES), "symlink escaped file policy");
    errno = 0;
    check(::kill(::getppid(),SIGCONT) == -1 && errno == EPERM, "signal to non-child process not denied");
    const pid_t signal_child = ::fork(); check(signal_child >= 0, "child signal probe fork failed");
    if (!signal_child) { ::alarm(10); for (;;) ::pause(); }
    check(::kill(signal_child,SIGTERM) == 0, "signal to direct child denied");
    int signal_status = 0;
    check(::waitpid(signal_child,&signal_status,0) == signal_child && WIFSIGNALED(signal_status), "child signal not delivered");
    Fd network(::socket(AF_INET,SOCK_STREAM,0));
    if (network.value >= 0) {
      sockaddr_in target{}; target.sin_family=AF_INET; target.sin_port=htons(9); target.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
      errno=0; check(::connect(network.value,reinterpret_cast<sockaddr*>(&target),sizeof(target)) == -1 &&
                    (errno == EPERM || errno == EACCES), "network restriction not enforced");
    } else check(errno == EPERM || errno == EACCES, "unexpected network failure");
    const auto pid = ::fork(); check(pid >= 0, "set-ID probe fork failed");
    if (!pid) { ::execl("/usr/bin/su","su","root","-c","true",nullptr); _exit(errno == EPERM || errno == EACCES ? 0 : 1); }
    int status; check(::waitpid(pid,&status,0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0,"set-ID executable not denied");
    std::cout << "identity, external files, network and set-ID execution denied\n";
    return 0;
  }
  throw std::runtime_error("unknown job probe mode");
}
void brokerQualification(const Config& c, const fs::path& probe, const fs::path& sentinel) {
  const auto startup_deadline = std::chrono::steady_clock::now()+std::chrono::seconds(15);
  for (;;) {
    Fd ready(::socket(AF_UNIX,SOCK_STREAM,0)); check(ready.value >= 0,"qualification startup socket failed");
    const auto addr = address(c.socket.string());
    if (::connect(ready.value,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr)) == 0) break;
    check(std::chrono::steady_clock::now() < startup_deadline,"supervisor did not start");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  const auto make = [&](const std::string& name) {
    const auto workspace = c.work/name;
    check(!fs::exists(workspace), "qualification workspace already exists");
    fs::create_directories(workspace/"src"); fs::create_directory(workspace/"home"); fs::create_directory(workspace/"tmp");
    fs::copy_file(probe,workspace/"src/probe"); ::chmod((workspace/"src/probe").c_str(),0755);
    return workspace;
  };
  for (const std::string mode : {"restrictions","detach","burst","handoff","immediate","linger","closed-output","stopped"}) {
    const auto workspace = make("qualification-"+mode);
    const auto start = std::chrono::steady_clock::now();
    std::string log;
    const auto result = ckgit::runMacNative(c.socket,workspace,
        {(workspace/"src/probe").string(),"job-probe",mode,std::to_string(c.job),sentinel.string()},
        {"PATH=/usr/bin:/bin","CKGIT_MACOS_SANDBOX=forged"},mode == "closed-output" ? 1 : 10,65536,
        [&](std::string_view text) { log += text; },
        [&] { return mode == "immediate" || ((mode == "linger" || mode == "stopped") && std::chrono::steady_clock::now()-start > std::chrono::milliseconds(300)); },{});
    if (mode == "closed-output") check(result.timed_out,"closed-output timeout did not fire");
    else if (mode == "immediate" || mode == "linger" || mode == "stopped") check(result.cancelled,"cancellation did not fire");
    else if (result.exit_code != 0) throw std::runtime_error("qualification "+mode+": "+log);
    // R was authenticated and follows drain/recovery; broker must now own all output.
    struct stat st{}; check(::stat((workspace/"src").c_str(),&st) == 0 && st.st_uid == ::getuid(),"workspace not returned to broker");
    fs::remove_all(workspace);
    std::cout << "PASS " << mode << '\n';
  }
  const auto workspace = make("qualification-disconnect");
  Fd fd(::socket(AF_UNIX,SOCK_STREAM,0)); check(fd.value >= 0,"disconnect socket failed"); noSigpipe(fd.value);
  const auto addr = address(c.socket.string());
  check(::connect(fd.value,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr)) == 0,"disconnect connect failed");
  send(fd.value,encode({"run",workspace.filename().string(),"10","65536","3",(workspace/"src/probe").string(),"job-probe","disconnect","PATH=/usr/bin:/bin"}));
  // Ownership is transferred, so wait on public output instead of inspecting its private files.
  std::this_thread::sleep_for(std::chrono::milliseconds(700));
  ::close(fd.value); fd.value=-1;
  // An authenticated probe response is queued behind execution and its UID drain.
  Fd health(::socket(AF_UNIX,SOCK_STREAM,0)); check(health.value >= 0,"health socket failed"); noSigpipe(health.value);
  check(::connect(health.value,reinterpret_cast<const sockaddr*>(&addr),sizeof(addr)) == 0,"health connect failed");
  send(health.value,encode({"probe",c.work.string()})); check(receive(health.value)=="ready-v1","disconnect recovery failed");
  fs::remove_all(workspace); std::cout << "PASS disconnect\n";
}
void qualifyRestart(const Config& c) {
  const auto workspace = c.work/"qualification-restart";
  check(!fs::exists(workspace), "restart qualification workspace already exists");
  const auto probe = executablePath();
  const pid_t broker = ::fork(); check(broker >= 0,"restart broker fork failed");
  if (!broker) {
    try {
      drop(c.broker,c.broker_group);
      fs::create_directories(workspace/"src"); fs::create_directory(workspace/"tmp"); fs::create_directory(workspace/"home");
      fs::copy_file(probe,workspace/"src/probe"); ::chmod((workspace/"src/probe").c_str(),0755);
      try {
        ckgit::runMacNative(c.socket,workspace,{(workspace/"src/probe").string(),"job-probe","disconnect"},
                           {"PATH=/usr/bin:/bin"},20,65536,{}, {}, {});
      } catch (...) { _exit(0); } // Losing the killed supervisor is expected.
      _exit(1);
    } catch (...) { _exit(2); }
  }
  const auto started = std::chrono::steady_clock::now();
  while (!fs::exists(workspace/"src/orphan.pid")) {
    check(std::chrono::steady_clock::now()-started < std::chrono::seconds(15),"restart worker did not start");
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
  }
  check(!members(c.job).empty(),"restart probe had no live job processes");
  command({"/bin/launchctl","kill","SIGKILL","system/org.ckgit.native."+c.state.filename().string()});
  int status = 0; while (::waitpid(broker,&status,0) < 0 && errno == EINTR) {}
  check(WIFEXITED(status) && WEXITSTATUS(status)==0,"restart broker did not detect supervisor loss");
  const auto deadline = std::chrono::steady_clock::now()+std::chrono::seconds(30);
  while (fs::exists(c.state/"active") || !members(c.job).empty()) {
    check(std::chrono::steady_clock::now() < deadline,"launchd restart did not reconcile the native UID");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  struct stat st{};
  check(::stat(workspace.c_str(),&st)==0 && st.st_uid==c.broker,"restart did not restore workspace ownership");
  fs::remove_all(workspace);
  std::cout << "PASS supervisor SIGKILL and launchd restart recovery\n";
}
void qualify(const Config& c) {
  Fd broker_lock;
  const auto ready_deadline = std::chrono::steady_clock::now()+std::chrono::seconds(15);
  do {
    broker_lock.value = ::open((c.socket.string()+".broker-lock").c_str(),O_RDWR|O_NOFOLLOW|O_CLOEXEC);
    if (broker_lock.value >= 0) break;
    check(errno == ENOENT && std::chrono::steady_clock::now() < ready_deadline,"supervisor lock did not become available");
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  } while (true);
  struct stat lock_info{};
  check(::fstat(broker_lock.value,&lock_info)==0 && S_ISREG(lock_info.st_mode) && lock_info.st_uid==c.broker &&
        lock_info.st_nlink==1 && ::flock(broker_lock.value,LOCK_EX|LOCK_NB)==0,
        "stop the SSH broker before native qualification");
  // No broker may claim work while this gate is absent. Do not reuse a failed marker.
  ::unlink((c.state/"qualified").c_str());
  check(members(c.job).empty(), "qualification requires an idle native slot");
  const auto sentinel = c.work.parent_path()/("ckgit-qualification-sentinel-"+c.state.filename().string());
  writeNew(sentinel,"outside data",0644);
  const auto probe = executablePath();
  const pid_t child = ::fork(); check(child >= 0,"qualification broker fork failed");
  if (!child) {
    try { drop(c.broker,c.broker_group); brokerQualification(c,probe,sentinel); std::cout.flush(); _exit(0); }
    catch (const std::exception& e) { ::dprintf(2,"qualification: %s\n",e.what()); _exit(1); }
  }
  int status = 0; while (::waitpid(child,&status,0) < 0 && errno == EINTR) {}
  ::unlink(sentinel.c_str());
  check(WIFEXITED(status) && WEXITSTATUS(status) == 0,"native qualification failed; broker remains disabled");
  check(members(c.job).empty(),"native qualification left processes alive");
  qualifyRestart(c);
  writeNew(c.state/"qualified",qualificationIdentity(c));
  std::cout << "Native process/policy qualification passed for this executable, configuration and macOS build.\n";
}
