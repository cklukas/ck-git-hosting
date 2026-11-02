// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "guest_executor.hpp"
#include "ckgit/archive.hpp"
#include "ckgit/hash.hpp"
#include "ckgit/path_utf8.hpp"
#include "ckgit/process.hpp"
#include "ckgit/windows.hpp"
#include <windows.h>
#include <algorithm>
#include <chrono>
#include <fstream>
#include <map>
#include <stdexcept>
namespace ckgit {
namespace fs=std::filesystem;
namespace {
std::uint64_t now(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());}
std::string upper(std::string s){for(auto& c:s)if(c>='a'&&c<='z')c=static_cast<char>(c-'a'+'A');return s;}
std::string variable(std::string prefix,std::string s){s=upper(s);for(auto& c:s)if(!((c>='A'&&c<='Z')||(c>='0'&&c<='9')))c='_';return prefix+s;}
void ordinary(const fs::path& p){auto attributes=GetFileAttributesW(p.c_str());if(attributes==INVALID_FILE_ATTRIBUTES || (attributes&FILE_ATTRIBUTE_REPARSE_POINT))throw std::runtime_error("missing or redirected guest job operand");}
fs::path below(const fs::path& root,const fs::path& relative){if(relative.is_absolute() || relative.has_root_path())throw std::runtime_error("guest job path must be relative");auto p=root;for(const auto& part:relative){if(part=="..")throw std::runtime_error("guest job operand escapes checkout");p/=part;ordinary(p);}return p;}
void tree(const fs::path& root){ordinary(root);for(const auto& entry:fs::recursive_directory_iterator(root))ordinary(entry.path());}
std::string substitute(std::string value,const fs::path& tools){std::size_t offset=0;auto replacement=pathToUtf8(tools);while((offset=value.find("${TOOLS}",offset))!=std::string::npos){value.replace(offset,8,replacement);offset+=replacement.size();}return value;}
struct Cwd{fs::path previous=fs::current_path();explicit Cwd(const fs::path& p){fs::current_path(p);}~Cwd(){std::error_code ignored;fs::current_path(previous,ignored);}};
}
CiRunRecord executeWindowsVmGuestJob(const CiVmAssignment& assigned,const RunnerConfig& profile,
 const std::vector<fs::path>& sources,const fs::path& scratch,CiResultSink& sink) {
 const auto& prepared=assigned.prepared;const auto& job=prepared.job;const auto& policy=assigned.policy;
 CiRunRecord record;record.run_id=job.job_id;record.parent_run_id=job.parent_run_id;record.workflow_job=job.workflow_job;record.project_name=job.project_name;record.ref=job.ref;record.commit_id=job.commit_id;record.attempt=job.attempt;record.runner="windows-vm-guest";record.status=CiRunStatus::Running;record.started_epoch_seconds=now();
 auto finish=[&](CiRunStatus status,std::string detail){record.status=status;detail.resize(std::min(detail.size(),detail.find('\n')));detail.resize(std::min(detail.size(),kMaximumCiDetailBytes));record.detail=std::move(detail);record.finished_epoch_seconds=record.heartbeat_epoch_seconds=now();return sink.finish(record);};
 auto heartbeat=std::chrono::steady_clock::now();
 auto continuing=[&]{sink.tick();auto t=std::chrono::steady_clock::now();if(t-heartbeat>=std::chrono::seconds(1)){sink.heartbeat(record);heartbeat=t;}return !sink.leaseLost() && !sink.cancelRequested();};
 try {
  if(policy.allow_network || !profile.toolchain_root || profile.allow_network || profile.allow_degraded || profile.cache_root || sources.size()!=prepared.sisters.size()+1)throw std::runtime_error("guest requires an offline operator profile and exact assigned sources");
  if(scratch.root_name()!=L"V:" || !scratch.is_absolute() || !fs::is_directory(scratch) || !fs::is_empty(scratch))throw std::runtime_error("guest scratch must be fresh on V:");
  const auto work=sources[0];for(const auto& source:sources)tree(source);Cwd cwd(work);sink.runStarted(record);
  if(!continuing())return finish(CiRunStatus::Cancelled,"cancelled before guest setup");
  std::vector<fs::path> bins;for(const auto& relative:profile.toolchain_paths){auto p=below(*profile.toolchain_root,relative);if(!fs::is_directory(p))throw std::runtime_error("guest tool directory missing");bins.push_back(p);}
  auto resolve=[&](const std::string& name){auto relative=pathFromUtf8(name);if(relative.is_absolute())throw std::runtime_error("guest executable must be a staged name or checkout-relative path");if(relative.has_parent_path())return below(work,relative);for(const auto& bin:bins){auto p=bin/relative;if(!p.has_extension())p+=L".exe";if(fs::is_regular_file(p)){ordinary(p);return p;}}throw std::runtime_error("tool not in approved guest SDK: "+name);};
  std::map<std::string,std::string> common;
  for(const auto& [name,raw]:profile.toolchain_environment)common[upper(name)]=substitute(raw,*profile.toolchain_root);
  for(const auto& bin:bins){if(!common["PATH"].empty())common["PATH"]+=';';common["PATH"]+=pathToUtf8(bin);}
  wchar_t windows[32768]{};auto n=GetWindowsDirectoryW(windows,32768);if(!n || n>=32768)throw std::runtime_error("cannot resolve guest OS");
  common["SYSTEMROOT"]=common["WINDIR"]=wideToUtf8(windows);common["COMSPEC"]=common["SYSTEMROOT"]+"\\System32\\cmd.exe";
  for(std::size_t i=0;i<prepared.sisters.size();++i)common[variable("CKGIT_SISTER_",prepared.sisters[i].name)]=pathToUtf8(sources[i+1]);
  std::size_t index=0;
  for(const auto& ci_job:prepared.workflow.jobs){
   auto environment=common;for(const auto& [key,value]:prepared.workflow.env)environment[upper(key)]=value;for(const auto& [key,value]:ci_job.env)environment[upper(key)]=value;
   for(const auto& cache:prepared.workflow.caches){auto p=scratch/L"cache"/pathFromUtf8(sha256Hex(ci_job.name+":"+cache.name));fs::create_directories(p);environment[variable("CKGIT_CACHE_",cache.name)]=pathToUtf8(p);for(const auto& name:cache.env)environment[upper(name)]=pathToUtf8(p);}
   for(const auto* name:{"TEMP","TMP","TMPDIR","CPL_TMPDIR"})environment[name]=pathToUtf8(scratch);
   for(const auto* name:{"HOME","USERPROFILE","APPDATA","LOCALAPPDATA"}){auto p=scratch/pathFromUtf8(name);fs::create_directory(p);environment[name]=pathToUtf8(p);}
   environment["VSCMD_SKIP_SENDTELEMETRY"]=environment["DOTNET_CLI_TELEMETRY_OPTOUT"]=environment["VCPKG_DISABLE_METRICS"]="1";
   environment["CI"]="true";environment["CKGIT_CI"]="1";environment["CKGIT_COMMIT"]=job.commit_id;environment["CKGIT_REF"]=job.ref;
   if(job.ref.starts_with("refs/heads/"))environment["CKGIT_BRANCH"]=job.ref.substr(11);if(job.ref.starts_with("refs/tags/"))environment["CKGIT_TAG"]=job.ref.substr(10);
   for(const auto& step:ci_job.steps){
    if(!continuing())return finish(CiRunStatus::Cancelled,"cancelled before guest step");
    auto command=step.argv;if(step.usesShell()){auto shell=step.shell.empty()?"pwsh":step.shell;if(shell=="sh")command={shell,"-ec",step.script};else command={shell,"-NoProfile","-NonInteractive","-Command","$ErrorActionPreference='Stop'; & {\n"+step.script+"\n}; if (!$?) { exit 1 }; if ($null -ne $LASTEXITCODE) { exit $LASTEXITCODE }"};}
    command.at(0)=pathToUtf8(resolve(command.at(0)));sink.stepStarted(record,index);ProcessOptions options;options.timeout=std::chrono::seconds(policy.timeout_seconds);options.output_limit=0;options.should_continue=continuing;
    // The service's environment is a guest-only allowlist. Remove it all from
    // this child before applying the declared job environment.
    LPWCH inherited=GetEnvironmentStringsW();if(!inherited)throw std::runtime_error("cannot inspect guest environment");
    for(auto p=inherited;*p;p+=wcslen(p)+1){std::wstring value(p);auto equal=value.find(L'=',value.front()==L'='?1:0);if(equal!=std::wstring::npos && value.front()!=L'=')options.environment[upper(wideToUtf8(value.substr(0,equal)))]=std::nullopt;}FreeEnvironmentStringsW(inherited);
    for(const auto& [key,value]:environment)options.environment[key]=value;
    std::size_t logged=0;bool truncated=false;
    options.on_output=[&](std::string_view bytes){auto count=std::min<std::size_t>(bytes.size(),static_cast<std::size_t>(policy.max_log_bytes)-logged);if(count)sink.appendLog(index,bytes.substr(0,count));logged+=count;truncated|=count!=bytes.size();};
    auto result=runProcess(command,options);record.steps.push_back({step.name.empty()?ci_job.name:step.name,result.exit_code,result.timed_out,truncated});sink.stepFinished(record,index++);
    if(result.cancelled)return finish(CiRunStatus::Cancelled,"cancelled during guest step");if(result.timed_out)return finish(CiRunStatus::Timeout,"guest step exceeded time budget");if(result.exit_code)return finish(CiRunStatus::Failure,"guest step exited "+std::to_string(result.exit_code));
   }
   if(ci_job.artifact){auto bundle=scratch/pathFromUtf8(ci_job.artifact->name+".tar");std::string note;
    try{for(const auto& name:ci_job.artifact->paths)tree(below(work,pathFromUtf8(name)));TarPackOptions pack;pack.max_bytes=policy.artifact_max_bytes;pack.should_continue=continuing;packRegularTreeTar(work,ci_job.artifact->paths,bundle,pack);}catch(const std::exception& error){note=error.what();}
    sink.putArtifact(*ci_job.artifact,bundle,note,prepared.is_release);std::error_code ignored;fs::remove(bundle,ignored);
   }
  }
  if(!continuing())return finish(CiRunStatus::Cancelled,"cancelled before guest publication");
  if(!prepared.is_release && prepared.is_default_branch && prepared.workflow.pages_path){auto site=below(work,pathFromUtf8(*prepared.workflow.pages_path));tree(site);sink.putPagesSite(site);}
  return finish(CiRunStatus::Success,"all guest steps passed");
 }catch(const std::exception& error){return finish(sink.cancelRequested()?CiRunStatus::Cancelled:CiRunStatus::Error,error.what());}
}
}
