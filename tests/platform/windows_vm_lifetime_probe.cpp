// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
// Opt-in administrator/Hyper-V administrator qualification. Creates only
// uniquely identified empty HCS VMs: no guest OS, network, disks or shares.
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <computecore.h>
#include <objbase.h>
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
namespace {
struct Operation {
  HCS_OPERATION value = HcsCreateOperation(nullptr, nullptr);
  Operation() { if (!value) throw std::runtime_error("HcsCreateOperation failed"); }
  ~Operation() { HcsCloseOperation(value); }
  std::wstring finish(HRESULT started) {
    PWSTR detail = nullptr;
    const HRESULT result = FAILED(started) ? started : HcsWaitForOperationResult(value, 30000, &detail);
    std::wstring message = detail ? detail : L"";
    if (detail) LocalFree(detail);
    if (FAILED(result)) {
      std::wcerr << L"HCS operation failed: 0x" << std::hex << static_cast<unsigned long>(result)
                 << L" " << message << L'\n';
      throw std::runtime_error("HCS operation failed");
    }
    return message;
  }
};
struct System {
  HCS_SYSTEM value = nullptr;
  ~System() { if (value) HcsCloseComputeSystem(value); }
  void close() { if (value) HcsCloseComputeSystem(value); value=nullptr; }
};
std::wstring makeId() {
  GUID value{}; if (FAILED(CoCreateGuid(&value))) throw std::runtime_error("GUID generation failed");
  wchar_t text[40]{}; if (!StringFromGUID2(value,text,40)) throw std::runtime_error("GUID formatting failed");
  return std::wstring(text+1,36);
}
constexpr wchar_t config[] = LR"({"SchemaVersion":{"Major":2,"Minor":1},"Owner":"ckgit-windows-vm-lifetime-probe","ShouldTerminateOnLastHandleClosed":true,"VirtualMachine":{"StopOnReset":true,"Chipset":{"Uefi":{}},"ComputeTopology":{"Memory":{"SizeInMB":256,"AllowOvercommit":false},"Processor":{"Count":1}}}})";
std::wstring enumerate(const std::wstring& id) {
  const auto query=L"{\"Ids\":[\""+id+L"\"]}";
  Operation op;
  return op.finish(HcsEnumerateComputeSystems(query.c_str(),op.value));
}
void assertGone(const std::wstring& id) {
  const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(30);
  while(std::chrono::steady_clock::now()<end) {
    auto state=enumerate(id);
    if(state.find(id)==std::wstring::npos) { std::wcout<<L"Confirmed removed "<<id<<L'\n';return; }
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
  }
  throw std::runtime_error("HCS VM survived owner handle closure");
}
void create(const std::wstring& id,System& system) {
  {Operation op;op.finish(HcsCreateComputeSystem(id.c_str(),config,op.value,nullptr,&system.value));}
  {Operation op;op.finish(HcsStartComputeSystem(system.value,op.value,nullptr));}
  Operation op;
  auto props=op.finish(HcsGetComputeSystemProperties(system.value,op.value,L"{\"PropertyTypes\":[\"TerminateOnLastHandleClosed\"]}"));
  auto compact=props;compact.erase(std::remove_if(compact.begin(),compact.end(),[](wchar_t c){return c==L' ' || c==L'\r' || c==L'\n' || c==L'\t';}),compact.end());
  if(compact.find(L"\"TerminateOnLastHandleClosed\":true")==std::wstring::npos)
    throw std::runtime_error("HCS did not attest terminate-on-last-handle policy");
  if(enumerate(id).find(id)==std::wstring::npos)throw std::runtime_error("HCS VM not present after startup");
  std::wcout<<L"Started empty offline HCS VM "<<id<<L" policy "<<props<<L'\n';
}
std::wstring executable() {
  std::wstring text(32768,L'\0');auto n=GetModuleFileNameW(nullptr,text.data(),static_cast<DWORD>(text.size()));
  if(!n || n>=text.size())throw std::runtime_error("Cannot resolve probe executable");text.resize(n);return text;
}
std::filesystem::path scratch() {
  wchar_t value[32768]{};auto n=GetEnvironmentVariableW(L"TMPDIR",value,32768);
  if(!n || n>=32768)throw std::runtime_error("TMPDIR is required");
  auto path=std::filesystem::canonical(value);
  if(_wcsicmp(path.root_name().c_str(),L"V:") || !std::filesystem::is_directory(path))
    throw std::runtime_error("TMPDIR must be an existing private V: directory");
  return path;
}
}
int wmain(int argc,wchar_t** argv) {
  try {
    auto root=scratch();
    if(argc==4 && std::wstring(argv[1])==L"--owner") {
      const std::wstring id(argv[2]);GUID admitted{};
      if(FAILED(CLSIDFromString((L"{"+id+L"}").c_str(),&admitted)))throw std::runtime_error("Invalid VM ID");
      const std::filesystem::path ready(argv[3]);
      if(ready.parent_path()!=root || std::filesystem::exists(ready))throw std::runtime_error("Unsafe owner marker");
      System system;create(id,system);
      {std::ofstream out(ready,std::ios::binary);out<<"ready";if(!out)throw std::runtime_error("Cannot write owner marker");}
      Sleep(120000);return 2;
    }
    if(argc!=1)throw std::runtime_error("Unexpected arguments");
    const auto normal=makeId();{System system;create(normal,system);system.close();}assertGone(normal);
    const auto abrupt=makeId();const auto ready=root/(abrupt+L".ready");
    auto command=L"\""+executable()+L"\" --owner "+abrupt+L" \""+ready.wstring()+L"\"";
    STARTUPINFOW startup{};startup.cb=sizeof(startup);PROCESS_INFORMATION process{};
    if(!CreateProcessW(executable().c_str(),command.data(),nullptr,nullptr,FALSE,CREATE_NO_WINDOW,nullptr,root.c_str(),&startup,&process))
      throw std::runtime_error("Cannot start crash owner");
    CloseHandle(process.hThread);
    try {
      const auto end=std::chrono::steady_clock::now()+std::chrono::seconds(45);
      while(!std::filesystem::exists(ready)) {
        if(WaitForSingleObject(process.hProcess,100)!=WAIT_TIMEOUT || std::chrono::steady_clock::now()>end)
          throw std::runtime_error("Crash owner did not create its VM");
      }
      if(!TerminateProcess(process.hProcess,91) || WaitForSingleObject(process.hProcess,5000)!=WAIT_OBJECT_0)
        throw std::runtime_error("Cannot terminate crash owner");
      CloseHandle(process.hProcess);process.hProcess=nullptr;
      assertGone(abrupt);std::filesystem::remove(ready);
    } catch(...) {
      if(process.hProcess){TerminateProcess(process.hProcess,92);WaitForSingleObject(process.hProcess,5000);CloseHandle(process.hProcess);}
      throw;
    }
    std::cout<<"PASS: normal close and forced owner death remove unique empty offline VMs\n";
    return 0;
  } catch(const std::exception& error) {std::cerr<<error.what()<<'\n';return 1;}
}
