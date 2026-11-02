// Copyright (c) 2026 C. Klukas. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once
#include <winsock2.h>
#include <stdexcept>
struct VmTestWinsock {
 VmTestWinsock(){WSADATA data{};if(WSAStartup(MAKEWORD(2,2),&data))throw std::runtime_error("WSAStartup failed");}
 ~VmTestWinsock(){WSACleanup();}
};
inline int vmTestSocketPair(SOCKET output[2]) {
 output[0]=output[1]=INVALID_SOCKET;
 SOCKET listener=WSASocketW(AF_INET,SOCK_STREAM,IPPROTO_TCP,nullptr,0,WSA_FLAG_NO_HANDLE_INHERIT);
 if(listener==INVALID_SOCKET)return -1;
 sockaddr_in address{};address.sin_family=AF_INET;address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
 int size=sizeof(address);
 if(bind(listener,reinterpret_cast<sockaddr*>(&address),size) || listen(listener,1) || getsockname(listener,reinterpret_cast<sockaddr*>(&address),&size)){closesocket(listener);return -1;}
 output[0]=WSASocketW(AF_INET,SOCK_STREAM,IPPROTO_TCP,nullptr,0,WSA_FLAG_NO_HANDLE_INHERIT);
 if(output[0]==INVALID_SOCKET || connect(output[0],reinterpret_cast<sockaddr*>(&address),size)){closesocket(listener);if(output[0]!=INVALID_SOCKET)closesocket(output[0]);output[0]=INVALID_SOCKET;return -1;}
 output[1]=accept(listener,nullptr,nullptr);closesocket(listener);
 if(output[1]==INVALID_SOCKET){closesocket(output[0]);output[0]=INVALID_SOCKET;return -1;}
 return 0;
}
