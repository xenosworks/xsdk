#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "xsdk/kernel.h"
#include "xsdk/log.h"
#include "xsdk/thread.h"

// What a title's sockets do on the console, through XAM's exports and never xsdk's sockets, so xsdk never starts the
// network itself: the 2026-10-14 probe (docs/scope/networking.md, step 1), and milestone 56's step 1
// (docs/scope/56-store-networking.md). probe_net_host.txt beside the .xex holds the PC's address, the port
// tools/net_echo.py listens on, and XNetStartup's flags: "192.168.0.125 7370 0". probe_net_start.txt names the cases
// to run, in order, and may give XNetStartup's limits first, "startup=<stream sockets>,<datagram sockets>,<receive
// KB>,<send KB>", and restart's, "restart=..." the same; without it, the 2026-10-14 run, "reach".

enum {
	// Winsock's values, which XAM takes as they are (docs/HARDWARE.md, "Sockets in xsdk").
	NET_INET = 2,
	NET_STREAM = 1,
	NET_DATAGRAM = 2,
	NET_TCP = 6,
	NET_UDP = 17,
	NET_SOCKET_LEVEL = 0xFFFF,
	NET_SOCKET_ERROR_OPTION = 0x1007,
	NET_LINGER = 0x0080,
	NET_SEND_BUFFER = 0x1001,
	NET_RECEIVE_BUFFER = 0x1002,
	NET_SEND_TIMEOUT = 0x1005,
	NET_RECEIVE_TIMEOUT = 0x1006,
	NET_WOULD_BLOCK = 10035,
	NET_IN_PROGRESS = 10036,
	NET_INVALID_SOCKET = -1,
	// Xenia takes either as disabling a socket's encryption (xenia:src/xenia/kernel/xsocket.cc:78-82).
	NET_MARK_INSECURE = 0x5801,
	NET_VERSION = 0x0202,
	// XNetStartupParams: its size, flags, datagram and stream sockets, receive and send KB, then 7 more
	// (xenia:src/xenia/kernel/xam/xam_net.cc:154-168).
	STARTUP_PARAMS_BYTES = 13,
	STARTUP_FLAGS = 1,
	STARTUP_DATAGRAM_SOCKETS = 2,
	STARTUP_STREAM_SOCKETS = 3,
	STARTUP_RECEIVE_KB = 4,
	STARTUP_SEND_KB = 5,
	// NetDll_XNetGetOpt's option for the startup settings in force (xam_net.cc:206-223).
	OPTION_STARTUP = 1,
	// XNET_GET_XNADDR_PENDING and XNET_ETHERNET_LINK_ACTIVE (xam_net.cc:429-450, 529-535).
	ADDRESS_PENDING = 0,
	LINK_ACTIVE = 0x01,
	WSADATA_BYTES = 512,
	LOCALHOST = 0x7F000001,
	CONSOLE_LISTEN_PORT = 7371,
	LOOPBACK_PORT = 7375,
	CONNECT_TIMEOUT_MS = 3000,
	PUBLIC_CONNECT_TIMEOUT_MS = 5000,
	RECEIVE_TIMEOUT_MS = 3000,
	ACCEPT_TIMEOUT_MS = 5000,
	ADDRESS_TIMEOUT_MS = 10000,
	ADDRESS_POLL_MS = 10,
	DNS_TIMEOUT_MS = 10000,
	POLL_MS = 50,
	MICROSECONDS_PER_MILLISECOND = 1000,
	LINE_BYTES = 128,
	HTTP_PORT = 80,
	HTTPS_PORT = 443,
	// Past the most the startup's byte can ask for, 255.
	MAX_SOCKETS = 300,
	// How long net_echo.py keeps a held connection, and the timeouts asked for, each well inside it.
	HOLD_MS = 3000,
	LONG_HOLD_MS = 5000,
	SOCKET_TIMEOUT_MS = 500,
	// A send taking this long counts as having waited for its timeout.
	SLOW_SEND_MS = 300,
	CHUNK_BYTES = 16384,
	MAX_CHUNKS = 2048,
	LINGER_SECONDS = 2,
	CLOSE_SETTLE_MS = 300,
	CASE_NAME_BYTES = 32,
	MAX_RUN = 16,
};

// Winsock's FIONBIO, which XAM takes (docs/HARDWARE.md, "Sockets reach the LAN and the internet when marked insecure").
static const uint32_t FIONBIO = 0x8004667E;

// sockaddr_in, big-endian as the console is (xenia:src/xenia/kernel/xsocket.h:63-67).
typedef struct {
	uint16_t family;
	uint16_t port;
	uint32_t address;
	uint8_t zero[8];
} NetAddress;

typedef struct {
	uint32_t pc;
	uint16_t port;
	char pc_text[16];
	uint8_t startup[STARTUP_PARAMS_BYTES];
	uint8_t restart[STARTUP_PARAMS_BYTES];
	int64_t started_us;
} Probe;

typedef struct {
	const char *name;
	// What may go wrong past a failed call, for the first log line; NULL if nothing.
	const char *risk;
	void (*run)(const Probe *probe);
} ProbeCase;

static const char *const mode_names[] = { "plain", "marked 0x5801" };
static int32_t held_sockets[MAX_SOCKETS];
static uint8_t chunk[CHUNK_BYTES];

static int64_t now_us(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * 1000000 + now.tv_nsec / 1000;
}

static unsigned ms_since(int64_t start_us)
{
	return (unsigned)((now_us() - start_us) / 1000);
}

