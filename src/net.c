#include "xsdk/net.h"

#include <errno.h>

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
