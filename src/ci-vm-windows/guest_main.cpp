// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Guest service only. There is no host runner credential or lease token.
#include <winsock2.h>
#include <windows.h>
#include <hvsocket.h>
#include <objbase.h>
#include "hcs_vm.hpp"
#include "guest_executor.hpp"
#include "source_transfer.hpp"
#include "../ci-vm-guest-macos/result_sink.hpp"
#include "ckgit/windows.hpp"
#include <atomic>
#include <filesystem>
#include <iostream>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <vector>
#include <cwctype>
namespace {
namespace fs=std::filesystem;
std::atomic_bool stopped=false;
std::atomic<DWORD> startup_error{ERROR_GEN_FAILURE};
[[noreturn]] void socketFailure(const char* message){const auto code=WSAGetLastError();startup_error=static_cast<DWORD>(code);throw std::runtime_error(std::string(message)+": "+std::to_string(code));}
SERVICE_STATUS_HANDLE service_handle=nullptr;
void status(DWORD state,DWORD error=NO_ERROR){if(!service_handle)return;SERVICE_STATUS value{};value.dwServiceType=SERVICE_WIN32_OWN_PROCESS;value.dwCurrentState=state;value.dwControlsAccepted=state==SERVICE_RUNNING?SERVICE_ACCEPT_STOP|SERVICE_ACCEPT_SHUTDOWN:0;value.dwWin32ExitCode=error;if(error==ERROR_SERVICE_SPECIFIC_ERROR)value.dwServiceSpecificExitCode=startup_error.load();value.dwWaitHint=30000;SetServiceStatus(service_handle,&value);}
DWORD WINAPI control(DWORD code,DWORD,void*,void*){if(code==SERVICE_CONTROL_STOP || code==SERVICE_CONTROL_SHUTDOWN){stopped=true;status(SERVICE_STOP_PENDING);}return NO_ERROR;}
void manageVolumes(){HANDLE token=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_ADJUST_PRIVILEGES|TOKEN_QUERY,&token))throw std::runtime_error("cannot open guest service token");TOKEN_PRIVILEGES privileges{};privileges.PrivilegeCount=1;if(!LookupPrivilegeValueW(nullptr,L"SeManageVolumePrivilege",&privileges.Privileges[0].Luid)){CloseHandle(token);throw std::runtime_error("cannot resolve guest volume privilege");}privileges.Privileges[0].Attributes=SE_PRIVILEGE_ENABLED;SetLastError(NO_ERROR);bool changed=AdjustTokenPrivileges(token,FALSE,&privileges,0,nullptr,nullptr);DWORD error=GetLastError();CloseHandle(token);if(!changed || error==ERROR_NOT_ALL_ASSIGNED)throw std::runtime_error("guest service cannot manage its own volumes");}
struct Volume{std::wstring path;DWORD flags=0;};
Volume findVolume(const wchar_t* label){wchar_t name[1024]{};HANDLE search=FindFirstVolumeW(name,1024);if(search==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot enumerate guest volumes");Volume found;try{do{wchar_t actual[256]{},filesystem[256]{};DWORD flags=0;if(GetVolumeInformationW(name,actual,256,nullptr,nullptr,&flags,filesystem,256) && _wcsicmp(actual,label)==0){if(!found.path.empty() || _wcsicmp(filesystem,L"NTFS"))throw std::runtime_error("ambiguous or unsupported guest disk role");found={name,flags};}}while(FindNextVolumeW(search,name,1024));FindVolumeClose(search);return found;}catch(...){FindVolumeClose(search);throw;}}
std::wstring volumeAt(const wchar_t* path){wchar_t volume[1024]{};if(!GetVolumeNameForVolumeMountPointW(path,volume,1024))return {};return volume;}
// Windows permits one drive letter per volume. Fresh data disks can receive
// a letter automatically before the service starts; move only this verified
// development volume, never the system volume or an unrelated target.
void releaseOtherDriveLetters(const wchar_t* target,const Volume& volume){
 if(wcslen(target)!=3 || target[1]!=L':' || target[2]!=L'\\')return;
 wchar_t system[MAX_PATH]{};if(!GetWindowsDirectoryW(system,MAX_PATH))throw std::runtime_error("cannot identify guest system drive");
 DWORD size=0;GetVolumePathNamesForVolumeNameW(volume.path.c_str(),nullptr,0,&size);
 if(!size || size>65536)throw std::runtime_error("cannot size guest development volume mount paths");
 std::vector<wchar_t> paths(size);if(!GetVolumePathNamesForVolumeNameW(volume.path.c_str(),paths.data(),size,&size))throw std::runtime_error("cannot inspect guest development volume mount paths");
 for(const wchar_t* p=paths.data();*p;p+=wcslen(p)+1){
  if(wcslen(p)!=3 || p[1]!=L':' || p[2]!=L'\\' || !_wcsicmp(p,target))continue;
  if(towupper(p[0])==towupper(system[0]) || _wcsicmp(volumeAt(p).c_str(),volume.path.c_str()))throw std::runtime_error("refuse to reassign an unexpected guest volume");
  if(!DeleteVolumeMountPointW(p))throw std::runtime_error("cannot release guest development drive letter: Win32 "+std::to_string(GetLastError()));
 }
}
void mount(const wchar_t* path,const Volume& volume){auto previous=volumeAt(path);if(!previous.empty()){if(_wcsicmp(previous.c_str(),volume.path.c_str()))throw std::runtime_error("guest development mount already has another volume");return;}releaseOtherDriveLetters(path,volume);if(!SetVolumeMountPointW(path,volume.path.c_str())){const auto error=GetLastError();startup_error=error;throw std::runtime_error("cannot mount guest disk role at "+ckgit::wideToUtf8(path)+": Win32 "+std::to_string(error));}}
void prepareVolumes(){DWORD optIn=1,bytes=sizeof(optIn);if(RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Policies\\Microsoft\\VisualStudio\\SQM",L"OptIn",RRF_RT_REG_DWORD,nullptr,&optIn,&bytes)!=ERROR_SUCCESS || optIn!=0)throw std::runtime_error("guest requires machine Visual Studio telemetry opt-out policy");manageVolumes();Volume work,tools;auto end=std::chrono::steady_clock::now()+std::chrono::seconds(60);while(std::chrono::steady_clock::now()<end && !stopped){work=findVolume(L"CKGIT_WORK");tools=findVolume(L"CKGIT_SDK");if(!work.path.empty() && !tools.path.empty())break;std::this_thread::sleep_for(std::chrono::milliseconds(200));}if(work.path.empty() || tools.path.empty() || work.path==tools.path || (work.flags&FILE_READ_ONLY_VOLUME) || !(tools.flags&FILE_READ_ONLY_VOLUME))throw std::runtime_error("guest disk roles/read-only SDK were not established");mount(L"V:\\",work);fs::create_directory(L"V:\\tools");mount(L"V:\\tools\\",tools);
 const auto canary=L"V:\\tools\\.ckgit-guest-write-probe";HANDLE probe=CreateFileW(canary,GENERIC_WRITE,0,nullptr,CREATE_NEW,FILE_ATTRIBUTE_NORMAL,nullptr);if(probe!=INVALID_HANDLE_VALUE){CloseHandle(probe);DeleteFileW(canary);throw std::runtime_error("guest can write its SDK disk");}
 fs::create_directory(L"V:\\tmp");for(auto name:{L"TEMP",L"TMP",L"TMPDIR"})if(!SetEnvironmentVariableW(name,L"V:\\tmp"))throw std::runtime_error("cannot set guest temporary root");
}
// A readiness greeting must not admit an OS image while OOBE/specialize is
// still progressing. Read Microsoft's documented state; never rewrite it.
void awaitSetup(){auto end=std::chrono::steady_clock::now()+std::chrono::minutes(10);while(!stopped && std::chrono::steady_clock::now()<end){wchar_t state[256]{};DWORD bytes=sizeof(state);if(RegGetValueW(HKEY_LOCAL_MACHINE,L"SOFTWARE\\Microsoft\\Windows\\CurrentVersion\\Setup\\State",L"ImageState",RRF_RT_REG_SZ,nullptr,state,&bytes)==ERROR_SUCCESS && std::wstring(state)==L"IMAGE_STATE_COMPLETE")return;std::this_thread::sleep_for(std::chrono::milliseconds(500));}throw std::runtime_error("Windows setup has not reached IMAGE_STATE_COMPLETE");}
struct Winsock{bool active=false;void start(){WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))throw std::runtime_error("guest WSAStartup failed");active=true;}~Winsock(){if(active)WSACleanup();}};
struct Socket{SOCKET value=INVALID_SOCKET;~Socket(){if(value!=INVALID_SOCKET)closesocket(value);}};
int execute(){Winsock winsock;Socket listener,connection;try{
 winsock.start();
 listener.value=WSASocketW(AF_HYPERV,SOCK_STREAM,HV_PROTOCOL_RAW,nullptr,0,WSA_FLAG_NO_HANDLE_INHERIT);if(listener.value==INVALID_SOCKET)socketFailure("cannot create guest Hyper-V listener");
 SOCKADDR_HV address{};address.Family=AF_HYPERV;address.VmId=HV_GUID_PARENT;if(FAILED(CLSIDFromString(ckgit::utf8ToWide("{"+std::string(ckgit::kWindowsVmServiceGuid)+"}").c_str(),&address.ServiceId)))throw std::runtime_error("invalid guest service GUID");
 if(bind(listener.value,reinterpret_cast<sockaddr*>(&address),sizeof(address)) || listen(listener.value,1))socketFailure("cannot bind private guest service");
 u_long nonblocking=1;if(ioctlsocket(listener.value,FIONBIO,&nonblocking))socketFailure("cannot make guest listener nonblocking");
 auto end=std::chrono::steady_clock::now()+std::chrono::minutes(5);
 while(!stopped && std::chrono::steady_clock::now()<end){connection.value=accept(listener.value,nullptr,nullptr);if(connection.value!=INVALID_SOCKET)break;if(WSAGetLastError()!=WSAEWOULDBLOCK)socketFailure("guest channel accept failed");std::this_thread::sleep_for(std::chrono::milliseconds(100));}
 if(connection.value==INVALID_SOCKET)throw std::runtime_error("host did not connect to guest");if(!SetHandleInformation(reinterpret_cast<HANDLE>(connection.value),HANDLE_FLAG_INHERIT,0))throw std::runtime_error("cannot protect guest channel from child inheritance");closesocket(listener.value);listener.value=INVALID_SOCKET;
 prepareVolumes();awaitSetup();const fs::path root=L"V:\\tmp\\ckgit-vm-job";if(!fs::create_directory(root))throw std::runtime_error("guest job root is not fresh");auto input=root/L"input",scratch=root/L"scratch";fs::create_directory(input);fs::create_directory(scratch);
 auto profile=ckgit::loadRunnerConfig(L"V:\\tools\\guest-toolchain.ini");if(!profile.toolchain_root || fs::canonical(*profile.toolchain_root)!=fs::canonical(L"V:\\tools\\approved") || profile.identity || profile.known_hosts)throw std::runtime_error("guest profile is not the credential-free SDK profile");
 ckgit::CiVmChannel channel(connection.value);auto keep=[] {return !stopped.load();};
 auto assigned=ckgit::ciVmGuestHandshake(channel,std::chrono::minutes(2),keep);auto sources=ckgit::receiveWindowsVmSources(channel,assigned.prepared,input,keep);
 ckgit::CiVmGuestSink sink(channel,assigned.prepared,assigned.policy,root);auto result=ckgit::executeWindowsVmGuestJob(assigned,profile,sources,scratch,sink);
 closesocket(connection.value);connection.value=INVALID_SOCKET;return result.status==ckgit::CiRunStatus::Success?0:1;
 }catch(const std::exception& error){
  // Pre-job startup diagnostics stay on the dedicated development disk. If
  // V: cannot be established, preserve the SCM error code and write nowhere.
  try{auto work=findVolume(L"CKGIT_WORK");if(!work.path.empty() && !(work.flags&FILE_READ_ONLY_VOLUME)){manageVolumes();mount(L"V:\\",work);fs::create_directory(L"V:\\tmp");std::ofstream log(fs::path(L"V:\\tmp\\ckgit-guest-startup-error.txt"),std::ios::binary);log<<std::string(error.what()).substr(0,300)<<'\n';}}catch(...){}
  std::cerr<<error.what()<<'\n';if(connection.value!=INVALID_SOCKET){try{ckgit::CiVmChannel channel(connection.value);channel.send(ckgit::CiVmFrameType::Error,std::string(error.what()).substr(0,200),std::chrono::seconds(2));}catch(...){}}return 1;}}
void WINAPI serviceMain(DWORD,LPWSTR*){service_handle=RegisterServiceCtrlHandlerExW(L"ckgit-vm-guest",control,nullptr);if(!service_handle)return;status(SERVICE_RUNNING);auto result=execute();status(SERVICE_STOPPED,result?ERROR_SERVICE_SPECIFIC_ERROR:NO_ERROR);}
}
int wmain(int argc,wchar_t** argv){if(argc!=2 || std::wstring(argv[1])!=L"--service"){std::cerr<<"This executable is the offline Windows VM guest service.\n";return 2;}SERVICE_TABLE_ENTRYW table[]={{const_cast<LPWSTR>(L"ckgit-vm-guest"),serviceMain},{nullptr,nullptr}};return StartServiceCtrlDispatcherW(table)?0:1;}