static void address_text(uint32_t address, char text[16])
{
	snprintf(text, 16, "%u.%u.%u.%u", (unsigned)(address >> 24), (unsigned)(address >> 16 & 0xFF),
		(unsigned)(address >> 8 & 0xFF), (unsigned)(address & 0xFF));
}

static bool parse_address(const char *text, uint32_t *address)
{
	unsigned parts[4];
	if (sscanf(text, "%u.%u.%u.%u", &parts[0], &parts[1], &parts[2], &parts[3]) != 4)
		return false;
	*address = parts[0] << 24 | parts[1] << 16 | parts[2] << 8 | parts[3];
	return true;
}

static NetAddress net_address(uint32_t address, uint16_t port)
{
	return (NetAddress){ .family = NET_INET, .port = port, .address = address };
}

static void set_blocking(int32_t socket, bool blocking)
{
	uint32_t non_blocking = !blocking;
	if (NetDll_ioctlsocket(XNCALLER_TITLE, socket, FIONBIO, &non_blocking) != 0)
		xsdk_log_write("net:   FIONBIO failed: %d", NetDll_WSAGetLastError());
}

// A socket of the type, marked insecure when the mode asks, and non-blocking; or NET_INVALID_SOCKET.
static int32_t open_socket(int type, int mode)
{
	const int32_t socket = NetDll_socket(XNCALLER_TITLE, NET_INET, type, type == NET_STREAM ? NET_TCP : NET_UDP);
	if (socket == NET_INVALID_SOCKET) {
		xsdk_log_write("net:   socket failed: %d", NetDll_WSAGetLastError());
		return socket;
	}
	if (mode == 1) {
		const uint32_t on = 1;
		if (NetDll_setsockopt(XNCALLER_TITLE, socket, NET_SOCKET_LEVEL, NET_MARK_INSECURE, &on, sizeof on) != 0)
			xsdk_log_write("net:   setsockopt 0x5801 failed: %d", NetDll_WSAGetLastError());
	}
	set_blocking(socket, false);
	return socket;
}

// Waits for the socket to be readable, or writable, or to fail; returns whether it got there in time.
static bool wait_socket(int32_t socket, bool write, uint32_t timeout_ms)
{
	WSA_FD_SET wanted = { .count = 1, .sockets = { socket } };
	WSA_FD_SET failed = { .count = 1, .sockets = { socket } };
	const WSA_TIMEVAL timeout = { .seconds = (int32_t)(timeout_ms / 1000),
		.microseconds = (int32_t)(timeout_ms % 1000 * MICROSECONDS_PER_MILLISECOND) };
	const int ready =
		NetDll_select(XNCALLER_TITLE, 0, write ? NULL : &wanted, write ? &wanted : NULL, &failed, &timeout);
	if (ready < 0) {
		xsdk_log_write("net:   select failed: %d", NetDll_WSAGetLastError());
		return false;
	}
	return ready > 0 && wanted.count > 0;
}

static bool connect_socket(int32_t socket, uint32_t address, uint16_t port, uint32_t timeout_ms)
{
	const NetAddress to = net_address(address, port);
	if (NetDll_connect(XNCALLER_TITLE, socket, &to, sizeof to) == 0)
		return true;
	const int error = NetDll_WSAGetLastError();
	if (error != NET_WOULD_BLOCK && error != NET_IN_PROGRESS) {
		xsdk_log_write("net:   connect failed: %d", error);
		return false;
	}
	if (!wait_socket(socket, true, timeout_ms)) {
		int32_t socket_error = 0;
		int32_t length = sizeof socket_error;
		(void)NetDll_getsockopt(
			XNCALLER_TITLE, socket, NET_SOCKET_LEVEL, NET_SOCKET_ERROR_OPTION, &socket_error, &length);
		xsdk_log_write(
			"net:   connect didn't complete within %u ms; SO_ERROR %d", (unsigned)timeout_ms, (int)socket_error);
		return false;
	}
	return true;
}

// Receives what arrives within the timeout, NUL-terminated; returns its length, or -1.
static int receive_text(int32_t socket, char *buffer, int size, uint32_t timeout_ms)
{
	if (!wait_socket(socket, false, timeout_ms)) {
		xsdk_log_write("net:   nothing arrived in %u ms", (unsigned)timeout_ms);
		return -1;
	}
	const int received = NetDll_recv(XNCALLER_TITLE, socket, buffer, size - 1, 0);
	if (received < 0) {
		xsdk_log_write("net:   recv failed: %d", NetDll_WSAGetLastError());
		return -1;
	}
	buffer[received] = 0;
	return received;
}

static bool send_text(int32_t socket, const char *text)
{
	const int length = (int)strlen(text);
	const int sent = NetDll_send(XNCALLER_TITLE, socket, text, length, 0);
	if (sent != length) {
		xsdk_log_write("net:   send gave %d of %d: %d", sent, length, NetDll_WSAGetLastError());
		return false;
	}
	return true;
}

// A marked connection to the PC with line sent, or NET_INVALID_SOCKET.
static int32_t connect_with(const Probe *probe, const char *line)
{
	const int32_t socket = open_socket(NET_STREAM, 1);
	if (socket == NET_INVALID_SOCKET)
		return socket;
	if (!connect_socket(socket, probe->pc, probe->port, CONNECT_TIMEOUT_MS) || !send_text(socket, line)) {
		(void)NetDll_closesocket(XNCALLER_TITLE, socket);
		return NET_INVALID_SOCKET;
	}
	return socket;
}

