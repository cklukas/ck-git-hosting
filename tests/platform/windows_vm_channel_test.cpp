// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "windows_vm_socket_pair.hpp"
#include "ckgit/ci_vm_protocol.hpp"
#include "ckgit/archive.hpp"
#include "ckgit/temp_directory.hpp"
#include "../../src/ci-vm-windows/source_transfer.hpp"
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <thread>
namespace fs=std::filesystem;
using namespace ckgit;
namespace {
void require(bool yes,const char* why){if(!yes)throw std::runtime_error(why);}
template<class F>bool rejects(F f){try{f();return false;}catch(const std::exception&){return true;}}
struct Pair{SOCKET fd[2];Pair(){require(vmTestSocketPair(fd)==0,"cannot create local test pair");}~Pair(){for(auto value:fd)if(value!=INVALID_SOCKET)closesocket(value);}};
}
int main(){try{
 VmTestWinsock winsock;const char* tmp=std::getenv("TMPDIR");require(tmp && fs::path(tmp).root_name()==L"V:","Select private V: TMPDIR");
 auto root=createTempDirectory(tmp,"windows-vm-channel-");struct Cleanup{fs::path p;~Cleanup(){std::error_code e;fs::remove_all(p,e);}}cleanup{root};
 CiPreparedJob job;job.job.job_id="00000000000000000001-abcdabcd";job.job.project_name="project";job.job.commit_id=std::string(40,'a');job.job.ref="refs/heads/main";job.workflow_text="version: 1\njobs:\n  - name: build\n    steps:\n      - run: [true]\n";job.workflow=parseCiWorkflow(job.workflow_text);CiRemotePolicy policy;policy.allow_network=true;
 {Pair p;CiVmChannel host(p.fd[0]),guest(p.fd[1]);std::exception_ptr error;CiVmAssignment assigned;
  std::jthread worker([&]{try{assigned=ciVmGuestHandshake(guest,std::chrono::seconds(2));}catch(...){error=std::current_exception();}});
  ciVmHostHandshake(host,job,policy,std::chrono::seconds(2));worker.join();if(error)std::rethrow_exception(error);
  require(!assigned.policy.allow_network && assigned.policy.lease_seconds==1 && assigned.prepared.job.commit_id==job.job.commit_id,"assignment boundary changed");
 }
 {Pair p;CiVmChannel sender(p.fd[0]),receiver(p.fd[1]);std::string bytes(kCiVmChunkPayloadMax,'z');bytes[5]='\0';std::exception_ptr error;
  std::jthread writer([&]{try{sender.send(CiVmFrameType::Chunk,bytes,std::chrono::seconds(2));}catch(...){error=std::current_exception();}});
  auto frame=receiver.receive(std::chrono::seconds(2));writer.join();if(error)std::rethrow_exception(error);require(frame.type==CiVmFrameType::Chunk && frame.payload==bytes,"binary channel corruption");
 }
 {Pair p;CiVmChannel receiver(p.fd[1]);auto frame=encodeCiVmFrame(CiVmFrameType::Chunk,"abc");require(send(p.fd[0],frame.data(),static_cast<int>(frame.size()-1),0)>0,"raw truncated write failed");shutdown(p.fd[0],SD_SEND);
  require(rejects([&]{receiver.receive(std::chrono::seconds(1));}) && receiver.failed(),"partial EOF was accepted");
 }
 {Pair p;CiVmChannel receiver(p.fd[1]);require(rejects([&]{receiver.receive(std::chrono::milliseconds(50));}) && receiver.failed(),"timeout did not poison channel");}
 {Pair p;CiVmChannel receiver(p.fd[1]);require(rejects([&]{receiver.receive(std::chrono::seconds(1),[]{return false;});}) && receiver.failed(),"cancellation did not poison channel");}
 {Pair p;CiVmChannel host(p.fd[0]),guest(p.fd[1]);job.sisters.push_back({"sister",std::string(40,'b'),{}});std::vector<fs::path> archives;
  for(int i=0;i<2;++i){auto source=root/("original-"+std::to_string(i));fs::create_directory(source);std::ofstream(source/"file with space.txt",std::ios::binary)<<std::string(200000,'A'+i);auto tar=root/("prepared-"+std::to_string(i)+".tar");TarPackOptions options;options.max_bytes=kWindowsVmSourceBytes;packRegularTreeTar(source,{"."},tar,options);archives.push_back(tar);}
  auto destination=root/"received";fs::create_directory(destination);std::vector<fs::path> extracted;std::exception_ptr error;
  std::jthread worker([&]{try{extracted=receiveWindowsVmSources(guest,job,destination);}catch(...){error=std::current_exception();shutdown(p.fd[1],SD_BOTH);}});
  sendWindowsVmSources(host,job,archives);worker.join();if(error)std::rethrow_exception(error);
  require(extracted.size()==2 && fs::file_size(extracted[1]/"file with space.txt")==200000,"source transfer lost assigned checkout");
 }
 {Pair p;CiVmChannel host(p.fd[0]),guest(p.fd[1]);auto destination=root/"invalid";fs::create_directory(destination);host.send(CiVmFrameType::Event,"source 99 0 "+std::string(64,'0'),std::chrono::seconds(1));require(rejects([&]{receiveWindowsVmSources(guest,job,destination);}) && guest.failed(),"wrong source ordinal accepted");}
 std::cout<<"PASS: restricted handshake, binary framing, EOF/timeout/cancellation, ordinal source archives\n";return 0;
}catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}}
