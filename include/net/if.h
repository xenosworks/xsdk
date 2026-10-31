#ifndef XSDK_NET_IF_H
#define XSDK_NET_IF_H

// Interface flags, Linux's and BSD's values: getifaddrs sets IFF_UP, and IFF_RUNNING while the link is up (src/net.c);
// the console's interface is no loopback.

enum {
	IFF_UP = 0x1,
	IFF_LOOPBACK = 0x8,
	IFF_RUNNING = 0x40,
};

#endif