// Sends line to the PC on a connection of its own and takes the answer.
static bool ask_pc(const Probe *probe, const char *line, char *reply, int size)
{
	const int32_t socket = connect_with(probe, line);
	const bool answered = socket != NET_INVALID_SOCKET && receive_text(socket, reply, size, LONG_HOLD_MS) > 0;
	if (socket != NET_INVALID_SOCKET)
		(void)NetDll_closesocket(XNCALLER_TITLE, socket);
	if (!answered)
		strcpy(reply, "(no answer)");
	reply[strcspn(reply, "\r\n")] = 0;
	return answered;
}

static void log_option(int32_t socket, int32_t name, const char *what)
{
	int32_t value = 0;
	int32_t length = sizeof value;
	const int32_t result = NetDll_getsockopt(XNCALLER_TITLE, socket, NET_SOCKET_LEVEL, name, &value, &length);
	xsdk_log_write("net:   %s (0x%04X) reads %d, %d bytes: %d, error %d", what, (unsigned)name, (int)value, (int)length,
		(int)result, result ? NetDll_WSAGetLastError() : 0);
}

static void set_option(int32_t socket, int32_t name, const void *value, int32_t length, const char *what)
{
	const int32_t result = NetDll_setsockopt(XNCALLER_TITLE, socket, NET_SOCKET_LEVEL, name, value, length);
	xsdk_log_write("net:   setsockopt %s (0x%04X), %d bytes: %d, error %d", what, (unsigned)name, (int)length,
		(int)result, result ? NetDll_WSAGetLastError() : 0);
}

static void test_tcp_echo(int mode, uint32_t pc, uint16_t port)
{
	const int32_t socket = open_socket(NET_STREAM, mode);
	bool echoed = false;
	if (socket != NET_INVALID_SOCKET && connect_socket(socket, pc, port, CONNECT_TIMEOUT_MS)) {
		char line[LINE_BYTES];
		snprintf(line, sizeof line, "tcp echo, %s\n", mode_names[mode]);
		char reply[LINE_BYTES];
		echoed = send_text(socket, line) && receive_text(socket, reply, sizeof reply, RECEIVE_TIMEOUT_MS) > 0 &&
			strcmp(reply, line) == 0;
	}
	if (socket != NET_INVALID_SOCKET)
		(void)NetDll_closesocket(XNCALLER_TITLE, socket);
	xsdk_log_write("net: %s: TCP to the PC and back: %s", mode_names[mode], echoed ? "yes" : "no");
}

static void test_udp_echo(int mode, uint32_t pc, uint16_t port)
{
	const int32_t socket = open_socket(NET_DATAGRAM, mode);
	bool echoed = false;
	if (socket != NET_INVALID_SOCKET) {
		char line[LINE_BYTES];
		snprintf(line, sizeof line, "udp echo, %s\n", mode_names[mode]);
		const NetAddress to = net_address(pc, port);
		const int length = (int)strlen(line);
		const int sent = NetDll_sendto(XNCALLER_TITLE, socket, line, length, 0, &to, sizeof to);
		if (sent != length)
			xsdk_log_write("net:   sendto gave %d of %d: %d", sent, length, NetDll_WSAGetLastError());
		else if (wait_socket(socket, false, RECEIVE_TIMEOUT_MS)) {
			char reply[LINE_BYTES];
			NetAddress from;
			int32_t from_length = sizeof from;
			const int received =
				NetDll_recvfrom(XNCALLER_TITLE, socket, reply, sizeof reply - 1, 0, &from, &from_length);
			if (received >= 0) {
				reply[received] = 0;
				echoed = strcmp(reply, line) == 0;
			} else
				xsdk_log_write("net:   recvfrom failed: %d", NetDll_WSAGetLastError());
		} else
			xsdk_log_write("net:   no datagram came back in %u ms", (unsigned)RECEIVE_TIMEOUT_MS);
		(void)NetDll_closesocket(XNCALLER_TITLE, socket);
	}
	xsdk_log_write("net: %s: UDP to the PC and back: %s", mode_names[mode], echoed ? "yes" : "no");
}

// Listens, asks the PC over TCP to connect back, and takes its line.
static void test_accept(int mode, uint32_t pc, uint16_t port)
{
	const uint16_t listen_port = (uint16_t)(CONSOLE_LISTEN_PORT + mode);
	const int32_t listener = open_socket(NET_STREAM, mode);
	bool accepted = false;
	if (listener != NET_INVALID_SOCKET) {
		const NetAddress any = net_address(0, listen_port);
		if (NetDll_bind(XNCALLER_TITLE, listener, &any, sizeof any) != 0)
			xsdk_log_write("net:   bind failed: %d", NetDll_WSAGetLastError());
		else if (NetDll_listen(XNCALLER_TITLE, listener, 1) != 0)
			xsdk_log_write("net:   listen failed: %d", NetDll_WSAGetLastError());
		else {
			const int32_t asker = open_socket(NET_STREAM, mode);
			char request[LINE_BYTES];
			snprintf(request, sizeof request, "back %u\n", (unsigned)listen_port);
			char reply[LINE_BYTES];
			const bool asked = asker != NET_INVALID_SOCKET && connect_socket(asker, pc, port, CONNECT_TIMEOUT_MS) &&
				send_text(asker, request) && receive_text(asker, reply, sizeof reply, RECEIVE_TIMEOUT_MS) > 0;
			if (asker != NET_INVALID_SOCKET)
				(void)NetDll_closesocket(XNCALLER_TITLE, asker);
			if (!asked)
				xsdk_log_write("net:   couldn't ask the PC to connect back");
			else if (wait_socket(listener, false, ACCEPT_TIMEOUT_MS)) {
				NetAddress from;
				int32_t from_length = sizeof from;
				const int32_t connection = NetDll_accept(XNCALLER_TITLE, listener, &from, &from_length);
				if (connection == NET_INVALID_SOCKET)
					xsdk_log_write("net:   accept failed: %d", NetDll_WSAGetLastError());
				else {
					char line[LINE_BYTES];
					accepted = receive_text(connection, line, sizeof line, RECEIVE_TIMEOUT_MS) > 0 &&
						strncmp(line, "hello from the PC", 17) == 0;
					(void)NetDll_closesocket(XNCALLER_TITLE, connection);
				}
			} else
				xsdk_log_write("net:   no connection came in %u ms", (unsigned)ACCEPT_TIMEOUT_MS);
		}
		(void)NetDll_closesocket(XNCALLER_TITLE, listener);
	}
	xsdk_log_write("net: %s: a connection from the PC, accepted: %s", mode_names[mode], accepted ? "yes" : "no");
}

