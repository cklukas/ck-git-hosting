// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#ifdef _WIN32
#include "ckgit/ci_vm_protocol.hpp"
#include <winsock2.h>
#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>
namespace ckgit {
namespace {
auto deadlineFor(std::chrono::milliseconds timeout) {
  if(timeout<=std::chrono::milliseconds::zero() || timeout>std::chrono::hours(24))
    throw std::invalid_argument("invalid VM channel timeout");
  return std::chrono::steady_clock::now()+timeout;
}
void ready(SOCKET socket,bool writing,std::chrono::steady_clock::time_point end,const std::function<bool()>& keep) {
  for(;;) {
    if(keep && !keep())throw std::runtime_error("VM channel cancelled");
    auto now=std::chrono::steady_clock::now();if(now>=end)throw std::runtime_error("VM channel timed out");
    auto wait=std::clamp<std::int64_t>(std::chrono::duration_cast<std::chrono::microseconds>(end-now).count(),1,100000);
    timeval timeout{0,static_cast<long>(wait)};fd_set read{},write{},error{};
    FD_ZERO(&read);FD_ZERO(&write);FD_ZERO(&error);FD_SET(socket,&error);
    if(writing)FD_SET(socket,&write);else FD_SET(socket,&read);
    const int result=select(0,&read,&write,&error,&timeout);
    if(result==SOCKET_ERROR && WSAGetLastError()==WSAEINTR)continue;
    if(result==SOCKET_ERROR || FD_ISSET(socket,&error))throw std::runtime_error("VM socket polling failed");
    if(result>0)return;
  }
}
bool retry(){const int error=WSAGetLastError();return error==WSAEINTR || error==WSAEWOULDBLOCK;}
}
CiVmChannel::CiVmChannel(CiVmSocket socket):fd_(socket) {
  if(socket==INVALID_SOCKET)throw std::invalid_argument("invalid VM socket");
  u_long nonblocking=1;if(ioctlsocket(socket,FIONBIO,&nonblocking))throw std::runtime_error("cannot make VM channel nonblocking");
}
void CiVmChannel::send(CiVmFrameType type,std::string_view payload,std::chrono::milliseconds timeout,const std::function<bool()>& keep) {
  if(failed_)throw std::runtime_error("VM channel already failed");
  try {
    auto frame=encodeCiVmFrame(type,payload);auto end=deadlineFor(timeout);std::size_t offset=0;
    while(offset<frame.size()) {
      ready(fd_,true,end,keep);
      const auto remaining=std::min<std::size_t>(frame.size()-offset,64*1024);
      int count=::send(fd_,frame.data()+offset,static_cast<int>(remaining),0);
      if(count==SOCKET_ERROR && retry())continue;
      if(count<=0){const auto error=count==SOCKET_ERROR?WSAGetLastError():0;throw std::runtime_error("VM socket write failed: Winsock "+std::to_string(error)+", requested "+std::to_string(remaining)+" bytes");}offset+=static_cast<std::size_t>(count);
    }
  }catch(...){failed_=true;throw;}
}
CiVmFrame CiVmChannel::receive(std::chrono::milliseconds timeout,const std::function<bool()>& keep) {
  if(failed_)throw std::runtime_error("VM channel already failed");
  try {
    auto end=deadlineFor(timeout);
    for(;;) {
      if(keep && !keep())throw std::runtime_error("VM channel cancelled");
      if(auto frame=decoder_.next())return std::move(*frame);
      ready(fd_,false,end,keep);std::array<char,64*1024> bytes{};
      int count=recv(fd_,bytes.data(),static_cast<int>(bytes.size()),0);
      if(count==SOCKET_ERROR && retry())continue;
      if(count==SOCKET_ERROR)throw std::runtime_error("VM socket read failed");
      if(!count){decoder_.finish();throw std::runtime_error("VM socket closed");}
      decoder_.feed({bytes.data(),static_cast<std::size_t>(count)});
    }
  }catch(...){failed_=true;throw;}
}
}
#endif
