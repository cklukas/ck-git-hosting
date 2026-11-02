// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Apple Virtualization helper used by the experimental macOS VM runner.
#import <Foundation/Foundation.h>
#import <AppKit/AppKit.h>
#import <Virtualization/Virtualization.h>
#include <signal.h>
#include <string>
#include <fcntl.h>
#include <poll.h>
#include <charconv>
#include <cerrno>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string_view>
#include <sys/socket.h>
#include <sys/file.h>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>

static void fail(NSString* message) {
  fprintf(stderr, "ckgit-macos-vm: %s\n", message.UTF8String);
  exit(1);
}

// The directory descriptor is retained only for a disposable run. A marker
// is written exclusively after Virtualization reports that the VM is stopped.
static int stoppedMarkerDirectory = -1;
static void confirmedStop() {
  if (stoppedMarkerDirectory >= 0) {
    constexpr std::string_view value = "ckgit-vm-stopped-v1\n";
    const int fd = openat(stoppedMarkerDirectory, ".ckgit-vm-stopped",
                          O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (fd < 0) fail(@"cannot record confirmed VM shutdown");
    const bool ok = write(fd, value.data(), value.size()) == static_cast<ssize_t>(value.size()) && fsync(fd) == 0;
    close(fd);
    if (!ok || fsync(stoppedMarkerDirectory) != 0) fail(@"cannot persist confirmed VM shutdown");
  }
  exit(0);
}

static NSURL* item(NSURL* directory, NSString* name) {
  return [directory URLByAppendingPathComponent:name];
}

// Refuse a stale directory below /Volumes when the named external volume has
// been unmounted; otherwise setup or install could write to the system disk.
static void requireMountedVolume(NSString* path) {
  NSArray<NSString*>* parts = path.stringByStandardizingPath.pathComponents;
  if (parts.count < 3 || ![parts[0] isEqualToString:@"/"] || ![parts[1] isEqualToString:@"Volumes"]) return;
  NSString* expected = [@"/Volumes" stringByAppendingPathComponent:parts[2]];
  struct statfs mount {};
  if (statfs(path.fileSystemRepresentation, &mount) != 0 ||
      ![expected isEqualToString:[NSString stringWithUTF8String:mount.f_mntonname]])
    fail(@"the selected VM volume is not mounted at its expected path");
}

@interface VMDelegate : NSObject <VZVirtualMachineDelegate>
@property(nonatomic) BOOL startCompleted;
@end

@interface VMProbeDelegate : NSObject <VZVirtioSocketListenerDelegate>
@property(nonatomic) BOOL accepted;
@property(nonatomic, copy) NSString* resultPath;
@end
@implementation VMProbeDelegate
- (BOOL)listener:(VZVirtioSocketListener*)listener shouldAcceptNewConnection:(VZVirtioSocketConnection*)connection
    fromSocketDevice:(VZVirtioSocketDevice*)device {
  (void)listener;
  (void)device;
  if (self.accepted) return NO;
  self.accepted = YES;
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_UTILITY, 0), ^{
    // A result file is used by the offline qualification. Its request must
    // differ from the ordinary connectivity probe so that an older agent or
    // an agent started without --require-offline cannot qualify the VM.
    const std::string request = self.resultPath ? "ckgit-vsock-offline-v1\n" : "ckgit-vsock-probe-v1\n";
    constexpr char response[] = "ckgit-vsock-reply-v1\n";
    std::string received(request.size(), '\0');
    const int fd = connection.fileDescriptor;
    timeval timeout{5, 0};
    int noSigPipe = 1;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
    std::size_t offset = 0;
    while (offset < received.size()) {
      const auto count = read(fd, received.data() + offset, received.size() - offset);
      if (count <= 0) break;
      offset += static_cast<std::size_t>(count);
    }
    const bool offlineFailure = self.resultPath && received == "ckgit-vsock-network-v1\n";
    if (offset == received.size() && (received == request || offlineFailure) &&
        write(fd, response, sizeof(response) - 1) == sizeof(response) - 1) {
      bool recorded = true;
      if (self.resultPath) {
        const int result = open(self.resultPath.fileSystemRepresentation, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
        const char* message = offlineFailure ? "ckgit-vsock-network-v1\n" : "ckgit-vsock-offline-v1\n";
        const auto length = strlen(message);
        recorded = result >= 0 && write(result, message, length) == static_cast<ssize_t>(length) && fsync(result) == 0;
        if (result >= 0) close(result);
      }
      fprintf(stderr, "Apple Virtio socket host probe %s\n", !recorded ? "could not record result" :
              offlineFailure ? "reported an active interface or inspection error" : "passed");
    } else {
      fprintf(stderr, "Apple Virtio socket host probe failed\n");
    }
    [connection close];
  });
  return YES;
}
@end