static void test_public_connect(int mode, uint32_t address, uint16_t port, const char *what)
{
	const int32_t socket = open_socket(NET_STREAM, mode);
	const bool connected =
		socket != NET_INVALID_SOCKET && connect_socket(socket, address, port, PUBLIC_CONNECT_TIMEOUT_MS);
	if (socket != NET_INVALID_SOCKET)
		(void)NetDll_closesocket(XNCALLER_TITLE, socket);
	xsdk_log_write("net: %s: TCP to %s: %s", mode_names[mode], what, connected ? "connected" : "no");
}

// An HTTP/1.0 request to a public host on port 80; logs the reply's status line.
static void test_http(int mode, uint32_t address, const char *host)
{
	const int32_t socket = open_socket(NET_STREAM, mode);
	char reply[LINE_BYTES] = "";
	bool answered = false;
	if (socket != NET_INVALID_SOCKET && connect_socket(socket, address, HTTP_PORT, PUBLIC_CONNECT_TIMEOUT_MS)) {
		char request[LINE_BYTES];
		snprintf(request, sizeof request, "GET / HTTP/1.0\r\nHost: %s\r\n\r\n", host);
		answered = send_text(socket, request) && receive_text(socket, reply, sizeof reply, RECEIVE_TIMEOUT_MS) > 0;
		char *end = strpbrk(reply, "\r\n");
		if (end)
			*end = 0;
	}
	if (socket != NET_INVALID_SOCKET)
		(void)NetDll_closesocket(XNCALLER_TITLE, socket);
	xsdk_log_write("net: %s: HTTP from %s: %s", mode_names[mode], host, answered ? reply : "no reply");
}

static bool look_up(const char *name, uint32_t *address)
{
	XNDNS *result = NULL;
	const int status = NetDll_XNetDnsLookup(XNCALLER_TITLE, name, 0, &result);
	if (status != 0 || !result) {
		xsdk_log_write("net: DNS lookup of %s failed to start: %d", name, status);
		return false;
	}
	uint32_t waited = 0;
	while (result->status == NET_IN_PROGRESS && waited < DNS_TIMEOUT_MS) {
		(void)xsdk_thread_sleep(POLL_MS);
		waited += POLL_MS;
	}
	const bool found = result->status == 0 && result->count > 0;
	if (found) {
		*address = result->addresses[0];
		char text[16];
		address_text(*address, text);
		xsdk_log_write(
			"net: DNS: %s is %s, %u addresses, in about %u ms", name, text, (unsigned)result->count, (unsigned)waited);
	} else
		xsdk_log_write("net: DNS: %s not found: status %d, %u addresses, after %u ms", name, (int)result->status,
			(unsigned)result->count, (unsigned)waited);
	(void)NetDll_XNetDnsRelease(XNCALLER_TITLE, result);
	return found;
}

static void log_title_address(void)
{
	XNADDR address;
	uint32_t status = ADDRESS_PENDING;
	uint32_t waited = 0;
	while (waited < ADDRESS_TIMEOUT_MS) {
		memset(&address, 0, sizeof address);
		status = NetDll_XNetGetTitleXnAddr(XNCALLER_TITLE, &address);
		if (status != ADDRESS_PENDING)
			break;
		(void)xsdk_thread_sleep(POLL_MS);
		waited += POLL_MS;
	}
	char text[16];
	address_text(address.address, text);
	xsdk_log_write("net: title address: status %08X after %u ms, %s", (unsigned)status, (unsigned)waited, text);
}

// The 2026-10-14 probe: what reaches where, on plain sockets and marked ones.
static void run_reach(const Probe *probe)
{
	const uint32_t link = NetDll_XNetGetEthernetLinkStatus(XNCALLER_TITLE);
	xsdk_log_write("net: link status %08X, %s", (unsigned)link, link & LINK_ACTIVE ? "active" : "not active");
	log_title_address();
	uint32_t public_address = 0;
	(void)parse_address("1.1.1.1", &public_address);
	for (int mode = 0; mode < 2; mode++) {
		test_tcp_echo(mode, probe->pc, probe->port);
		test_udp_echo(mode, probe->pc, probe->port);
		test_accept(mode, probe->pc, probe->port);
		test_public_connect(mode, public_address, HTTPS_PORT, "1.1.1.1:443");
	}
	uint32_t found = 0;
	if (look_up("example.com", &found)) {
		for (int mode = 0; mode < 2; mode++)
			test_http(mode, found, "example.com");
	}
}

