// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "executor.hpp"
#include "host_connection.hpp"
#include "source_transfer.hpp"
#include "../ci-vm-host-macos/result_relay.hpp"
#include "ckgit/archive.hpp"
#include "ckgit/path_utf8.hpp"
#include <chrono>
#include <memory>
#include <stdexcept>
namespace ckgit {
namespace fs=std::filesystem;
namespace {
std::uint64_t epoch(){return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count());}
// All guest-supplied execution metadata is replaced by host-owned values.
// A guest completion remains provisional; this adapter never calls finish.
class ReportingSink final:public CiResultSink {
 CiResultSink& sink_;std::string runner_;CiExecutionReport report_;std::uint64_t started_epoch_;bool started_=false;
 CiRunRecord trusted(CiRunRecord r)const{r.runner=runner_;r.execution=report_;r.started_epoch_seconds=started_epoch_;r.heartbeat_epoch_seconds=epoch();return r;}
 public:
 ReportingSink(CiResultSink& sink,std::string runner,CiExecutionReport report,std::uint64_t started):sink_(sink),runner_(std::move(runner)),report_(std::move(report)),started_epoch_(started){}
 void runStarted(const CiRunRecord& r)override{if(!started_){sink_.runStarted(trusted(r));started_=true;}}
 void stepStarted(const CiRunRecord& r,std::size_t n)override{sink_.stepStarted(trusted(r),n);}
 void appendLog(std::size_t n,std::string_view b)override{sink_.appendLog(n,b);}
 void stepFinished(const CiRunRecord& r,std::size_t n)override{sink_.stepFinished(trusted(r),n);}
 void heartbeat(const CiRunRecord& r)override{sink_.heartbeat(trusted(r));}
 void tick()override{sink_.tick();}
 void progress(const CiRunRecord& r)override{sink_.progress(trusted(r));}
 bool cancelRequested()override{return sink_.cancelRequested();}
 bool leaseLost()override{return sink_.leaseLost();}
 CiArtifactRecord putArtifact(const CiArtifact& a,const fs::path& p,const std::string& n,bool release)override{return sink_.putArtifact(a,p,n,release);}
 void putPagesSite(const fs::path& p)override{sink_.putPagesSite(p);}
 CiRunRecord finish(CiRunRecord)override{throw std::runtime_error("guest cannot commit a terminal host result");}
};
}
CiRunRecord executeWindowsHcsJob(const CiPreparedJob& prepared,const CiExecutionOptions& options,
 const WindowsVmImages& images,CiSourceProvider& source,CiResultSink& sink){
 if(options.allow_network || !options.cache_root.empty() || options.keep_scratch)throw std::runtime_error("HCS jobs require offline disposable storage with no shared cache");
 auto disks=images.createWorkspace(options.build_root);const auto root=disks.os_disk.parent_path();
 CiRunRecord record;record.run_id=prepared.job.job_id;record.parent_run_id=prepared.job.parent_run_id;record.workflow_job=prepared.job.workflow_job;record.project_name=prepared.job.project_name;record.ref=prepared.job.ref;record.commit_id=prepared.job.commit_id;record.attempt=prepared.job.attempt;record.runner=options.runner;record.status=CiRunStatus::Running;record.started_epoch_seconds=epoch();
 const CiExecutionReport execution{"windows","x64","hyperv-hcs","denied",static_cast<std::uint64_t>(disks.memory_mb)*1024*1024,0,options.timeout_seconds};
 const auto host_started=record.started_epoch_seconds;record.execution=execution;ReportingSink reporting(sink,options.runner,execution,host_started);std::unique_ptr<HcsCiVm> vm;
 auto heartbeat=std::chrono::steady_clock::now();
 const auto pulse=[&]{sink.tick();auto now=std::chrono::steady_clock::now();if(now-heartbeat>=std::chrono::seconds(1)){reporting.heartbeat(record);heartbeat=now;}};
 const auto keep=[&]{pulse();return !sink.leaseLost() && !sink.cancelRequested();};
 try{
  reporting.runStarted(record);
  if(!keep())throw std::runtime_error("VM job cancelled before setup");
  auto inputs=root/L"inputs",relay=root/L"relay";fs::create_directory(inputs);fs::create_directory(relay);std::vector<fs::path> archives;
  for(std::size_t i=0;i<=prepared.sisters.size();++i){
   const auto destination=inputs/pathFromUtf8("checkout-"+std::to_string(i));CiCheckoutRequest request;
   if(i==0)request={prepared.job.project_name,prepared.job.commit_id,prepared.workflow.checkout,prepared.job.ref};
   else{const auto& sister=prepared.sisters.at(i-1);request={sister.name,sister.commit,sister.checkout,{}};}
   source.checkout(request,destination,keep);
   std::vector<std::string> paths;for(const auto& child:fs::directory_iterator(destination))paths.push_back(pathToUtf8(child.path().filename()));
   auto archive=inputs/pathFromUtf8("source-"+std::to_string(i)+".tar");TarPackOptions pack;pack.max_bytes=kWindowsVmSourceBytes;pack.should_continue=keep;packRegularTreeTar(destination,paths,archive,pack);archives.push_back(archive);
  }
  vm=std::make_unique<HcsCiVm>(disks,keep);
  WindowsVmConnection connection(vm->runtimeId(),std::chrono::minutes(10),keep);CiVmChannel channel(connection.socket());CiRemotePolicy policy;policy.timeout_seconds=options.timeout_seconds;policy.max_log_bytes=options.max_log_bytes;policy.artifact_max_bytes=options.artifact_max_bytes;policy.allow_network=false;
  ciVmHostHandshake(channel,prepared,policy,std::chrono::minutes(2),keep);sendWindowsVmSources(channel,prepared,archives,keep);
  record=relayCiVmResults(channel,prepared,policy,reporting,relay);record.runner=options.runner;record.execution=execution;
 }catch(const std::exception& e){record.status=sink.cancelRequested()?CiRunStatus::Cancelled:CiRunStatus::Error;record.detail=std::string(e.what()).substr(0,200);}
 // Cancellation or lease expiry cannot interrupt teardown. A failed absence
 // check preserves disks and escapes to the daemon's fatal refusal path.
 if(vm && !vm->stop([&]{pulse();return true;}))throw std::runtime_error("HCS shutdown unconfirmed; workspace retained, runner must stop");
 vm.reset();WindowsVmImages::cleanup(root,disks.id);
 record.started_epoch_seconds=host_started;record.finished_epoch_seconds=record.heartbeat_epoch_seconds=epoch();
 if(sink.leaseLost())throw std::runtime_error("HCS job lease lost; VM stopped and no terminal result written");
 return sink.finish(record);
}
}
