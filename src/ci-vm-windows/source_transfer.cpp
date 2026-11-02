// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include "source_transfer.hpp"
#include "ckgit/archive.hpp"
#include "ckgit/hash.hpp"
#include <array>
#include <charconv>
#include <fstream>
#include <stdexcept>
namespace ckgit {
namespace fs=std::filesystem;
namespace {
constexpr auto timeout=std::chrono::minutes(2);
void check(const std::function<bool()>& keep){if(keep && !keep())throw std::runtime_error("VM source transfer cancelled");}
std::uint64_t size(std::string_view value){std::uint64_t n=0;auto parsed=std::from_chars(value.data(),value.data()+value.size(),n);if(value.empty() || parsed.ec!=std::errc{} || parsed.ptr!=value.data()+value.size() || std::to_string(n)!=value)throw std::runtime_error("invalid VM source size");return n;}
}
void sendWindowsVmSources(CiVmChannel& channel,const CiPreparedJob& assigned,const std::vector<fs::path>& sources,const std::function<bool()>& keep) {
 try {
  if(sources.size()!=assigned.sisters.size()+1)throw std::invalid_argument("VM source count differs from assignment");
  std::uint64_t total=0;
  for(std::size_t i=0;i<sources.size();++i){
   check(keep);auto length=fs::file_size(sources[i]);if(length>kWindowsVmSourceBytes || length>kWindowsVmTotalSourceBytes-total)throw std::runtime_error("VM source exceeds transfer budget");total+=length;
   const auto hash=sha256HexOfFile(sources[i]);std::ifstream file(sources[i],std::ios::binary);if(!file)throw std::runtime_error("cannot open prepared VM source archive");
   channel.send(CiVmFrameType::Event,"source "+std::to_string(i)+" "+std::to_string(length)+" "+hash,timeout,keep);
   std::array<char,kCiVmChunkPayloadMax> bytes{};std::uint64_t left=length;
   while(left){check(keep);auto count=static_cast<std::streamsize>(std::min<std::uint64_t>(left,bytes.size()));file.read(bytes.data(),count);if(file.gcount()!=count)throw std::runtime_error("VM source changed while reading");channel.send(CiVmFrameType::Chunk,{bytes.data(),static_cast<std::size_t>(count)},timeout,keep);left-=static_cast<std::uint64_t>(count);}
   if(file.peek()!=std::char_traits<char>::eof())throw std::runtime_error("VM source grew during transfer");
   channel.send(CiVmFrameType::Done,{},timeout,keep);auto ack=channel.receive(timeout,keep);
   if(ack.type!=CiVmFrameType::Event || ack.payload!="source-ready "+std::to_string(i))throw std::runtime_error("invalid VM source acknowledgement");
  }
 }catch(...){channel.invalidate();throw;}
}
std::vector<fs::path> receiveWindowsVmSources(CiVmChannel& channel,const CiPreparedJob& assigned,const fs::path& root,const std::function<bool()>& keep) {
 try {
  if(!root.is_absolute() || !fs::is_directory(root) || !fs::is_empty(root))throw std::invalid_argument("VM source root must be fresh and empty");
  std::vector<fs::path> result;std::uint64_t total=0;
  for(std::size_t i=0;i<=assigned.sisters.size();++i){
   check(keep);auto frame=channel.receive(timeout,keep);const auto prefix="source "+std::to_string(i)+" ";
   if(frame.type!=CiVmFrameType::Event || !frame.payload.starts_with(prefix))throw std::runtime_error("unexpected VM source ordinal");
   auto fields=std::string_view(frame.payload).substr(prefix.size());auto separator=fields.find(' ');if(separator==std::string_view::npos)throw std::runtime_error("invalid VM source header");
   auto length=size(fields.substr(0,separator));auto hash=fields.substr(separator+1);
   if(hash.size()!=64 || hash.find_first_not_of("0123456789abcdef")!=std::string_view::npos || length>kWindowsVmSourceBytes || length>kWindowsVmTotalSourceBytes-total)throw std::runtime_error("invalid VM source identity/budget");total+=length;
   auto archive=root/("source-"+std::to_string(i)+".tar");if(fs::exists(archive))throw std::runtime_error("VM source archive already exists");std::ofstream out(archive,std::ios::binary);if(!out)throw std::runtime_error("cannot create guest source archive");
   auto left=length;while(left){auto chunk=channel.receive(timeout,keep);if(chunk.type!=CiVmFrameType::Chunk || chunk.payload.empty() || chunk.payload.size()>left)throw std::runtime_error("invalid VM source chunk");out.write(chunk.payload.data(),static_cast<std::streamsize>(chunk.payload.size()));if(!out)throw std::runtime_error("cannot write guest source archive");left-=chunk.payload.size();}
   auto done=channel.receive(timeout,keep);if(done.type!=CiVmFrameType::Done || !done.payload.empty())throw std::runtime_error("invalid VM source terminator");out.close();if(!out || sha256HexOfFile(archive)!=hash)throw std::runtime_error("VM source checksum differs");
   auto destination=root/("checkout-"+std::to_string(i));TarExtractOptions options;options.max_archive_bytes=kWindowsVmSourceBytes;options.max_bytes=kWindowsVmSourceBytes;options.max_file_bytes=kWindowsVmSourceBytes;options.max_entries=100000;options.should_continue=keep;extractRegularTreeTar(archive,destination,options);fs::remove(archive);result.push_back(destination);
   channel.send(CiVmFrameType::Event,"source-ready "+std::to_string(i),timeout,keep);
  }
  return result;
 }catch(...){channel.invalidate();throw;}
}
}