// The link and the address from the network's start, each change logged, till the address isn't pending.
static void run_address(const Probe *probe)
{
	uint32_t last_link = UINT32_MAX;
	uint32_t last_status = UINT32_MAX;
	uint32_t last_address = UINT32_MAX;
	const int64_t start = now_us();
	for (;;) {
		XNADDR address;
		memset(&address, 0, sizeof address);
		const uint32_t status = NetDll_XNetGetTitleXnAddr(XNCALLER_TITLE, &address);
		const uint32_t link = NetDll_XNetGetEthernetLinkStatus(XNCALLER_TITLE);
		if (link != last_link || status != last_status || address.address != last_address) {
			char text[16];
			address_text(address.address, text);
			xsdk_log_write("net: address: %u ms after XNetStartup: link %08X, address status %08X, %s",
				(unsigned)((now_us() - probe->started_us) / 1000), (unsigned)link, (unsigned)status, text);
			last_link = link;
			last_status = status;
			last_address = address.address;
		}
		if (status != ADDRESS_PENDING || ms_since(start) >= ADDRESS_TIMEOUT_MS)
			break;
		(void)xsdk_thread_sleep(ADDRESS_POLL_MS);
	}
}

static void log_startup_settings(const char *when)
{
	uint8_t settings[32] = { 0 };
	uint32_t size = sizeof settings;
	const int32_t result = NetDll_XNetGetOpt(XNCALLER_TITLE, OPTION_STARTUP, settings, &size);
	char hex[3 * sizeof settings + 1] = "";
	for (uint32_t i = 0; i < size && i < sizeof settings; i++)
		snprintf(hex + 3 * i, 4, "%02X ", settings[i]);
	xsdk_log_write("net: XNetGetOpt(1) %s: %d, %u bytes: %s", when, (int)result, (unsigned)size, hex);
	if (result == 0 && size >= STARTUP_PARAMS_BYTES)
		xsdk_log_write("net:   flags %u, datagram sockets %u, stream sockets %u, receive %u KB, send %u KB",
			settings[STARTUP_FLAGS], settings[STARTUP_DATAGRAM_SOCKETS], settings[STARTUP_STREAM_SOCKETS],
			settings[STARTUP_RECEIVE_KB], settings[STARTUP_SEND_KB]);
}

static void run_settings(const Probe *probe)
{
	(void)probe;
	log_startup_settings("after the start");
}

// Opens sockets of the type until one fails or MAX_SOCKETS are open; then closes them.
static void count_sockets(int type, const char *what)
{
	int opened = 0;
	int error = 0;
	for (; opened < MAX_SOCKETS; opened++) {
		held_sockets[opened] = NetDll_socket(XNCALLER_TITLE, NET_INET, type, type == NET_STREAM ? NET_TCP : NET_UDP);
		if (held_sockets[opened] == NET_INVALID_SOCKET) {
			error = NetDll_WSAGetLastError();
			break;
		}
	}
	xsdk_log_write("net: %s sockets open at once: %d%s, error %d", what, opened,
		opened == MAX_SOCKETS ? " (the probe's most)" : "", error);
	for (int i = 0; i < opened; i++)
		(void)NetDll_closesocket(XNCALLER_TITLE, held_sockets[i]);
}

// Connects to the PC until a connection fails or MAX_SOCKETS are open; then closes them.
static void count_connections(const Probe *probe)
{
	int opened = 0;
	for (; opened < MAX_SOCKETS; opened++) {
		held_sockets[opened] = open_socket(NET_STREAM, 1);
		if (held_sockets[opened] == NET_INVALID_SOCKET)
			break;
		if (!connect_socket(held_sockets[opened], probe->pc, probe->port, CONNECT_TIMEOUT_MS)) {
			(void)NetDll_closesocket(XNCALLER_TITLE, held_sockets[opened]);
			break;
		}
	}
	xsdk_log_write("net: TCP connections to the PC open at once: %d%s", opened,
		opened == MAX_SOCKETS ? " (the probe's most)" : "");
	for (int i = 0; i < opened; i++)
		(void)NetDll_closesocket(XNCALLER_TITLE, held_sockets[i]);
}

static void log_buffers(const char *when)
{
	const int32_t socket = open_socket(NET_STREAM, 1);
	if (socket == NET_INVALID_SOCKET)
		return;
	xsdk_log_write("net: buffers %s:", when);
	log_option(socket, NET_RECEIVE_BUFFER, "SO_RCVBUF");
	log_option(socket, NET_SEND_BUFFER, "SO_SNDBUF");
	(void)NetDll_closesocket(XNCALLER_TITLE, socket);
}

static void run_limits(const Probe *probe)
{
	count_sockets(NET_STREAM, "TCP");
	count_sockets(NET_DATAGRAM, "UDP");
	count_connections(probe);
	log_buffers("on a new socket");
	const int32_t socket = open_socket(NET_STREAM, 1);
	if (socket == NET_INVALID_SOCKET)
		return;
	static const int32_t sizes[] = { 65536, 262144 };
	for (uint32_t i = 0; i < sizeof sizes / sizeof sizes[0]; i++) {
		set_option(socket, NET_RECEIVE_BUFFER, &sizes[i], sizeof sizes[i], "SO_RCVBUF");
		log_option(socket, NET_RECEIVE_BUFFER, "SO_RCVBUF");
		set_option(socket, NET_SEND_BUFFER, &sizes[i], sizeof sizes[i], "SO_SNDBUF");
		log_option(socket, NET_SEND_BUFFER, "SO_SNDBUF");
	}
	(void)NetDll_closesocket(XNCALLER_TITLE, socket);
}