// For CI mode the parent owns the runner lease and credentials. This bridge
// passes only the parent's explicitly supplied byte stream over Virtio; it
// does not parse messages, open host paths, or attach an IP network device.
@interface VMBridgeDelegate : NSObject <VZVirtioSocketListenerDelegate>
@property(nonatomic) BOOL accepted;
@end
@implementation VMBridgeDelegate
- (BOOL)listener:(VZVirtioSocketListener*)listener shouldAcceptNewConnection:(VZVirtioSocketConnection*)connection
    fromSocketDevice:(VZVirtioSocketDevice*)device {
  (void)listener;
  (void)device;
  if (self.accepted) return NO;
  self.accepted = YES;
  fprintf(stderr, "VM guest channel connected\n");
  const int socketFD = connection.fileDescriptor;
  int noSigPipe = 1;
  setsockopt(socketFD, SOL_SOCKET, SO_NOSIGPIPE, &noSigPipe, sizeof(noSigPipe));
  void (^pump)(int, int, bool) = ^(int input, int output, bool toSocket) {
    if (connection.fileDescriptor != socketFD) return;
    char buffer[16 * 1024];
    for (;;) {
      const auto count = read(input, buffer, sizeof(buffer));
      if (count < 0 && errno == EINTR) continue;
      if (count <= 0) break;
      ssize_t offset = 0;
      while (offset < count) {
        const auto sent = toSocket ? send(output, buffer + offset, static_cast<std::size_t>(count - offset), MSG_NOSIGNAL)
                                   : write(output, buffer + offset, static_cast<std::size_t>(count - offset));
        if (sent < 0 && errno == EINTR) continue;
        if (sent <= 0) { offset = -1; break; }
        offset += sent;
      }
      if (offset < 0) break;
    }
    // The VM cannot outlive its job's stream. Shutdown unblocks the opposite
    // pump without closing and potentially recycling its shared descriptor.
    // Both blocks retain the connection until they return.
    shutdown(socketFD, SHUT_RDWR);
    kill(getpid(), SIGTERM);
  };
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ pump(STDIN_FILENO, socketFD, true); });
  dispatch_async(dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^{ pump(socketFD, STDOUT_FILENO, false); });
  return YES;
}
@end

@interface VMWindowDelegate : NSObject <NSWindowDelegate>
@property(nonatomic, strong) VZVirtualMachine* vm;
@end
@implementation VMWindowDelegate
- (BOOL)windowShouldClose:(NSWindow*)sender {
  (void)sender;
  if (self.vm.state == VZVirtualMachineStateStopped) { [NSApp terminate:nil]; return YES; }
  if (self.vm.canStop) {
    [self.vm stopWithCompletionHandler:^(NSError* error) {
      if (error) fail(error.localizedDescription);
      [NSApp terminate:nil];
    }];
  }
  return NO;
}
@end

