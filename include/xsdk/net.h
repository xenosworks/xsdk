#ifndef XSDK_NET_H
#define XSDK_NET_H

#include <netinet/in.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// XAM's network beside the sockets: the link, the console's address and the limits it starts with (docs/GUIDE.md,
// "Sockets").

// XAM's link bits, XNET_ETHERNET_LINK_* (xenia:src/xenia/kernel/xam/xam_net.cc:529-535); 0 is no link.
enum {
	XSDK_NET_LINK_ACTIVE = 0x01,
	XSDK_NET_LINK_100MBPS = 0x02,
	XSDK_NET_LINK_10MBPS = 0x04,
	XSDK_NET_LINK_FULL_DUPLEX = 0x08,
	XSDK_NET_LINK_HALF_DUPLEX = 0x10,
};

// What XAM's network starts with, as XNetStartupParams carries it (xenia:src/xenia/kernel/xam/xam_net.cc:154-168); 0
// keeps XAM's own: 32 stream sockets, 8 datagram, 16 KB buffers. XAM takes up to 232 sockets of each kind and 63 KB
// buffers (docs/HARDWARE.md, "XAM's sockets and loader, milestone 56").
typedef struct {
	uint8_t stream_sockets;
	uint8_t datagram_sockets;
	uint8_t receive_kb;
	uint8_t send_kb;
} XsdkNetworkLimits;

// A title that wants more defines this; libxsdk's own, weak, is all 0. xsdk reads it as the network starts, which is
// before main where the network log starts it.
extern const XsdkNetworkLimits xsdk_network_limits;

// The Ethernet link's XSDK_NET_LINK_ bits into link. Starts the network, as a socket does; returns 0, or ENETDOWN if it
// won't start.
int xsdk_net_link(uint32_t *link);
// The console's IPv4 address into address. Starts the network; returns 0, EAGAIN while XAM is still getting one,
// EADDRNOTAVAIL with none, or ENETDOWN if the network won't start.
int xsdk_net_address(struct in_addr *address);

#ifdef __cplusplus
}
#endif

#endif
