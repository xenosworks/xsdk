#include "xsdk/net.h"

#include <errno.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <stdlib.h>
#include <string.h>

#include "socket_private.h"
#include "xsdk/kernel.h"

// XNET_GET_XNADDR_ bits: 0 while the address is pending, and how it was got once there is one
// (xenia:src/xenia/kernel/xam/xam_net.cc:429-450).
enum {
	ADDRESS_PENDING = 0,
	ADDRESS_STATIC = 0x04,
	ADDRESS_DHCP = 0x08,
	ADDRESS_PPPOE = 0x10,
};

// getifaddrs' one interface, in one block that freeifaddrs frees whole.
typedef struct {
	struct ifaddrs entry;
	struct sockaddr_in address;
	char name[sizeof("eth0")];
} Interface;

// Here, not where it's read, so the compiler can't take its zeros for the title's.
__attribute__((weak)) const XsdkNetworkLimits xsdk_network_limits = { 0 };

int xsdk_net_link(uint32_t *link)
{
	const int error = xsdk_network_start();
	if (error)
		return error;
	*link = NetDll_XNetGetEthernetLinkStatus(XNCALLER_TITLE);
	return 0;
}

int xsdk_net_address(struct in_addr *address)
{
	const int error = xsdk_network_start();
	if (error)
		return error;
	XNADDR found;
	const uint32_t status = NetDll_XNetGetTitleXnAddr(XNCALLER_TITLE, &found);
	if (status == ADDRESS_PENDING)
		return EAGAIN;
	if (!(status & (ADDRESS_STATIC | ADDRESS_DHCP | ADDRESS_PPPOE)))
		return EADDRNOTAVAIL;
	// Both in network order, which is the console's own.
	address->s_addr = found.address;
	return 0;
}

int getifaddrs(struct ifaddrs **list)
{
	uint32_t link;
	const int error = xsdk_net_link(&link);
	if (error) {
		errno = error;
		return -1;
	}
	Interface *const found = calloc(1, sizeof(*found));
	if (!found) {
		errno = ENOMEM;
		return -1;
	}
	strcpy(found->name, "eth0");
	found->entry.ifa_name = found->name;
	found->entry.ifa_flags = IFF_UP | ((link & XSDK_NET_LINK_ACTIVE) ? IFF_RUNNING : 0);
	// Until XAM has an address the interface is listed without one, as an interface without one is.
	if (xsdk_net_address(&found->address.sin_addr) == 0) {
		found->address.sin_family = AF_INET;
		found->entry.ifa_addr = (struct sockaddr *)&found->address;
	}
	*list = &found->entry;
	return 0;
}

void freeifaddrs(struct ifaddrs *list)
{
	// The entry starts the block getifaddrs took.
	free(list);
}