static void installVM(const char* imagePath, const char* basePath, unsigned diskGiB) {
  NSURL* imageURL = [NSURL fileURLWithFileSystemRepresentation:imagePath isDirectory:NO relativeToURL:nil];
  NSURL* baseURL = [NSURL fileURLWithFileSystemRepresentation:basePath isDirectory:YES relativeToURL:nil];
  if (!imageURL.path.isAbsolutePath || !baseURL.path.isAbsolutePath || diskGiB < 64 || diskGiB > 2048)
    fail(@"install requires absolute image/base paths and a disk size from 64 to 2048 GiB");
  NSString* parent = baseURL.URLByDeletingLastPathComponent.path;
  if (![parent.stringByStandardizingPath isEqualToString:parent.stringByResolvingSymlinksInPath])
    fail(@"the VM base parent must exist without symlink components");
  requireMountedVolume(parent);
  requireMountedVolume(imageURL.path);
  struct stat imageStat {};
  if (lstat(imageURL.fileSystemRepresentation, &imageStat) != 0 || !S_ISREG(imageStat.st_mode) || imageStat.st_size <= 0)
    fail(@"the Apple restore image must be a nonempty regular file");
  [VZMacOSRestoreImage loadFileURL:imageURL completionHandler:^(VZMacOSRestoreImage* image, NSError* loadError) {
    dispatch_async(dispatch_get_main_queue(), ^{
      if (loadError || !image || !image.mostFeaturefulSupportedConfiguration)
        fail(loadError.localizedDescription ?: @"restore image is not supported on this Mac");
      if (mkdir(baseURL.fileSystemRepresentation, 0700) != 0)
        fail(@"cannot create a new private VM base directory");
      VZMacOSConfigurationRequirements* requirements = image.mostFeaturefulSupportedConfiguration;
      VZMacMachineIdentifier* identifier = [[VZMacMachineIdentifier alloc] init];
      if (![identifier.dataRepresentation writeToURL:item(baseURL, @"MachineIdentifier") atomically:NO] ||
          ![requirements.hardwareModel.dataRepresentation writeToURL:item(baseURL, @"HardwareModel") atomically:NO])
        fail(@"cannot save VM hardware identity");
      NSError* error = nil;
      VZMacAuxiliaryStorage* auxiliary = [[VZMacAuxiliaryStorage alloc]
          initCreatingStorageAtURL:item(baseURL, @"AuxiliaryStorage") hardwareModel:requirements.hardwareModel options:0 error:&error];
      if (!auxiliary) fail(error.localizedDescription ?: @"cannot create auxiliary VM storage");
      NSURL* diskURL = item(baseURL, @"Disk.img");
      const int diskFD = open(diskURL.fileSystemRepresentation, O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
      if (diskFD < 0) fail(@"cannot create VM disk");
      const auto diskBytes = static_cast<off_t>(diskGiB) * 1024 * 1024 * 1024;
      if (ftruncate(diskFD, diskBytes) != 0 || fsync(diskFD) != 0) { close(diskFD); fail(@"cannot size VM disk"); }
      close(diskFD);
      VZDiskImageStorageDeviceAttachment* disk = [[VZDiskImageStorageDeviceAttachment alloc] initWithURL:diskURL readOnly:NO error:&error];
      if (!disk) fail(error.localizedDescription ?: @"cannot attach VM disk");
      VZMacPlatformConfiguration* platform = [[VZMacPlatformConfiguration alloc] init];
      platform.machineIdentifier = identifier;
      platform.hardwareModel = requirements.hardwareModel;
      platform.auxiliaryStorage = auxiliary;
      VZVirtualMachineConfiguration* config = [VZVirtualMachineConfiguration new];
      config.platform = platform;
      config.bootLoader = [[VZMacOSBootLoader alloc] init];
      config.CPUCount = MAX(requirements.minimumSupportedCPUCount, MIN(4, VZVirtualMachineConfiguration.maximumAllowedCPUCount));
      config.memorySize = MAX(requirements.minimumSupportedMemorySize, 8ull * 1024 * 1024 * 1024);
      config.storageDevices = @[[[VZVirtioBlockDeviceConfiguration alloc] initWithAttachment:disk]];
      config.entropyDevices = @[[[VZVirtioEntropyDeviceConfiguration alloc] init]];
      VZMacGraphicsDeviceConfiguration* graphics = [[VZMacGraphicsDeviceConfiguration alloc] init];
      graphics.displays = @[[[VZMacGraphicsDisplayConfiguration alloc] initWithWidthInPixels:1024 heightInPixels:768 pixelsPerInch:80]];
      config.graphicsDevices = @[graphics];
      if (![config validateWithError:&error]) fail(error.localizedDescription ?: @"invalid installation VM configuration");
      VZVirtualMachine* vm = [[VZVirtualMachine alloc] initWithConfiguration:config];
      VZMacOSInstaller* installer = [[VZMacOSInstaller alloc] initWithVirtualMachine:vm restoreImageURL:imageURL];
      puts("Installing macOS into the selected VM base; this may take a while.");
      fflush(stdout);
      [installer installWithCompletionHandler:^(NSError* installError) {
        (void)installer;
        if (installError) fail(installError.localizedDescription);
        puts("Apple macOS installation completed. Guest setup and build tools are still required.");
        fflush(stdout);
        exit(0);
      }];
    });
  }];
  dispatch_main();
}
@implementation VMDelegate
- (void)guestDidStopVirtualMachine:(VZVirtualMachine*)virtualMachine {
  (void)virtualMachine;
  if (stoppedMarkerDirectory >= 0 && !self.startCompleted)
    fail(@"guest stopped before startup was confirmed");
  confirmedStop();
}
- (void)virtualMachine:(VZVirtualMachine*)virtualMachine didStopWithError:(NSError*)error {
  (void)virtualMachine;
  fail(error.localizedDescription);
}
@end

int main(int argc, const char* argv[]) {
  @autoreleasepool {
    if (argc == 2 && std::string_view(argv[1]) == "restore-info") {
      [VZMacOSRestoreImage fetchLatestSupportedWithCompletionHandler:^(VZMacOSRestoreImage* image, NSError* error) {
        if (error || !image || !image.mostFeaturefulSupportedConfiguration)
          fail(error.localizedDescription ?: @"no supported macOS restore image");
        printf("url=%s\nminimum_cpus=%lu\nminimum_memory_bytes=%llu\n", image.URL.absoluteString.UTF8String,
               static_cast<unsigned long>(image.mostFeaturefulSupportedConfiguration.minimumSupportedCPUCount),
               static_cast<unsigned long long>(image.mostFeaturefulSupportedConfiguration.minimumSupportedMemorySize));
        fflush(stdout);
        exit(0);
      }];
      dispatch_main();
    }
    if (argc == 2 && std::string_view(argv[1]) == "--help") {
      puts("usage: ckgit-macos-vm restore-info | install RESTORE-IPSW NEW-BASE DISK-GIB | setup VM-BUNDLE [--provision-password-file FILE] [--setup-share DIR] [--vsock-probe] | validate|run VM-BUNDLE EXCHANGE-DIRECTORY");
      return 0;
    }
    if (argc == 5 && std::string_view(argv[1]) == "install") {
      unsigned diskGiB = 0;
      const std::string_view value(argv[4]);
      const auto [end, error] = std::from_chars(value.data(), value.data() + value.size(), diskGiB);
      if (error != std::errc{} || end != value.data() + value.size()) fail(@"invalid VM disk size");
      installVM(argv[2], argv[3], diskGiB);
    }
    const bool setup = argc >= 3 && std::string_view(argv[1]) == "setup";
    const bool run = argc >= 4 && std::string_view(argv[1]) == "run";
    const char* passwordPath = nullptr;
    const char* probeResult = nullptr;
    const char* setupShare = nullptr;
    bool vsockProbe = false;
    bool broker = false;
    if (setup || run) {
      for (int index = setup ? 3 : 4; index < argc; ++index) {
        const std::string_view option(argv[index]);
        if (option == "--vsock-probe" && !vsockProbe) vsockProbe = true;
        else if (run && option == "--broker" && !broker) broker = true;
        else if (setup && option == "--provision-password-file" && !passwordPath && index + 1 < argc) passwordPath = argv[++index];
        else if (setup && option == "--setup-share" && !setupShare && index + 1 < argc) setupShare = argv[++index];
        else if (run && option == "--probe-result" && !probeResult && index + 1 < argc) probeResult = argv[++index];
        else fail(@"invalid or duplicate setup option");
      }
    }
    if (!setup && !run && (argc != 4 || std::string_view(argv[1]) != "validate")) {
      fprintf(stderr, "usage: ckgit-macos-vm setup VM-BUNDLE | validate|run VM-BUNDLE EXCHANGE-DIRECTORY\n");
      return 2;
    }
    if ((probeResult && !vsockProbe) || (broker && vsockProbe)) fail(@"invalid VM socket mode");
    NSURL* bundle = [NSURL fileURLWithFileSystemRepresentation:argv[2] isDirectory:YES relativeToURL:nil];
    NSURL* exchange = setup ? (setupShare ? [NSURL fileURLWithFileSystemRepresentation:setupShare isDirectory:YES relativeToURL:nil] : nil)
                            : [NSURL fileURLWithFileSystemRepresentation:argv[3] isDirectory:YES relativeToURL:nil];
    requireMountedVolume(bundle.path);
    if (exchange) requireMountedVolume(exchange.path);
    if (setup) {
      const int baseLock = open(bundle.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      struct stat baseInfo {};
      if (baseLock < 0 || fstat(baseLock, &baseInfo) != 0 || !S_ISDIR(baseInfo.st_mode) ||
          baseInfo.st_uid != geteuid() || (baseInfo.st_mode & 0777) != 0700 ||
          flock(baseLock, LOCK_EX | LOCK_NB) != 0)
        fail(@"VM base is invalid or in use; stop setup and wait for cloning to finish");
      // Keep the directory descriptor and exclusive lock until setup exits.
    }
    if (run) {
      struct stat marker {};
      if (::lstat(item(bundle, @".ckgit-disposable-vm").fileSystemRepresentation, &marker) != 0 ||
          !S_ISREG(marker.st_mode) || marker.st_uid != ::geteuid() || marker.st_nlink != 1)
        fail(@"refusing to run a VM without an owned disposable-workspace marker");
      const int useLock = open(item(bundle, @".ckgit-vm-use.lock").fileSystemRepresentation,
                               O_RDWR | O_NOFOLLOW | O_CLOEXEC);
      struct stat lockInfo {};
      if (useLock < 0 || fstat(useLock, &lockInfo) != 0 || !S_ISREG(lockInfo.st_mode) ||
          lockInfo.st_uid != geteuid() || lockInfo.st_nlink != 1 || (lockInfo.st_mode & 0777) != 0600 ||
          flock(useLock, LOCK_EX | LOCK_NB) != 0 || fstat(useLock, &lockInfo) != 0 || lockInfo.st_nlink != 1)
        fail(@"refusing to run without an exclusive VM workspace lock");
      // Deliberately keep the descriptor open for the entire helper lifetime.
      stoppedMarkerDirectory = open(bundle.fileSystemRepresentation, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
      struct stat stoppedInfo {};
      struct stat namedLock {};
      if (stoppedMarkerDirectory < 0 ||
          fstatat(stoppedMarkerDirectory, ".ckgit-vm-use.lock", &namedLock, AT_SYMLINK_NOFOLLOW) != 0 ||
          !S_ISREG(namedLock.st_mode) || namedLock.st_dev != lockInfo.st_dev || namedLock.st_ino != lockInfo.st_ino ||
          (fstatat(stoppedMarkerDirectory, ".ckgit-vm-stopped", &stoppedInfo, AT_SYMLINK_NOFOLLOW) == 0 || errno != ENOENT))
        fail(@"refusing to restart a completed or invalid VM workspace");
    }
    BOOL isDirectory = NO;
    if (exchange && (!exchange.path.isAbsolutePath || ![[NSFileManager defaultManager] fileExistsAtPath:exchange.path isDirectory:&isDirectory] || !isDirectory))
      fail(@"exchange directory does not exist");
    NSData* machineData = [NSData dataWithContentsOfURL:item(bundle, @"MachineIdentifier")];
    NSData* hardwareData = [NSData dataWithContentsOfURL:item(bundle, @"HardwareModel")];
    VZMacMachineIdentifier* identifier = machineData ? [[VZMacMachineIdentifier alloc] initWithDataRepresentation:machineData] : nil;
    VZMacHardwareModel* hardware = hardwareData ? [[VZMacHardwareModel alloc] initWithDataRepresentation:hardwareData] : nil;
    if (!identifier || !hardware || !hardware.supported) fail(@"invalid or unsupported macOS VM identity/hardware model");
    VZMacPlatformConfiguration* platform = [[VZMacPlatformConfiguration alloc] init];
    platform.machineIdentifier = identifier;
    platform.hardwareModel = hardware;
    platform.auxiliaryStorage = [[VZMacAuxiliaryStorage alloc] initWithContentsOfURL:item(bundle, @"AuxiliaryStorage")];
    NSError* error = nil;
    VZDiskImageStorageDeviceAttachment* disk = [[VZDiskImageStorageDeviceAttachment alloc]
        initWithURL:item(bundle, @"Disk.img") readOnly:!(run || setup) error:&error];
    if (!disk) fail(error.localizedDescription ?: @"cannot open VM disk");
    VZVirtualMachineConfiguration* config = [VZVirtualMachineConfiguration new];
    config.platform = platform;
    config.bootLoader = [[VZMacOSBootLoader alloc] init];
    config.CPUCount = MAX(VZVirtualMachineConfiguration.minimumAllowedCPUCount, MIN(4, VZVirtualMachineConfiguration.maximumAllowedCPUCount));
    config.memorySize = MAX(VZVirtualMachineConfiguration.minimumAllowedMemorySize, 8ull * 1024 * 1024 * 1024);
    config.storageDevices = @[[[VZVirtioBlockDeviceConfiguration alloc] initWithAttachment:disk]];
    config.entropyDevices = @[[[VZVirtioEntropyDeviceConfiguration alloc] init]];
    if (vsockProbe || broker) config.socketDevices = @[[[VZVirtioSocketDeviceConfiguration alloc] init]];
    VZMacGraphicsDeviceConfiguration* graphics = [[VZMacGraphicsDeviceConfiguration alloc] init];
    graphics.displays = @[[[VZMacGraphicsDisplayConfiguration alloc] initWithWidthInPixels:1024 heightInPixels:768 pixelsPerInch:80]];
    config.graphicsDevices = @[graphics];
    if (setup) {
      // Network exists only in this explicit, interactive base-preparation mode.
      VZVirtioNetworkDeviceConfiguration* network = [[VZVirtioNetworkDeviceConfiguration alloc] init];
      network.attachment = [[VZNATNetworkDeviceAttachment alloc] init];
      config.networkDevices = @[network];
      printf("setup_network_mac=%s\n", network.MACAddress.string.UTF8String);
      fflush(stdout);
      config.keyboards = @[[[VZUSBKeyboardConfiguration alloc] init]];
      config.pointingDevices = @[[[VZUSBScreenCoordinatePointingDeviceConfiguration alloc] init]];
    }
    if (exchange) {
      VZVirtioFileSystemDeviceConfiguration* share = [[VZVirtioFileSystemDeviceConfiguration alloc]
          initWithTag:VZVirtioFileSystemDeviceConfiguration.macOSGuestAutomountTag];
      // Setup and CI shares are always read-only; CI output travels over Virtio.
      share.share = [[VZSingleDirectoryShare alloc] initWithDirectory:[[VZSharedDirectory alloc] initWithURL:exchange readOnly:YES]];
      config.directorySharingDevices = @[share];
      // CI mode attaches no network device or other host directories.
    }
    if (run && config.networkDevices.count != 0) fail(@"CI VM must not have a network device");
    if (![config validateWithError:&error]) fail(error.localizedDescription ?: @"invalid VM configuration");
    if (!run && !setup) { puts("VM configuration valid"); return 0; }
    VZVirtualMachine* vm = [[VZVirtualMachine alloc] initWithConfiguration:config];
    __attribute__((objc_precise_lifetime)) VMProbeDelegate* probeDelegate = nil;
    __attribute__((objc_precise_lifetime)) VMBridgeDelegate* bridgeDelegate = nil;
    __attribute__((objc_precise_lifetime)) VZVirtioSocketListener* probeListener = nil;
    if (vsockProbe || broker) {
      VZVirtioSocketDevice* device = (VZVirtioSocketDevice*)vm.socketDevices.firstObject;
      if (!device) fail(@"VM Virtio socket device is unavailable");
      probeListener = [VZVirtioSocketListener new];
      if (broker) {
        bridgeDelegate = [VMBridgeDelegate new];
        probeListener.delegate = bridgeDelegate;
      } else {
        probeDelegate = [VMProbeDelegate new];
        if (probeResult) {
          NSString* resultPath = [NSString stringWithUTF8String:probeResult];
          if (!resultPath.isAbsolutePath || ![resultPath.stringByDeletingLastPathComponent.stringByStandardizingPath
              isEqualToString:bundle.path.stringByDeletingLastPathComponent.stringByStandardizingPath])
            fail(@"the probe result must be in the disposable VM workspace");
          probeDelegate.resultPath = resultPath;
        }
        probeListener.delegate = probeDelegate;
      }
      [device setSocketListener:probeListener forPort:41777];
    }
    __attribute__((objc_precise_lifetime)) VMDelegate* delegate = [VMDelegate new];
    vm.delegate = delegate;
    __attribute__((objc_precise_lifetime)) NSWindow* window = nil;
    __attribute__((objc_precise_lifetime)) VMWindowDelegate* windowDelegate = nil;
    if (setup) {
      NSApplication* app = [NSApplication sharedApplication];
      [app setActivationPolicy:NSApplicationActivationPolicyRegular];
      window = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, 1024, 768)
          styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskResizable
          backing:NSBackingStoreBuffered defer:NO];
      window.title = @"ck-git-hosting macOS VM setup";
      VZVirtualMachineView* view = [[VZVirtualMachineView alloc] initWithFrame:window.contentView.bounds];
      view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
      view.virtualMachine = vm;
      window.contentView = view;
      windowDelegate = [VMWindowDelegate new];
      windowDelegate.vm = vm;
      window.delegate = windowDelegate;
      [window center];
      [window makeKeyAndOrderFront:nil];
      [app activateIgnoringOtherApps:YES];
    }
    signal(SIGTERM, SIG_IGN);
    signal(SIGINT, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    dispatch_source_t term = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGTERM, 0, dispatch_get_main_queue());
    dispatch_source_t interrupt = dispatch_source_create(DISPATCH_SOURCE_TYPE_SIGNAL, SIGINT, 0, dispatch_get_main_queue());
    __block bool stopRequested = false;
    __block bool stopInProgress = false;
    __block bool startCompleted = false;
    void (^stop)(void) = ^{
      stopRequested = true;
      // A stop request can arrive before the asynchronous start completes.
      // Do not exit and abandon the VM while the framework is still starting it.
      if (!startCompleted || vm.state == VZVirtualMachineStateStarting || stopInProgress) return;
      if (vm.state == VZVirtualMachineStateStopped) confirmedStop();
      if (!vm.canStop) fail(@"VM cannot be stopped");
      stopInProgress = true;
      [vm stopWithCompletionHandler:^(NSError* failure) {
        if (failure) fail(failure.localizedDescription);
        if (vm.state != VZVirtualMachineStateStopped) fail(@"VM stop did not reach the stopped state");
        confirmedStop();
      }];
    };
    dispatch_source_set_event_handler(term, stop);
    dispatch_source_set_event_handler(interrupt, stop);
    dispatch_resume(term);
    dispatch_resume(interrupt);
    // The runner blocks these across spawn so it cannot kill the helper
    // before shutdown handling exists. A closed broker socket is also watched
    // below, including when a previously pending signal was ignored.
    sigset_t startupSignals;
    sigemptyset(&startupSignals);
    sigaddset(&startupSignals, SIGTERM);
    sigaddset(&startupSignals, SIGINT);
    if (sigprocmask(SIG_UNBLOCK, &startupSignals, nullptr) != 0) fail(@"cannot enable VM shutdown signals");
    __attribute__((objc_precise_lifetime)) dispatch_source_t parentWatch = nil;
    if (broker) {
      int type = 0;
      socklen_t typeLength = sizeof(type);
      if (getsockopt(STDIN_FILENO, SOL_SOCKET, SO_TYPE, &type, &typeLength) != 0 || type != SOCK_STREAM)
        fail(@"VM broker requires a private stream socket");
      parentWatch = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, dispatch_get_main_queue());
      dispatch_source_set_timer(parentWatch, DISPATCH_TIME_NOW, 500 * NSEC_PER_MSEC, 50 * NSEC_PER_MSEC);
      dispatch_source_set_event_handler(parentWatch, ^{
        pollfd monitored{STDIN_FILENO, POLLIN, 0};
        const int ready = poll(&monitored, 1, 0);
        if (ready < 0 && errno != EINTR) { stop(); return; }
        if (ready > 0 && (monitored.revents & (POLLHUP | POLLERR | POLLNVAL))) { stop(); return; }
        if (ready > 0 && (monitored.revents & POLLIN)) {
          char byte;
          const auto count = recv(STDIN_FILENO, &byte, 1, MSG_PEEK | MSG_DONTWAIT);
          if (count == 0 || (count < 0 && errno != EAGAIN && errno != EWOULDBLOCK && errno != EINTR)) stop();
        }
      });
      dispatch_resume(parentWatch);
    }
    void (^started)(NSError*) = ^(NSError* failure) {
      if (failure) fail(failure.localizedDescription);
      startCompleted = true;
      delegate.startCompleted = YES;
      if (stopRequested) { stop(); return; }
      if (broker) fprintf(stderr, "VM started\n");
      else { puts("VM started"); fflush(stdout); }
    };
    if (setup && passwordPath) {
      if (@available(macOS 27.0, *)) {
        const int passwordFD = open(passwordPath, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
        struct stat passwordInfo {};
        if (passwordFD < 0 || fstat(passwordFD, &passwordInfo) != 0 || !S_ISREG(passwordInfo.st_mode) ||
            passwordInfo.st_uid != geteuid() || passwordInfo.st_nlink != 1 || (passwordInfo.st_mode & 0777) != 0600 ||
            passwordInfo.st_size < 16 || passwordInfo.st_size > 128)
          fail(@"provisioning password file must be an owned, private regular file of 16 to 128 bytes");
        char passwordBytes[129]{};
        const auto length = read(passwordFD, passwordBytes, static_cast<std::size_t>(passwordInfo.st_size));
        close(passwordFD);
        if (length != passwordInfo.st_size) fail(@"cannot read provisioning password file");
        NSString* password = [[NSString alloc] initWithBytes:passwordBytes length:static_cast<NSUInteger>(length)
                                                   encoding:NSUTF8StringEncoding];
        volatile char* wipe = passwordBytes;
        for (std::size_t i = 0; i < sizeof(passwordBytes); ++i) wipe[i] = 0;
        password = [password stringByTrimmingCharactersInSet:NSCharacterSet.newlineCharacterSet];
        if (!password || password.length < 16) fail(@"invalid provisioning password");
        VZMacGuestProvisioningOptions* provision = [[VZMacGuestProvisioningOptions alloc] init];
        provision.fullName = @"ck-git-hosting CI";
        provision.username = @"ckgit";
        provision.password = password;
        provision.logsInAutomatically = YES;
        provision.enablesRemoteLogin = YES;
        VZMacOSVirtualMachineStartOptions* options = [[VZMacOSVirtualMachineStartOptions alloc] init];
        NSError* provisionError = nil;
        if (![options setGuestProvisioningOptions:provision error:&provisionError])
          fail(provisionError.localizedDescription ?: @"invalid guest provisioning options");
        [vm startWithOptions:options completionHandler:started];
      } else {
        fail(@"automatic guest provisioning requires macOS 27 or later");
      }
    } else {
      [vm startWithCompletionHandler:started];
    }
    if (setup) { [NSApp run]; return 0; }
    dispatch_main();
  }
}
