// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#include <winsock2.h>
#include <windows.h>
#include <hvsocket.h>
#include <objbase.h>
#include "host_connection.hpp"
#include "hcs_vm.hpp"
#include "ckgit/windows.hpp"
#include <stdexcept>
#include <thread>
namespace ckgit {
struct WindowsVmConnection::State {
 bool winsock=false;SOCKET socket=INVALID_SOCKET;
 ~State(){if(socket!=INVALID_SOCKET)closesocket(socket);if(winsock)WSACleanup();}
};
void WindowsVmConnection::preflight(){
 const auto path=L"SOFTWARE\\Microsoft\\Windows NT\\CurrentVersion\\Virtualization\\GuestCommunicationServices\\8f22bd55-d776-43ea-bd82-93b2ee39f6da";
 wchar_t name[128]{},owner[128]{};DWORD bytes=sizeof(name);
 if(RegGetValueW(HKEY_LOCAL_MACHINE,path,L"ElementName",RRF_RT_REG_SZ,nullptr,name,&bytes)!=ERROR_SUCCESS || std::wstring(name)!=L"ck-git offline CI")throw std::runtime_error("operator must register the dedicated ck-git Hyper-V socket application");
 bytes=sizeof(owner);if(RegGetValueW(HKEY_LOCAL_MACHINE,path,L"Owner",RRF_RT_REG_SZ,nullptr,owner,&bytes)!=ERROR_SUCCESS || std::wstring(owner)!=L"ckgit-windows-ci")throw std::runtime_error("Hyper-V socket service GUID ownership differs");
}
WindowsVmConnection::WindowsVmConnection(const std::string& runtime,
 std::chrono::milliseconds timeout,const std::function<bool()>& keep):state_(std::make_unique<State>()){
 preflight();WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))throw std::runtime_error("host WSAStartup failed");state_->winsock=true;
 SOCKADDR_HV address{};address.Family=AF_HYPERV;
 if(FAILED(CLSIDFromString(utf8ToWide("{"+runtime+"}").c_str(),&address.VmId)) || FAILED(CLSIDFromString(utf8ToWide("{"+std::string(kWindowsVmServiceGuid)+"}").c_str(),&address.ServiceId)))throw std::runtime_error("invalid private VM endpoint identity");
 int last_error=0;const auto end=std::chrono::steady_clock::now()+timeout;
 while(std::chrono::steady_clock::now()<end){
  if(keep && !keep())throw std::runtime_error("private VM connection cancelled");
  state_->socket=WSASocketW(AF_HYPERV,SOCK_STREAM,HV_PROTOCOL_RAW,nullptr,0,WSA_FLAG_NO_HANDLE_INHERIT);
  if(state_->socket==INVALID_SOCKET)throw std::runtime_error("cannot create private host Hyper-V socket");
  u_long nonblocking=1;if(ioctlsocket(state_->socket,FIONBIO,&nonblocking))throw std::runtime_error("cannot make host Hyper-V socket nonblocking");
  if(connect(state_->socket,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0)return;
  int error=WSAGetLastError();
  if(error==WSAEWOULDBLOCK || error==WSAEINPROGRESS){
   while(std::chrono::steady_clock::now()<end){
    if(keep && !keep())throw std::runtime_error("private VM connection cancelled");
    fd_set ready,failed;FD_ZERO(&ready);FD_ZERO(&failed);FD_SET(state_->socket,&ready);FD_SET(state_->socket,&failed);timeval wait{0,100000};int result=select(0,nullptr,&ready,&failed,&wait);
    if(result==SOCKET_ERROR)throw std::runtime_error("Hyper-V connection wait failed");if(!result)continue;
    int size=sizeof(error);if(getsockopt(state_->socket,SOL_SOCKET,SO_ERROR,reinterpret_cast<char*>(&error),&size))throw std::runtime_error("cannot inspect Hyper-V connection result");
    if(!error)return;break;
   }
  }
  last_error=error;closesocket(state_->socket);state_->socket=INVALID_SOCKET;
  if(error!=WSAEADDRNOTAVAIL && error!=WSAEACCES && error!=WSAECONNREFUSED && error!=WSAETIMEDOUT && error!=WSAEHOSTUNREACH && error!=WSAENETUNREACH)throw std::runtime_error("Hyper-V connection rejected: "+std::to_string(error));
  std::this_thread::sleep_for(std::chrono::milliseconds(100));
 }
 throw std::runtime_error("private VM service did not become ready before deadline; last socket error "+std::to_string(last_error));
}
WindowsVmConnection::~WindowsVmConnection()=default;
CiVmSocket WindowsVmConnection::socket()const{return static_cast<CiVmSocket>(state_->socket);}
}
