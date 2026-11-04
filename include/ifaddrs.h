#ifndef XSDK_IFADDRS_H
#define XSDK_IFADDRS_H

#include <sys/socket.h>

// BSD's and Linux's list of network interfaces (src/net.c): the console's one, "eth0", with its IPv4 address once XAM
// has one. XAM gives no netmask, so ifa_netmask is NULL.

#ifdef __cplusplus
extern "C" {
#endif

struct ifaddrs {
	struct ifaddrs *ifa_next;
	char *ifa_name;
	unsigned int ifa_flags;
	struct sockaddr *ifa_addr;
	struct sockaddr *ifa_netmask;
	struct sockaddr *ifa_dstaddr;
	void *ifa_data;
};

// Starts the network, as a socket does; returns 0, or -1 with errno ENETDOWN if it won't start or ENOMEM.
int getifaddrs(struct ifaddrs **list);
void freeifaddrs(struct ifaddrs *list);

#ifdef __cplusplus
}
#endif

#endif
