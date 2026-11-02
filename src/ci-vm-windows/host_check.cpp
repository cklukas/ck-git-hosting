// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "hcs_vm.hpp"
#include "storage.hpp"
#include "host_connection.hpp"
#include "ckgit/private_file_windows.hpp"
#include "ckgit/path_utf8.hpp"
#include <windows.h>
#include <fstream>
#include <iostream>
namespace fs=std::filesystem;
namespace {
struct Lock{HANDLE value=INVALID_HANDLE_VALUE;explicit Lock(const fs::path& p){value=CreateFileW(p.c_str(),GENERIC_READ|GENERIC_WRITE,0,nullptr,OPEN_ALWAYS,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);if(value==INVALID_HANDLE_VALUE)throw std::runtime_error("image initialization already active");}~Lock(){if(value!=INVALID_HANDLE_VALUE)CloseHandle(value);}};
int initialize(const fs::path& os,const fs::path& work,const fs::path& sdk,const fs::path& acceptance){
 if(os.root_name()!=L"V:" || work.root_name()!=L"V:" || sdk.root_name()!=L"V:" || acceptance.root_name()!=L"V:" || !fs::is_regular_file(acceptance) || fs::file_size(acceptance)>4096)throw std::runtime_error("explicit V: images and human license acceptance record required");
 std::ifstream input(acceptance,std::ios::binary);std::string accepted((std::istreambuf_iterator<char>(input)),{});
 if(accepted.find("Accept the Windows license and initialize this VM")==std::string::npos || accepted.find("6b5d4b32236a921bacdefe19852ec0699ca2eacd72d4e57f50a2de06ede21c8e")==std::string::npos)throw std::runtime_error("expected human Windows license acceptance absent");
 ckgit::WindowsVmConnection::preflight();
 Lock lock(os.parent_path()/L"first-boot.lock");const auto instances=os.parent_path()/L"first-boot-instances";fs::create_directory(instances);ckgit::WindowsVmImages::recover(instances);
 for(unsigned attempt=1;attempt<=5;++attempt){
  ckgit::HcsCiVmOptions options;options.id=ckgit::HcsCiVm::freshId();auto root=instances/ckgit::pathFromUtf8("ckgit-vm-"+options.id);fs::create_directory(root);ckgit::writePrivateWindowsFile(root/L"vm-owner.txt","ckgit-windows-hcs-v1\n"+options.id+"\n",false,false);options.os_disk=os;options.tools_disk=sdk;options.work_disk=root/L"work.vhdx";
  ckgit::WindowsVmImages::createDifferencingDisk(work,options.work_disk);
  std::unique_ptr<ckgit::HcsCiVm> vm;bool ready=false,retry=false;std::string failure;std::cout<<"First boot attempt "<<attempt<<" HCS id="<<options.id<<'\n';
  try{
   vm=std::make_unique<ckgit::HcsCiVm>(options);std::cout<<"Runtime VM id="<<vm->runtimeId()<<"; waiting for Windows setup and private service\n";auto last=std::chrono::steady_clock::now();bool running=true;
   auto keep=[&]{auto now=std::chrono::steady_clock::now();if(now-last>=std::chrono::seconds(2)){running=vm->running();last=now;}return running;};
   ckgit::WindowsVmConnection connection(vm->runtimeId(),std::chrono::minutes(10),keep);ckgit::CiVmChannel channel(connection.socket());auto hello=channel.receive(std::chrono::minutes(12),keep);
   if(hello.type==ckgit::CiVmFrameType::Error)throw std::runtime_error("guest initialization: "+hello.payload.substr(0,200));
   if(hello.type!=ckgit::CiVmFrameType::Hello || hello.payload!="ckgit-vm-guest-v1")throw std::runtime_error("unexpected offline guest readiness greeting");
   ready=true;std::cout<<"Guest service ready over exact private Hyper-V socket; no job or credentials sent\n";
  }catch(const std::exception& e){failure=e.what();retry=!vm || !vm->running();std::cout<<"First boot phase: "<<failure<<'\n';}
  if(ready && !vm->shutdownClean()){ready=false;std::cout<<"Clean guest shutdown unconfirmed; base is not admitted\n";}
  if(vm && !vm->stop())throw std::runtime_error("base initialization shutdown unconfirmed");vm.reset();
  if(!ready && !retry){if(!ckgit::HcsCiVm::confirmedAbsent(options.id))throw std::runtime_error("startup stop unconfirmed");std::cout<<"Confirmed VM absent; retained pre-job startup diagnostics workspace "<<ckgit::pathToUtf8(root)<<'\n';throw std::runtime_error(failure.empty()?"clean base shutdown not confirmed":failure);}
  ckgit::WindowsVmImages::cleanup(root,options.id);std::cout<<"Confirmed VM absent and disposable work disk removed\n";
  if(ready)return 0;
 }
 throw std::runtime_error("Windows setup did not reach guest service readiness within five confirmed boot phases");
}
}
int wmain(int argc,wchar_t** argv){std::cout<<std::unitbuf;std::cerr<<std::unitbuf;try{
 if(argc==2 && std::wstring(argv[1])==L"--preflight"){ckgit::HcsCiVm::preflight();std::cout<<"HCS diskless startup, kill-on-owner-close attestation and confirmed stop passed\n";return 0;}
 if(argc==4 && std::wstring(argv[1])==L"--storage-check"){
  fs::path parent=argv[3];ckgit::WindowsVmImages::recover(parent);ckgit::WindowsVmImages images(argv[2]);auto disks=images.createWorkspace(parent);ckgit::WindowsVmImages::cleanup(disks.os_disk.parent_path(),disks.id);std::cout<<"Image hashes, deny-write locks, differencing disks and absence-checked cleanup passed\n";return 0;
 }
 if(argc==6 && std::wstring(argv[1])==L"--initialize-base")return initialize(argv[2],argv[3],argv[4],argv[5]);
 std::cerr<<"Use --preflight, --storage-check PROFILE WORKROOT, or --initialize-base OS WORK SDK ACCEPTANCE.\n";return 2;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
