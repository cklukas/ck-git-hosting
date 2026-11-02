// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "ckgit/runner_config.hpp"
#include "../../src/ci-vm-windows/hcs_vm.hpp"
#include "ckgit/ci_store.hpp"
#include "ckgit/temp_directory.hpp"
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
namespace fs=std::filesystem;
void require(bool yes,const char* message){if(!yes)throw std::runtime_error(message);}
template<class F>bool rejects(F f){try{f();return false;}catch(const std::exception&){return true;}}
int main(){try{
 const char* temp=std::getenv("TMPDIR");require(temp && fs::path(temp).root_name()==L"V:","Select explicit V: TMPDIR");auto root=ckgit::createTempDirectory(temp,"vm-config-");struct Cleanup{fs::path p;~Cleanup(){std::error_code e;fs::remove_all(p,e);}}cleanup{root};auto file=root/L"runner.ini";
 const auto id=ckgit::HcsCiVm::freshId();require(id.size()==36 && id.find_first_not_of("0123456789abcdef-")==std::string::npos,"fresh VM GUID is not canonical");
 require(rejects([&]{ckgit::HcsCiVm::confirmedAbsent("not-a-guid");}),"invalid recovery identity accepted");
 const std::string base="schema_version=1\nserver=ckgit@example.invalid\nname=test-runner\nci_build_root=V:/tmp/test-runner\n";
 const auto read=[&](std::string extra){std::ofstream(file,std::ios::binary)<<base+extra;return ckgit::loadRunnerConfig(file);};
 require(read("").windows_backend=="lpac","default Windows isolation changed");
 for(const auto& invalid:{"ci_windows_backend=none\n","ci_windows_backend=hyperv-hcs\n","ci_windows_vm_profile=V:/profile.ini\n","ci_windows_backend=hyperv-hcs\nci_windows_vm_profile=V:/profile.ini\n","ci_windows_backend=hyperv-hcs\nci_windows_vm_profile=V:/profile.ini\nci_allow_network=false\nci_cache_root=V:/cache\n","ci_allow_degraded=true\n"})require(rejects([&]{read(invalid);}),"unsafe Windows backend configuration accepted");
 const auto hcs=read("ci_windows_backend=hyperv-hcs\nci_windows_vm_profile=V:/profile.ini\nci_allow_network=false\n");require(hcs.windows_backend=="hyperv-hcs" && hcs.windows_vm_profile==fs::path(L"V:/profile.ini"),"explicit HCS configuration changed");
 const auto encoded=ckgit::renderRunnerConfig(hcs);std::ofstream(file,std::ios::binary)<<encoded;require(ckgit::loadRunnerConfig(file).windows_backend=="hyperv-hcs","HCS config round trip failed");
 const auto value="1,windows,x64,hyperv-hcs,denied,8589934592,0,7200";require(ckgit::formatCiExecutionReport(ckgit::parseCiExecutionReport(value))==value,"HCS report round trip failed");
 for(const auto& invalid:{"1,macos,arm64,hyperv-hcs,denied,0,0,0","1,linux,x64,hyperv-hcs,denied,0,0,0","1,windows,x64,hyperv-hcs,denied,-1,0,0"})require(rejects([&]{ckgit::parseCiExecutionReport(invalid);}),"invalid HCS report accepted");
 std::cout<<"PASS: isolated defaults, explicit offline HCS config, no shared caches/degraded fallback, platform-bound reports\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