// Stops the network and starts it again with restart's settings.
static void run_restart(const Probe *probe)
{
	log_buffers("before the restart");
	xsdk_log_write("net: restart: WSACleanup %d", (int)NetDll_WSACleanup(XNCALLER_TITLE));
	xsdk_log_write("net: restart: XNetCleanup %d", (int)NetDll_XNetCleanup(XNCALLER_TITLE, NULL));
	xsdk_log_write("net: restart: XNetStartup, stream sockets %u, datagram %u, receive %u KB, send %u KB: %d",
		probe->restart[STARTUP_STREAM_SOCKETS], probe->restart[STARTUP_DATAGRAM_SOCKETS],
		probe->restart[STARTUP_RECEIVE_KB], probe->restart[STARTUP_SEND_KB],
		(int)NetDll_XNetStartup(XNCALLER_TITLE, probe->restart));
	static uint8_t wsa_data[WSADATA_BYTES];
	xsdk_log_write("net: restart: WSAStartup %d", (int)NetDll_WSAStartup(XNCALLER_TITLE, NET_VERSION, wsa_data));
	log_startup_settings("after the restart");
	log_buffers("after the restart");
	count_sockets(NET_STREAM, "TCP, after the restart,");
	test_tcp_echo(1, probe->pc, probe->port);
}

// A blocking recv with a receive timeout, from a connection the PC holds silent.
static void run_receive_timeout(const Probe *probe)
{
	char line[LINE_BYTES];
	snprintf(line, sizeof line, "hold %d\n", HOLD_MS);
	const int32_t socket = connect_with(probe, line);
	if (socket == NET_INVALID_SOCKET)
		return;
	set_blocking(socket, true);
	const uint32_t timeout_ms = SOCKET_TIMEOUT_MS;
	set_option(socket, NET_RECEIVE_TIMEOUT, &timeout_ms, sizeof timeout_ms, "SO_RCVTIMEO, as milliseconds");
	log_option(socket, NET_RECEIVE_TIMEOUT, "SO_RCVTIMEO");
	for (int i = 0; i < 2; i++) {
		char byte;
		const int64_t start = now_us();
		const int32_t received = NetDll_recv(XNCALLER_TITLE, socket, &byte, 1, 0);
		xsdk_log_write("net: receive timeout: blocking recv %d returned %d after %u ms, error %d", i + 1, (int)received,
			ms_since(start), received < 0 ? NetDll_WSAGetLastError() : 0);
	}
	(void)NetDll_closesocket(XNCALLER_TITLE, socket);
}

// Blocking sends with a send timeout into a connection the PC holds unread, till one waits.
static void run_send_timeout(const Probe *probe)
{
	char line[LINE_BYTES];
	snprintf(line, sizeof line, "hold %d\n", LONG_HOLD_MS);
	const int32_t socket = connect_with(probe, line);
	if (socket == NET_INVALID_SOCKET)
		return;
	set_blocking(socket, true);
	const uint32_t timeout_ms = SOCKET_TIMEOUT_MS;
	set_option(socket, NET_SEND_TIMEOUT, &timeout_ms, sizeof timeout_ms, "SO_SNDTIMEO, as milliseconds");
	log_option(socket, NET_SEND_TIMEOUT, "SO_SNDTIMEO");
	uint32_t total = 0;
	for (int i = 0; i < MAX_CHUNKS; i++) {
		const int64_t start = now_us();
		const int32_t sent = NetDll_send(XNCALLER_TITLE, socket, chunk, CHUNK_BYTES, 0);
		const unsigned took_ms = ms_since(start);
		if (sent != CHUNK_BYTES || took_ms >= SLOW_SEND_MS) {
			xsdk_log_write("net: send timeout: send %d, after %u bytes, returned %d after %u ms, error %d", i + 1,
				(unsigned)total, (int)sent, took_ms, sent < 0 ? NetDll_WSAGetLastError() : 0);
			break;
		}
		total += (uint32_t)sent;
	}
	const int64_t start = now_us();
	const int32_t sent = NetDll_send(XNCALLER_TITLE, socket, chunk, CHUNK_BYTES, 0);
	xsdk_log_write("net: send timeout: the send after returned %d after %u ms, error %d; %u bytes before", (int)sent,
		ms_since(start), sent < 0 ? NetDll_WSAGetLastError() : 0, (unsigned)total);
	(void)NetDll_closesocket(XNCALLER_TITLE, socket);
}

// Closes a connection the PC holds, with linger set as given unless length is 0, and asks the PC how it ended.
static void close_held(const Probe *probe, const void *linger, int32_t length, const char *what)
{
	char line[LINE_BYTES];
	snprintf(line, sizeof line, "hold %d\n", HOLD_MS);
	const int32_t socket = connect_with(probe, line);
	if (socket == NET_INVALID_SOCKET)
		return;
	// So the PC has read the line and holds the connection before anything ends it.
	(void)xsdk_thread_sleep(CLOSE_SETTLE_MS);
	if (length) {
		set_option(socket, NET_LINGER, linger, length, what);
		log_option(socket, NET_LINGER, "SO_LINGER");
	}
	const int64_t start = now_us();
	const int32_t closed = NetDll_closesocket(XNCALLER_TITLE, socket);
	const unsigned took_ms = ms_since(start);
	char reply[LINE_BYTES];
	(void)ask_pc(probe, "last\n", reply, sizeof reply);
	xsdk_log_write("net: linger: %s: closesocket %d after %u ms; the PC saw %s", what, (int)closed, took_ms, reply);
}

