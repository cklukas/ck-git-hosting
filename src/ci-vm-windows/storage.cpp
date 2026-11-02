// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "storage.hpp"
#include "ckgit/hash.hpp"
#include "ckgit/path_utf8.hpp"
#include "ckgit/private_file_windows.hpp"
#include <windows.h>
#include <initguid.h>
#include <virtdisk.h>
#include <objbase.h>
#include <array>
#include <charconv>
#include <fstream>
#include <map>
#include <stdexcept>
namespace ckgit {
namespace fs=std::filesystem;
namespace {
void ordinary(const fs::path& path,bool directory){
 if(!path.is_absolute() || path.root_name()!=L"V:")throw std::runtime_error("Windows VM development paths must be on V:");
 for(auto p=path;!p.empty();p=p.parent_path()){
  auto a=GetFileAttributesW(p.c_str());if(a==INVALID_FILE_ATTRIBUTES || (a&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("missing or redirected Windows VM storage path");if(p==p.root_path())break;
 }
 if(directory?!fs::is_directory(path):!fs::is_regular_file(path))throw std::runtime_error("Windows VM storage role mismatch");
}
std::map<std::string,std::string> readProfile(const fs::path& path){ordinary(path,false);if(fs::file_size(path)>16384)throw std::runtime_error("VM image profile too large");std::ifstream f(path,std::ios::binary);std::map<std::string,std::string> result;for(std::string line;std::getline(f,line);){if(!line.empty() && line.back()=='\r')line.pop_back();if(line.empty() || line[0]=='#')continue;auto equals=line.find('=');if(equals==std::string::npos || !result.emplace(line.substr(0,equals),line.substr(equals+1)).second)throw std::runtime_error("invalid VM image profile");}return result;}
unsigned number(const std::string& value,unsigned low,unsigned high){unsigned n=0;auto parsed=std::from_chars(value.data(),value.data()+value.size(),n);if(parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || n<low || n>high)throw std::runtime_error("VM image resource limit invalid");return n;}
std::string readOwner(const fs::path& root){ordinary(root,true);auto owner=root/L"vm-owner.txt";ordinary(owner,false);if(fs::file_size(owner)>128)throw std::runtime_error("VM workspace owner record invalid");std::ifstream in(owner,std::ios::binary);std::string marker,id,extra;std::getline(in,marker);std::getline(in,id);if(marker!="ckgit-windows-hcs-v1" || std::getline(in,extra) || root.filename()!=pathFromUtf8("ckgit-vm-"+id))throw std::runtime_error("VM workspace identity invalid");return id;}
void difference(const fs::path& parent,const fs::path& child){
 VIRTUAL_STORAGE_TYPE storage{VIRTUAL_STORAGE_TYPE_DEVICE_VHDX,VIRTUAL_STORAGE_TYPE_VENDOR_MICROSOFT};CREATE_VIRTUAL_DISK_PARAMETERS parameters{};parameters.Version=CREATE_VIRTUAL_DISK_VERSION_2;
 if(FAILED(CoCreateGuid(&parameters.Version2.UniqueId)))throw std::runtime_error("cannot generate differencing disk identity");parameters.Version2.ParentPath=parent.c_str();
 HANDLE disk=INVALID_HANDLE_VALUE;auto error=CreateVirtualDisk(&storage,child.c_str(),VIRTUAL_DISK_ACCESS_NONE,nullptr,CREATE_VIRTUAL_DISK_FLAG_NONE,0,&parameters,nullptr,&disk);if(disk!=INVALID_HANDLE_VALUE)CloseHandle(disk);
 if(error!=ERROR_SUCCESS)throw std::runtime_error("cannot create disposable differencing disk: "+std::to_string(error));
 ordinary(child,false);
}
}
struct WindowsVmImages::State {
 std::array<fs::path,3> paths;std::array<HANDLE,3> locks{INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE,INVALID_HANDLE_VALUE};unsigned memory=8192,processors=4;std::string identity;
 ~State(){for(auto handle:locks)if(handle!=INVALID_HANDLE_VALUE)CloseHandle(handle);}
};
WindowsVmImages::WindowsVmImages(const fs::path& profile,const std::function<bool()>& keep):state_(std::make_unique<State>()){
 const auto fields=readProfile(profile);static const std::array<std::string,9> keys{"schema_version","os_disk","os_sha256","work_disk","work_sha256","sdk_disk","sdk_sha256","memory_mb","processors"};
 if(fields.size()!=keys.size())throw std::runtime_error("VM image profile has missing or unknown fields");for(const auto& key:keys)if(!fields.contains(key))throw std::runtime_error("VM image profile field missing");if(fields.at("schema_version")!="1")throw std::runtime_error("unsupported VM image profile version");
 state_->memory=number(fields.at("memory_mb"),2048,32768);state_->processors=number(fields.at("processors"),1,16);
 std::string identity;
 for(std::size_t i=0;i<3;++i){const std::string role=i==0?"os":i==1?"work":"sdk";auto path=pathFromUtf8(fields.at(role+"_disk"));ordinary(path,false);if(path.extension()!=L".vhdx")throw std::runtime_error("VM image must be VHDX");
  for(std::size_t j=0;j<i;++j)if(fs::equivalent(path,state_->paths[j]))throw std::runtime_error("VM image roles overlap");
  state_->locks[i]=CreateFileW(path.c_str(),GENERIC_READ,FILE_SHARE_READ,nullptr,OPEN_EXISTING,FILE_ATTRIBUTE_NORMAL|FILE_FLAG_OPEN_REPARSE_POINT,nullptr);if(state_->locks[i]==INVALID_HANDLE_VALUE)throw std::runtime_error("cannot seal approved VM image against writes/deletion");
  const auto expected=fields.at(role+"_sha256");if(expected.size()!=64 || expected.find_first_not_of("0123456789abcdef")!=std::string::npos || sha256HexOfFile(path,keep)!=expected)throw std::runtime_error("VM image differs from operator-approved identity");state_->paths[i]=path;identity+=role+":"+expected+"\n";
 }
 state_->identity=sha256Hex(identity+std::to_string(state_->memory)+":"+std::to_string(state_->processors));
}
WindowsVmImages::~WindowsVmImages()=default;
const std::string& WindowsVmImages::fingerprint()const{return state_->identity;}
HcsCiVmOptions WindowsVmImages::createWorkspace(const fs::path& parent)const{
 ordinary(parent,true);const auto id=HcsCiVm::freshId();auto root=parent/pathFromUtf8("ckgit-vm-"+id);if(!fs::create_directory(root))throw std::runtime_error("VM workspace collision");
 // Flush a fixed owner record before any VM creation. The exact same GUID is
 // supplied to HCS; a successor proves its absence before deleting this root.
 writePrivateWindowsFile(root/L"vm-owner.txt","ckgit-windows-hcs-v1\n"+id+"\n",false,false);
 HcsCiVmOptions result;result.id=id;result.memory_mb=state_->memory;result.processors=state_->processors;result.os_disk=root/L"os.vhdx";result.work_disk=root/L"work.vhdx";result.tools_disk=root/L"sdk.vhdx";result.grant_private_disk_access=true;
 try{difference(state_->paths[0],result.os_disk);difference(state_->paths[1],result.work_disk);difference(state_->paths[2],result.tools_disk);}catch(...){cleanup(root,id);throw;}return result;
}
void WindowsVmImages::cleanup(const fs::path& root,const std::string& id){
 if(readOwner(root)!=id || !HcsCiVm::confirmedAbsent(id))throw std::runtime_error("VM workspace stop/ownership unconfirmed; retained and runner quarantined");
 // A guest never sees host paths. Refuse to traverse even an unexpected host
 // reparse entry; do not turn a cleanup error into permission to claim again.
 for(const auto& entry:fs::recursive_directory_iterator(root)){auto a=GetFileAttributesW(entry.path().c_str());if(a==INVALID_FILE_ATTRIBUTES || (a&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("redirected VM workspace retained for inspection");}
 std::error_code error;fs::remove_all(root,error);if(error || fs::exists(root))throw std::runtime_error("VM workspace cleanup failed; refuse new jobs");
}
void WindowsVmImages::createDifferencingDisk(const fs::path& parent,const fs::path& child){ordinary(parent,false);ordinary(child.parent_path(),true);if(fs::exists(child))throw std::runtime_error("differencing disk already exists");difference(parent,child);}
void WindowsVmImages::recover(const fs::path& parent){ordinary(parent,true);for(const auto& child:fs::directory_iterator(parent)){auto name=pathToUtf8(child.path().filename());if(!name.starts_with("ckgit-vm-"))throw std::runtime_error("unknown entry in dedicated VM workspace root");auto id=readOwner(child.path());cleanup(child.path(),id);}}
}
