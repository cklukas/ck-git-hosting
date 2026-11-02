// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "hcs_vm.hpp"
#include "ckgit/windows.hpp"
#include "ckgit/path_utf8.hpp"
#include <windows.h>
#include <computecore.h>
#include <sddl.h>
#include <objbase.h>
#include <algorithm>
#include <array>
#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <thread>
#include <vector>
namespace ckgit {
namespace fs=std::filesystem;
namespace {
struct Operation {
 HCS_OPERATION value=HcsCreateOperation(nullptr,nullptr);
 Operation(){if(!value)throw std::runtime_error("cannot create HCS operation");}
 ~Operation(){HcsCloseOperation(value);}
 std::wstring finish(HRESULT begun,const std::function<bool()>& keep={},std::chrono::seconds limit=std::chrono::seconds(30)) {
  if(FAILED(begun))throw std::runtime_error("HCS operation rejected: "+std::to_string(static_cast<unsigned long>(begun)));
  const auto end=std::chrono::steady_clock::now()+limit;
  for(;;){
   PWSTR raw=nullptr;auto result=HcsWaitForOperationResult(value,100,&raw);
   std::wstring detail=raw?raw:L"";if(raw)LocalFree(raw);
   if(result==HCS_E_OPERATION_TIMEOUT || result==HRESULT_FROM_WIN32(WAIT_TIMEOUT) || result==HRESULT_FROM_WIN32(ERROR_TIMEOUT)){
    if((keep && !keep()) || std::chrono::steady_clock::now()>end){HcsCancelOperation(value);throw std::runtime_error("HCS operation cancelled or timed out");}continue;
   }
   if(FAILED(result))throw std::runtime_error("HCS operation failed: "+std::to_string(static_cast<unsigned long>(result))+" "+wideToUtf8(detail));
   return detail;
  }
 }
};
bool validId(const std::string& id){if(id.size()!=36)return false;for(std::size_t i=0;i<id.size();++i){if(i==8||i==13||i==18||i==23){if(id[i]!='-')return false;}else if(!((id[i]>='0'&&id[i]<='9')||(id[i]>='a'&&id[i]<='f')||(id[i]>='A'&&id[i]<='F')))return false;}return true;}
std::string guid(){GUID value{};if(FAILED(CoCreateGuid(&value)))throw std::runtime_error("cannot generate HCS identity");wchar_t text[40]{};StringFromGUID2(value,text,40);auto result=wideToUtf8(std::wstring(text+1,36));for(auto& c:result)if(c>='A' && c<='F')c=static_cast<char>(c-'A'+'a');return result;}
std::string sid(){HANDLE token=nullptr;if(!OpenProcessToken(GetCurrentProcess(),TOKEN_QUERY,&token))throw std::runtime_error("cannot inspect HCS owner token");DWORD count=0;GetTokenInformation(token,TokenUser,nullptr,0,&count);std::vector<unsigned char> bytes(count);bool read=count && GetTokenInformation(token,TokenUser,bytes.data(),count,&count);CloseHandle(token);if(!read)throw std::runtime_error("cannot read HCS owner identity");LPWSTR text=nullptr;if(!ConvertSidToStringSidW(reinterpret_cast<TOKEN_USER*>(bytes.data())->User.Sid,&text))throw std::runtime_error("cannot format HCS owner SID");std::string result=wideToUtf8(text);LocalFree(text);return result;}
std::string json(std::string_view value){std::string result="\"";for(unsigned char c:value){if(c=='\\' || c=='\"')result+='\\';if(c<32)throw std::runtime_error("control character in VM configuration");result+=static_cast<char>(c);}return result+'\"';}
std::string field(const std::wstring& text,const wchar_t* name){std::wregex expression(std::wstring(L"\"")+name+L"\"\\s*:\\s*\"([^\"]*)\"");std::wsmatch match;if(!std::regex_search(text,match,expression))throw std::runtime_error("HCS property missing");return wideToUtf8(match[1].str());}
std::string normalized(std::string value){std::transform(value.begin(),value.end(),value.begin(),[](unsigned char c){return static_cast<char>(c>='A'&&c<='Z'?c-'A'+'a':c);});return value;}
void ordinaryFile(const fs::path& path){if(!path.is_absolute())throw std::runtime_error("HCS disks require absolute paths");for(auto p=path;!p.empty();p=p.parent_path()) {auto attributes=GetFileAttributesW(p.c_str());if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("missing or redirected HCS disk path");if(p==p.root_path())break;}if(!fs::is_regular_file(path) || path.extension()!=L".vhdx")throw std::runtime_error("HCS requires ordinary VHDX files");}
bool gone(std::string_view id,const std::function<bool()>& keep){const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(30);const auto query=utf8ToWide("{\"Ids\":["+json(id)+"]}");for(;;){Operation op;auto document=op.finish(HcsEnumerateComputeSystems(query.c_str(),op.value),keep);if(normalized(wideToUtf8(document)).find(normalized(std::string(id)))==std::string::npos)return true;if(keep && !keep())return false;if(std::chrono::steady_clock::now()>=end)return false;std::this_thread::sleep_for(std::chrono::milliseconds(100));}}
std::string configuration(const HcsCiVmOptions* options){
 const std::string allowed="D:P(A;;GA;;;"+sid()+")";
 const std::string boot=options?",\"BootThis\":{\"DeviceType\":\"ScsiDrive\",\"DevicePath\":\"a937f420-75b6-4eaa-9ae0-99a0e56f88ad\",\"DiskNumber\":0}":"";
 std::string value="{\"SchemaVersion\":{\"Major\":2,\"Minor\":3},\"Owner\":\"ckgit-windows-ci\",\"ShouldTerminateOnLastHandleClosed\":true,\"VirtualMachine\":{\"StopOnReset\":true,\"Chipset\":{\"Uefi\":{\"StopOnBootFailure\":false"+boot+"}},\"ComputeTopology\":{\"Memory\":{\"SizeInMB\":"+std::to_string(options?options->memory_mb:256)+",\"AllowOvercommit\":false},\"Processor\":{\"Count\":"+std::to_string(options?options->processors:1)+"}}";
 if(options){
  if(options->memory_mb<1024 || options->memory_mb>32768 || !options->processors || options->processors>16)throw std::runtime_error("HCS resources outside supported operator limits");
  std::array<fs::path,3> paths{options->os_disk,options->work_disk,options->tools_disk};for(const auto& path:paths)ordinaryFile(path);
  if(fs::equivalent(paths[0],paths[1]) || fs::equivalent(paths[0],paths[2]) || fs::equivalent(paths[1],paths[2]))throw std::runtime_error("CI VM disk roles overlap");
  value+=",\"Devices\":{\"Scsi\":{\"a937f420-75b6-4eaa-9ae0-99a0e56f88ad\":{\"Attachments\":{";
  for(std::size_t i=0;i<paths.size();++i){if(i)value+=',';value+=json(std::to_string(i))+":{\"Type\":\"VirtualDisk\",\"Path\":"+json(pathToUtf8(paths[i]))+",\"ReadOnly\":"+(i==2?"true":"false")+"}";}
  value+="}}},\"HvSocket\":{\"HvSocketConfig\":{\"DefaultBindSecurityDescriptor\":\"D:P\",\"DefaultConnectSecurityDescriptor\":\"D:P\",\"ServiceTable\":{"+json(kWindowsVmServiceGuid)+":{\"BindSecurityDescriptor\":\"D:P\",\"ConnectSecurityDescriptor\":"+json(allowed)+",\"AllowWildcardBinds\":false}}}}}";
 }
 return value+"}}";
}
}
struct HcsCiVm::State {
 HCS_SYSTEM system=nullptr;std::string id=guid(),runtime;bool confirmed=false;
 ~State(){if(system)HcsCloseComputeSystem(system);}
 void create(const HcsCiVmOptions* options,const std::function<bool()>& keep){
  if(keep && !keep())throw std::runtime_error("HCS creation cancelled before startup");
  if(options){if(!validId(options->id))throw std::runtime_error("broker must supply a journaled fresh VM GUID");id=options->id;}
  auto config=utf8ToWide(configuration(options));auto identity=utf8ToWide(id);
  {Operation op;op.finish(HcsCreateComputeSystem(identity.c_str(),config.c_str(),op.value,nullptr,&system),keep);}
  {Operation op;op.finish(HcsStartComputeSystem(system,op.value,nullptr),keep);}
  Operation query;auto properties=query.finish(HcsGetComputeSystemProperties(system,query.value,L"{\"PropertyTypes\":[\"TerminateOnLastHandleClosed\"]}"),keep);
  if(!std::regex_search(properties,std::wregex(L"\"TerminateOnLastHandleClosed\"\\s*:\\s*true")))throw std::runtime_error("HCS did not attest kill-on-owner-close");
  runtime=field(properties,L"RuntimeId");GUID parsed{};if(FAILED(CLSIDFromString(utf8ToWide("{"+runtime+"}").c_str(),&parsed)))throw std::runtime_error("invalid HCS runtime identity");
  if(field(properties,L"State")!="Running")throw std::runtime_error("HCS VM not running after start");
 }
 bool stop(const std::function<bool()>& keep={}){
  if(confirmed)return true;
  // Cancellation cannot interrupt containment teardown. Continue heartbeat
  // callbacks while ignoring their lease/cancel result until absence is proven.
  auto cleanup=[&]{if(keep){try{keep();}catch(...){}}return true;};
  if(system){
   try {Operation op;op.finish(HcsTerminateComputeSystem(system,op.value,nullptr),cleanup);}catch(...){}
   HcsCloseComputeSystem(system);system=nullptr;
  }
  confirmed=gone(id,cleanup);return confirmed;
 }
};
HcsCiVm::HcsCiVm(const HcsCiVmOptions& options,const std::function<bool()>& keep):state_(std::make_unique<State>()) {
 try{state_->create(&options,keep);}catch(...){state_->stop();throw;}
}
HcsCiVm::~HcsCiVm(){if(state_){try{state_->stop();}catch(...){}}}
const std::string& HcsCiVm::id()const{return state_->id;}
const std::string& HcsCiVm::runtimeId()const{return state_->runtime;}
bool HcsCiVm::running()const{if(!state_->system)return false;try{Operation query;auto properties=query.finish(HcsGetComputeSystemProperties(state_->system,query.value,nullptr));return field(properties,L"State")=="Running";}catch(...){return false;}}
bool HcsCiVm::shutdownClean(){if(state_->confirmed)return true;if(!state_->system)return false;try{Operation op;op.finish(HcsShutDownComputeSystem(state_->system,op.value,nullptr),{},std::chrono::seconds(120));if(!gone(state_->id,{}))return false;HcsCloseComputeSystem(state_->system);state_->system=nullptr;state_->confirmed=true;return true;}catch(...){return false;}}
bool HcsCiVm::stop(const std::function<bool()>& keep){return state_->stop(keep);}
std::string HcsCiVm::freshId(){return guid();}
bool HcsCiVm::confirmedAbsent(const std::string& id){if(!validId(id))throw std::runtime_error("invalid recovery VM identity");return gone(id,{});}
void HcsCiVm::preflight(const std::function<bool()>& keep){State probe;try{probe.create(nullptr,keep);if(!probe.stop(keep))throw std::runtime_error("HCS preflight stop unconfirmed");}catch(...){probe.stop();throw;}}
}
