#ifndef XSDK_NETINET_TCP_H
#define XSDK_NETINET_TCP_H

// Winsock's TCP option, at level IPPROTO_TCP; it passes through to XAM (src/socket.c). A macro, as code tests for it
// with #ifdef (curl-xenon:lib/cf-socket.c does).
#define TCP_NODELAY 0x0001

#endif