// Linger's layout, and what a close does with it set: Winsock's two 16-bit fields, then POSIX's two ints.
static void run_linger(const Probe *probe)
{
	const int32_t socket = open_socket(NET_STREAM, 1);
	if (socket != NET_INVALID_SOCKET) {
		for (int32_t asked = 4; asked <= 8; asked += 4) {
			uint8_t value[8] = { 0 };
			int32_t length = asked;
			const int32_t result =
				NetDll_getsockopt(XNCALLER_TITLE, socket, NET_SOCKET_LEVEL, NET_LINGER, value, &length);
			xsdk_log_write(
				"net: linger: getsockopt with %d bytes: %d, %d bytes: %02X %02X %02X %02X %02X %02X %02X "
				"%02X",
				(int)asked, (int)result, (int)length, value[0], value[1], value[2], value[3], value[4], value[5],
				value[6], value[7]);
		}
		(void)NetDll_closesocket(XNCALLER_TITLE, socket);
	}
	close_held(probe, NULL, 0, "no linger set");
	const uint16_t abortive[2] = { 1, 0 };
	close_held(probe, abortive, sizeof abortive, "Winsock's {1, 0}");
	const int32_t posix_abortive[2] = { 1, 0 };
	close_held(probe, posix_abortive, sizeof posix_abortive, "POSIX's {1, 0}");

	// With data the PC hasn't read, a close lingering 2 s.
	char line[LINE_BYTES];
	snprintf(line, sizeof line, "hold %d\n", HOLD_MS);
	const int32_t full = connect_with(probe, line);
	if (full == NET_INVALID_SOCKET)
		return;
	uint32_t queued = 0;
	for (int i = 0; i < MAX_CHUNKS && NetDll_send(XNCALLER_TITLE, full, chunk, CHUNK_BYTES, 0) > 0; i++)
		queued += CHUNK_BYTES;
	const uint16_t lingering[2] = { 1, LINGER_SECONDS };
	set_option(full, NET_LINGER, lingering, sizeof lingering, "Winsock's {1, 2}");
	set_blocking(full, true);
	const int64_t start = now_us();
	const int32_t closed = NetDll_closesocket(XNCALLER_TITLE, full);
	xsdk_log_write("net: linger: {1, 2} with about %u bytes unread by the PC: closesocket %d after %u ms",
		(unsigned)queued, (int)closed, ms_since(start));
	(void)xsdk_thread_sleep(CLOSE_SETTLE_MS);
}

// A connection from the title to its own listener over the loopback address, as curl's wake-up makes one
// (curl:lib/socketpair.c, wakeup_inet): bound to 127.0.0.1 port 0, the port from getsockname.
static void try_loopback(int mode, uint32_t bind_address, uint16_t bind_port, uint32_t to_address)
{
	char bound[16];
	char to[16];
	address_text(bind_address, bound);
	address_text(to_address, to);
	const int32_t listener = open_socket(NET_STREAM, mode);
	if (listener == NET_INVALID_SOCKET)
		return;
	const NetAddress at = net_address(bind_address, bind_port);
	NetAddress got = { 0 };
	int32_t got_length = sizeof got;
	const bool listening = NetDll_bind(XNCALLER_TITLE, listener, &at, sizeof at) == 0 &&
		NetDll_listen(XNCALLER_TITLE, listener, 1) == 0 &&
		NetDll_getsockname(XNCALLER_TITLE, listener, &got, &got_length) == 0;
	const int error = listening ? 0 : NetDll_WSAGetLastError();
	const uint16_t port = got.port ? got.port : bind_port;
	bool connected = false;
	bool accepted = false;
	bool through = false;
	const int32_t client = listening ? open_socket(NET_STREAM, mode) : NET_INVALID_SOCKET;
	if (client != NET_INVALID_SOCKET) {
		connected = connect_socket(client, to_address, port, CONNECT_TIMEOUT_MS);
		if (connected && wait_socket(listener, false, ACCEPT_TIMEOUT_MS)) {
			// XAM wants somewhere to put the address (src/socket.c, accept).
			NetAddress from;
			int32_t from_length = sizeof from;
			const int32_t server = NetDll_accept(XNCALLER_TITLE, listener, &from, &from_length);
			accepted = server != NET_INVALID_SOCKET;
			char reply[LINE_BYTES];
			through = accepted && send_text(client, "loop\n") &&
				receive_text(server, reply, sizeof reply, RECEIVE_TIMEOUT_MS) > 0 && strcmp(reply, "loop\n") == 0;
			if (accepted)
				(void)NetDll_closesocket(XNCALLER_TITLE, server);
		}
		(void)NetDll_closesocket(XNCALLER_TITLE, client);
	}
	(void)NetDll_closesocket(XNCALLER_TITLE, listener);
	xsdk_log_write(
		"net: loopback: %s: bound to %s:%u, listening %s (error %d), port %u; connected to %s %s, "
		"accepted %s, a line through %s",
		mode_names[mode], bound, (unsigned)bind_port, listening ? "yes" : "no", error, (unsigned)port, to,
		connected ? "yes" : "no", accepted ? "yes" : "no", through ? "yes" : "no");
}

static void run_loopback(const Probe *probe)
{
	(void)probe;
	XNADDR own;
	memset(&own, 0, sizeof own);
	(void)NetDll_XNetGetTitleXnAddr(XNCALLER_TITLE, &own);
	for (int mode = 0; mode < 2; mode++) {
		try_loopback(mode, LOCALHOST, 0, LOCALHOST);
		try_loopback(mode, 0, (uint16_t)(LOOPBACK_PORT + mode), LOCALHOST);
		try_loopback(mode, 0, (uint16_t)(LOOPBACK_PORT + 2 + mode), own.address);
	}
}

static const ProbeCase cases[] = {
	{ "reach", NULL, run_reach },
	{ "address", NULL, run_address },
	{ "settings", NULL, run_settings },
	{ "limits", NULL, run_limits },
	{ "restart", "WSACleanup, XNetCleanup and a second XNetStartup may hang the title or leave it no network",
		run_restart },
	{ "receive_timeout", NULL, run_receive_timeout },
	{ "send_timeout", NULL, run_send_timeout },
	{ "linger", NULL, run_linger },
	{ "loopback", NULL, run_loopback },
};

enum { CASE_COUNT = sizeof cases / sizeof cases[0] };

static bool parse_settings(const char *text, uint8_t params[STARTUP_PARAMS_BYTES])
{
	unsigned stream, datagram, receive_kb, send_kb;
	if (sscanf(text, "%u,%u,%u,%u", &stream, &datagram, &receive_kb, &send_kb) != 4 || stream > UINT8_MAX ||
		datagram > UINT8_MAX || receive_kb > UINT8_MAX || send_kb > UINT8_MAX)
		return false;
	params[STARTUP_STREAM_SOCKETS] = (uint8_t)stream;
	params[STARTUP_DATAGRAM_SOCKETS] = (uint8_t)datagram;
	params[STARTUP_RECEIVE_KB] = (uint8_t)receive_kb;
	params[STARTUP_SEND_KB] = (uint8_t)send_kb;
	return true;
}

// The cases probe_net_start.txt names, in order, and the settings it gives; without it, or naming none, reach.
static uint32_t read_start(Probe *probe, const ProbeCase *chosen[MAX_RUN])
{
	uint32_t count = 0;
	FILE *const file = fopen("probe_net_start.txt", "r");
	char word[CASE_NAME_BYTES * 2];
	while (file && count < MAX_RUN && fscanf(file, "%63s", word) == 1) {
		if (strncmp(word, "startup=", 8) == 0 || strncmp(word, "restart=", 8) == 0) {
			uint8_t *params = word[0] == 's' ? probe->startup : probe->restart;
			if (!parse_settings(word + 8, params))
				xsdk_log_write("net: probe_net_start.txt: '%s' isn't <stream>,<datagram>,<receive KB>,<send KB>", word);
			continue;
		}
		uint32_t i = 0;
		while (i < CASE_COUNT && strcmp(word, cases[i].name) != 0)
			i++;
		if (i < CASE_COUNT)
			chosen[count++] = &cases[i];
		else
			xsdk_log_write("net: probe_net_start.txt names no case '%s'", word);
	}
	if (file)
		(void)fclose(file);
	if (count == 0)
		chosen[count++] = &cases[0];
	return count;
}

int main(void)
{
	Probe probe = { .startup = { STARTUP_PARAMS_BYTES }, .restart = { STARTUP_PARAMS_BYTES, 0, 16, 64, 64, 64 } };
	const ProbeCase *chosen[MAX_RUN];
	const uint32_t count = read_start(&probe, chosen);
	char risks[LINE_BYTES * 2] = "";
	for (uint32_t i = 0; i < count; i++) {
		if (chosen[i]->risk)
			snprintf(risks + strlen(risks), sizeof risks - strlen(risks), " %s: %s.", chosen[i]->name, chosen[i]->risk);
	}
	xsdk_log_write("net probe: %u cases.%s", (unsigned)count, risks[0] ? risks : " None should hang.");
	unsigned port = 0;
	unsigned flags = 0;
	FILE *host = fopen("probe_net_host.txt", "r");
	if (!host || fscanf(host, "%15s %u %u", probe.pc_text, &port, &flags) != 3 ||
		!parse_address(probe.pc_text, &probe.pc)) {
		xsdk_log_write("net: probe_net_host.txt beside the .xex must hold \"<PC address> <port> <XNetStartup flags>\"");
		return 1;
	}
	fclose(host);
	FILE *const net_log = fopen("net_log.txt", "r");
	if (net_log) {
		fclose(net_log);
		xsdk_log_write(
			"net: net_log.txt is beside the .xex, so xsdk started the network before main, and the "
			"XNetStartup below comes second; take it away for the startup cases");
	}
	probe.port = (uint16_t)port;
	probe.startup[STARTUP_FLAGS] = (uint8_t)flags;
	probe.restart[STARTUP_FLAGS] = (uint8_t)flags;
	memset(chunk, 0x5A, sizeof chunk);

	const int32_t started = NetDll_XNetStartup(XNCALLER_TITLE, probe.startup);
	probe.started_us = now_us();
	xsdk_log_write("net: XNetStartup, flags %u, stream sockets %u, datagram %u, receive %u KB, send %u KB: %d", flags,
		probe.startup[STARTUP_STREAM_SOCKETS], probe.startup[STARTUP_DATAGRAM_SOCKETS],
		probe.startup[STARTUP_RECEIVE_KB], probe.startup[STARTUP_SEND_KB], (int)started);
	static uint8_t wsa_data[WSADATA_BYTES];
	xsdk_log_write("net: WSAStartup 2.2: %d", (int)NetDll_WSAStartup(XNCALLER_TITLE, NET_VERSION, wsa_data));
	xsdk_log_write("net: the PC at %s, port %u", probe.pc_text, port);
	for (uint32_t i = 0; i < count; i++) {
		const int64_t start = now_us();
		xsdk_log_write("net probe: case %s", chosen[i]->name);
		chosen[i]->run(&probe);
		xsdk_log_write("net probe: case %s done in %u ms", chosen[i]->name, ms_since(start));
	}
	xsdk_log_write("net probe: done");
	return 0;
}
