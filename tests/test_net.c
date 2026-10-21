#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <poll.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <unistd.h>

#include "tests.h"
#include "xsdk/log.h"
#include "xsdk/net.h"
#include "xsdk/thread.h"

// Sockets against tools/net_echo.py on the PC, whose address and port net_host.txt beside the .xex holds, as
// "192.168.0.125 7370" (docs/scope/networking.md, step 2).

enum {
	TIMEOUT_MS = 3000,
	ACCEPT_TIMEOUT_MS = 5000,
	LINE_BYTES = 128,
	LISTEN_PORT = 7373,
	// Nothing listens there on the console.
	CLOSED_PORT = 7379,
	// Far more than XAM holds at once, opened and closed in turn.
	SOCKETS_IN_TURN = 1000,
	// XAM's own, and the most buffer it takes (docs/HARDWARE.md, "XAM's sockets and loader, milestone 56").
	DEFAULT_STREAM_SOCKETS = 32,
	RAISED_BUFFER_KB = 63,
	BYTES_PER_KB = 1024,
	// How long net_echo.py holds each connection, and the timeouts, well inside the holds.
	RECEIVE_HOLD_MS = 1000,
	SEND_HOLD_MS = 1500,
	LINGER_HOLD_MS = 500,
	SOCKET_TIMEOUT_MS = 200,
	TIMED_OUT_WITHIN_MS = 300,
	// Time for net_echo.py to read "hold" before the connection closes.
	SETTLE_MS = 200,
	CHUNK_BYTES = 16384,
	// Far more than the PC's window and the console's buffer hold.
	MAX_SEND_BYTES = 8 * 1024 * 1024,
	MILLISECONDS_PER_SECOND = 1000,
	MICROSECONDS_PER_MILLISECOND = 1000,
	NANOSECONDS_PER_MILLISECOND = 1000000,
};

// xsdk_test raises XAM's limits, as a title that wants more does (docs/GUIDE.md, "Sockets").
const XsdkNetworkLimits xsdk_network_limits = {
	.stream_sockets = 64,
	.datagram_sockets = 16,
	.receive_kb = RAISED_BUFFER_KB,
	.send_kb = RAISED_BUFFER_KB,
};

static char chunk[CHUNK_BYTES];

typedef struct {
	struct sockaddr_in address;
	char text[INET_ADDRSTRLEN];
} Host;

static struct sockaddr_in socket_address(in_addr_t address, in_port_t port)
{
	return (struct sockaddr_in){ .sin_family = AF_INET, .sin_port = htons(port), .sin_addr = { htonl(address) } };
}

static bool read_host(Host *pc)
{
	FILE *file = fopen("net_host.txt", "r");
	unsigned port = 0;
	in_addr_t address = 0;
	const bool read =
		file && fscanf(file, "%15s %u", pc->text, &port) == 2 && inet_pton(AF_INET, pc->text, &address) == 1;
	if (file)
		fclose(file);
	if (read)
		pc->address = socket_address(ntohl(address), (in_port_t)port);
	else
		xsdk_log_write("net: net_host.txt beside the .xex must hold \"<PC address> <port>\"");
	return read;
}

static bool wait_for(int fd, short events, int timeout_ms)
{
	struct pollfd wanted = { .fd = fd, .events = events };
	const int ready = poll(&wanted, 1, timeout_ms);
	if (ready < 0)
		xsdk_log_write("net: poll failed: %d", errno);
	return ready == 1 && (wanted.revents & events);
}

// Sends a line and returns whether the same comes back.
static bool echoes(int fd, const char *line)
{
	const size_t length = strlen(line);
	if (send(fd, line, length, 0) != (ssize_t)length) {
		xsdk_log_write("net: send failed: %d", errno);
		return false;
	}
	char reply[LINE_BYTES];
	if (!wait_for(fd, POLLIN, TIMEOUT_MS)) {
		xsdk_log_write("net: nothing came back in %d ms", TIMEOUT_MS);
		return false;
	}
	const ssize_t received = recv(fd, reply, sizeof(reply) - 1, 0);
	if (received < 0)
		xsdk_log_write("net: recv failed: %d", errno);
	return received == (ssize_t)length && memcmp(reply, line, length) == 0;
}

// Sends a line and polls for the echo with POLLPRI asked too: data to read comes back as POLLIN alone, as curl takes
// POLLPRI for an error (curl-xenon:lib/select.c, Curl_socket_check).
static bool polls_as_data(int fd, const char *line)
{
	const size_t length = strlen(line);
	struct pollfd wanted = { .fd = fd, .events = POLLIN | POLLPRI };
	const ssize_t sent = send(fd, line, length, 0);
	const int ready = sent == (ssize_t)length ? poll(&wanted, 1, TIMEOUT_MS) : -1;
	char reply[LINE_BYTES];
	// The echo may come in parts; what's ready is enough.
	const ssize_t received = ready == 1 ? recv(fd, reply, sizeof(reply), 0) : -1;
	xsdk_log_write("net: polled for an echo: sent %d, poll %d, revents 0x%04X, recv %d, errno %d", (int)sent, ready,
		(unsigned)wanted.revents, (int)received, errno);
	return received > 0 && wanted.revents == POLLIN;
}

// A blocking connection to the PC, or -1.
static int connect_to(const Host *pc)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	if (fd < 0) {
		xsdk_log_write("net: socket failed: %d", errno);
		return -1;
	}
	if (connect(fd, (const struct sockaddr *)&pc->address, sizeof(pc->address)) != 0) {
		xsdk_log_write("net: connect to %s failed: %d", pc->text, errno);
		close(fd);
		return -1;
	}
	return fd;
}

// Sends net_echo.py a line on a connection of its own and takes its one-line answer.
static bool ask_pc(const Host *pc, const char *line, char reply[LINE_BYTES])
{
	const int fd = connect_to(pc);
	const ssize_t length = (ssize_t)strlen(line);
	const ssize_t received = fd >= 0 && send(fd, line, (size_t)length, 0) == length && wait_for(fd, POLLIN, TIMEOUT_MS)
		? recv(fd, reply, LINE_BYTES - 1, 0)
		: -1;
	if (fd >= 0)
		close(fd);
	if (received <= 0)
		return false;
	reply[received] = 0;
	reply[strcspn(reply, "\r\n")] = 0;
	return true;
}

// The console's own address, as the PC sees it: getsockname gives 0.0.0.0 for a connected socket on the console.
static bool ask_address(const Host *pc, in_addr_t *address)
{
	char reply[LINE_BYTES];
	return ask_pc(pc, "addr\n", reply) && inet_pton(AF_INET, reply, address) == 1;
}

static void test_tcp(const Host *pc, struct sockaddr_in *console)
{
	const int fd = connect_to(pc);
	struct sockaddr_in local = { 0 };
	socklen_t length = sizeof(local);
	char text[INET_ADDRSTRLEN] = "";
	if (fd >= 0 && getsockname(fd, (struct sockaddr *)&local, &length) == 0)
		inet_ntop(AF_INET, &local.sin_addr, text, sizeof(text));
	xsdk_log_write(
		"net: connected to %s; getsockname gives %s, port %u", pc->text, text, (unsigned)ntohs(local.sin_port));
	check("net: TCP to the PC and back, through send and recv", fd >= 0 && echoes(fd, "tcp through send\n"));
	if (fd >= 0)
		close(fd);
	// A connection of its own: net_echo.py echoes one line a connection, then closes it.
	const int polled = connect_to(pc);
	check("net: an echo waiting to be read polls as POLLIN, not POLLPRI",
		polled >= 0 && polls_as_data(polled, "tcp polled\n"));
	if (polled >= 0)
		close(polled);

	in_addr_t address = 0;
	if (ask_address(pc, &address))
		*console = socket_address(ntohl(address), 0);
	inet_ntop(AF_INET, &console->sin_addr, text, sizeof(text));
	xsdk_log_write("net: the PC sees the console at %s", text);

	// The same through the descriptor calls.
	const int second = connect_to(pc);
	struct stat info;
	const char line[] = "tcp through write\n";
	char reply[LINE_BYTES];
	const bool written = second >= 0 && fstat(second, &info) == 0 && S_ISSOCK(info.st_mode) &&
		write(second, line, strlen(line)) == (ssize_t)strlen(line) && wait_for(second, POLLIN, TIMEOUT_MS) &&
		read(second, reply, sizeof(reply)) == (ssize_t)strlen(line) && memcmp(reply, line, strlen(line)) == 0;
	const bool closed = second >= 0 && close(second) == 0 && read(second, reply, 1) < 0 && errno == EBADF;
	check("net: a socket is a descriptor: fstat, write, read and close", written && closed);
}

static void test_udp(const Host *pc)
{
	const int fd = socket(AF_INET, SOCK_DGRAM, 0);
	const char line[] = "udp through sendto\n";
	char reply[LINE_BYTES];
	struct sockaddr_in from;
	socklen_t from_length = sizeof(from);
	const bool echoed = fd >= 0 &&
		sendto(fd, line, strlen(line), 0, (const struct sockaddr *)&pc->address, sizeof(pc->address)) ==
			(ssize_t)strlen(line) &&
		wait_for(fd, POLLIN, TIMEOUT_MS) &&
		recvfrom(fd, reply, sizeof(reply), 0, (struct sockaddr *)&from, &from_length) == (ssize_t)strlen(line) &&
		memcmp(reply, line, strlen(line)) == 0 && from.sin_addr.s_addr == pc->address.sin_addr.s_addr;
	if (fd >= 0)
		close(fd);
	check("net: UDP to the PC and back, through sendto and recvfrom", echoed);
}

// Listens, asks the PC to connect back, and takes its line.
static void test_accept(const Host *pc)
{
	const int listener = socket(AF_INET, SOCK_STREAM, 0);
	const struct sockaddr_in any = socket_address(INADDR_ANY, LISTEN_PORT);
	bool accepted = false;
	if (listener >= 0 && bind(listener, (const struct sockaddr *)&any, sizeof(any)) == 0 && listen(listener, 1) == 0) {
		const int asker = connect_to(pc);
		char request[LINE_BYTES];
		snprintf(request, sizeof(request), "back %d\n", LISTEN_PORT);
		char reply[LINE_BYTES];
		const bool asked = asker >= 0 && send(asker, request, strlen(request), 0) == (ssize_t)strlen(request) &&
			wait_for(asker, POLLIN, TIMEOUT_MS) && recv(asker, reply, sizeof(reply), 0) > 0;
		if (asker >= 0)
			close(asker);
		if (asked && wait_for(listener, POLLIN, ACCEPT_TIMEOUT_MS)) {
			const int connection = accept(listener, NULL, NULL);
			const ssize_t received = connection >= 0 && wait_for(connection, POLLIN, TIMEOUT_MS)
				? recv(connection, reply, sizeof(reply) - 1, 0)
				: -1;
			accepted = received > 0 && strncmp(reply, "hello from the PC", 17) == 0;
			if (connection >= 0)
				close(connection);
		}
	}
	if (listener >= 0)
		close(listener);
	check("net: a connection from the PC, accepted", accepted);
}

static void test_names(void)
{
	const struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
	struct addrinfo *found = NULL;
	const int error = getaddrinfo("example.com", "80", &hints, &found);
	char text[INET_ADDRSTRLEN] = "";
	const struct sockaddr_in *address = found ? (const struct sockaddr_in *)found->ai_addr : NULL;
	if (address)
		inet_ntop(AF_INET, &address->sin_addr, text, sizeof(text));
	xsdk_log_write("net: example.com: %s %s", error ? gai_strerror(error) : "found at", text);
	check("net: getaddrinfo looks a host name up",
		error == 0 && address && address->sin_port == htons(80) && address->sin_addr.s_addr != 0 &&
			found->ai_protocol == IPPROTO_TCP);
	freeaddrinfo(found);

	in_addr_t parsed = 0;
	char round[INET_ADDRSTRLEN] = "";
	const bool converted = inet_pton(AF_INET, "192.168.0.125", &parsed) == 1 && parsed == 0xC0A8007D &&
		inet_ntop(AF_INET, &parsed, round, sizeof(round)) && strcmp(round, "192.168.0.125") == 0 &&
		inet_pton(AF_INET, "256.1.1.1", &parsed) == 0 && inet_pton(AF_INET, "1.2.3", &parsed) == 0;
	check("net: inet_pton and inet_ntop take dotted addresses, and refuse others", converted);
}

// Starts a non-blocking connection and waits for poll to say how it went; gives poll's events and SO_ERROR.
static bool connect_without_blocking(int fd, const struct sockaddr_in *to, short *events, int *error)
{
	const bool set = fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK) == 0 && (fcntl(fd, F_GETFL) & O_NONBLOCK);
	const int connected = set ? connect(fd, (const struct sockaddr *)to, sizeof(*to)) : -1;
	const int connect_error = connected == 0 ? 0 : errno;
	struct pollfd wanted = { .fd = fd, .events = POLLOUT };
	const int ready = set && (connected == 0 || connect_error == EINPROGRESS) ? poll(&wanted, 1, TIMEOUT_MS) : -1;
	socklen_t length = sizeof(*error);
	const bool got = ready == 1 && getsockopt(fd, SOL_SOCKET, SO_ERROR, error, &length) == 0;
	*events = wanted.revents;
	if (!got)
		xsdk_log_write("net: non-blocking connect %d, errno %d; poll %d, revents %04X; SO_ERROR not read: %d",
			connected, connect_error, ready, (unsigned)wanted.revents, errno);
	return got;
}

// A closed port on the console itself: XAM refuses it at once, where the PC's firewall may let a connection time out.
static void test_refused(const struct sockaddr_in *console)
{
	const struct sockaddr_in closed = socket_address(ntohl(console->sin_addr.s_addr), CLOSED_PORT);
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	const int connected = fd >= 0 ? connect(fd, (const struct sockaddr *)&closed, sizeof(closed)) : 0;
	const int connect_error = errno;
	if (fd >= 0)
		close(fd);
	xsdk_log_write(
		"net: a connection to the console's port %d: connect %d, errno %d", CLOSED_PORT, connected, connect_error);
	check(
		"net: a connection to a closed port is refused, ECONNREFUSED", connected < 0 && connect_error == ECONNREFUSED);

	// Without blocking, XAM says only that it failed, which SO_ERROR gives as EIO (src/socket.c).
	const int second = socket(AF_INET, SOCK_STREAM, 0);
	short events = 0;
	int error = 0;
	const bool answered = second >= 0 && connect_without_blocking(second, &closed, &events, &error);
	if (second >= 0)
		close(second);
	xsdk_log_write("net: without blocking: revents %04X, SO_ERROR %d", (unsigned)events, error);
	check("net: a refused connection without blocking gives POLLERR and an SO_ERROR",
		answered && (events & POLLERR) && error == EIO);
}

static void test_non_blocking(const Host *pc)
{
	const int fd = socket(AF_INET, SOCK_STREAM, 0);
	short events = 0;
	int error = -1;
	const bool completed =
		fd >= 0 && connect_without_blocking(fd, &pc->address, &events, &error) && (events & POLLOUT) && error == 0;
	char nothing;
	const ssize_t received = completed ? recv(fd, &nothing, 1, 0) : 0;
	const bool would_block = received < 0 && errno == EAGAIN;
	check("net: a non-blocking connection completes through poll, and recv with nothing there gives EAGAIN",
		completed && would_block && echoes(fd, "tcp without blocking\n"));
	if (fd >= 0)
		close(fd);
}

static void test_link_address(const struct sockaddr_in *console)
{
	uint32_t link = 0;
	struct in_addr address = { 0 };
	const int link_error = xsdk_net_link(&link);
	const int address_error = xsdk_net_address(&address);
	char text[INET_ADDRSTRLEN] = "";
	inet_ntop(AF_INET, &address, text, sizeof(text));
	xsdk_log_write("net: link %02X, error %d; address %s, error %d", (unsigned)link, link_error, text, address_error);
	check("net: the link is active, and xsdk_net_address gives the address the PC sees",
		link_error == 0 && (link & XSDK_NET_LINK_ACTIVE) && address_error == 0 && address.s_addr != 0 &&
			address.s_addr == console->sin_addr.s_addr);
}

static int64_t now_ms(void)
{
	struct timespec now;
	(void)clock_gettime(CLOCK_MONOTONIC, &now);
	return (int64_t)now.tv_sec * MILLISECONDS_PER_SECOND + now.tv_nsec / NANOSECONDS_PER_MILLISECOND;
}

// A connection net_echo.py keeps hold_ms, reading and sending nothing, then closes; -1 if it couldn't be made.
static int held_connection(const Host *pc, int hold_ms)
{
	const int fd = connect_to(pc);
	char line[LINE_BYTES];
	snprintf(line, sizeof(line), "hold %d\n", hold_ms);
	if (fd >= 0 && send(fd, line, strlen(line), 0) != (ssize_t)strlen(line)) {
		xsdk_log_write("net: send failed: %d", errno);
		close(fd);
		return -1;
	}
	return fd;
}

static bool set_timeout(int fd, int name, int timeout_ms)
{
	const struct timeval timeout = { .tv_sec = timeout_ms / MILLISECONDS_PER_SECOND,
		.tv_usec = timeout_ms % MILLISECONDS_PER_SECOND * MICROSECONDS_PER_MILLISECOND };
	return setsockopt(fd, SOL_SOCKET, name, &timeout, sizeof(timeout)) == 0;
}

// Nothing comes, so only the timeout ends the recv.
static void test_receive_timeout(const Host *pc)
{
	const int fd = held_connection(pc, RECEIVE_HOLD_MS);
	char byte;
	const int64_t start = now_ms();
	const ssize_t received = fd >= 0 && set_timeout(fd, SO_RCVTIMEO, SOCKET_TIMEOUT_MS) ? recv(fd, &byte, 1, 0) : 0;
	const int error = errno;
	const int64_t took_ms = now_ms() - start;
	if (fd >= 0)
		close(fd);
	xsdk_log_write("net: recv with a %d ms SO_RCVTIMEO returned %d, errno %d, after %d ms", SOCKET_TIMEOUT_MS,
		(int)received, error, (int)took_ms);
	check("net: a blocking recv with a 200 ms SO_RCVTIMEO fails with EAGAIN within 300 ms",
		received < 0 && error == EAGAIN && took_ms >= SOCKET_TIMEOUT_MS / 2 && took_ms < TIMED_OUT_WITHIN_MS);
}

// The PC reads nothing, so sends fill its window and the console's buffer, then one waits out the timeout.
static void test_send_timeout(const Host *pc)
{
	const int fd = held_connection(pc, SEND_HOLD_MS);
	const bool set = fd >= 0 && set_timeout(fd, SO_SNDTIMEO, SOCKET_TIMEOUT_MS);
	size_t total = 0;
	ssize_t sent = 0;
	int64_t took_ms = 0;
	while (set && total < MAX_SEND_BYTES) {
		const int64_t start = now_ms();
		sent = send(fd, chunk, sizeof(chunk), 0);
		took_ms = now_ms() - start;
		if (sent < 0)
			break;
		total += (size_t)sent;
	}
	const int error = errno;
	if (fd >= 0)
		close(fd);
	xsdk_log_write("net: sends with a %d ms SO_SNDTIMEO: %u bytes went, then %d, errno %d, after %d ms",
		SOCKET_TIMEOUT_MS, (unsigned)total, (int)sent, error, (int)took_ms);
	check("net: a blocking send with SO_SNDTIMEO fails with EAGAIN once the PC's window fills",
		sent < 0 && error == EAGAIN && total > 0 && took_ms < TIMED_OUT_WITHIN_MS);
}

// The options read back as set, and linger {1, 0} makes the close a reset, which net_echo.py's "last" reports.
static void test_options(const Host *pc)
{
	const int fd = held_connection(pc, LINGER_HOLD_MS);
	(void)xsdk_thread_sleep(SETTLE_MS);
	const struct timeval receive = { .tv_sec = 1, .tv_usec = 500000 };
	const struct timeval sending = { .tv_sec = 0, .tv_usec = 250000 };
	const struct linger abortive = { .l_onoff = 1, .l_linger = 0 };
	const bool set = fd >= 0 && setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive, sizeof(receive)) == 0 &&
		setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sending, sizeof(sending)) == 0 &&
		setsockopt(fd, SOL_SOCKET, SO_LINGER, &abortive, sizeof(abortive)) == 0;
	struct timeval receive_read = { 0 };
	struct timeval sending_read = { 0 };
	struct linger linger_read = { -1, -1 };
	socklen_t receive_length = sizeof(receive_read);
	socklen_t sending_length = sizeof(sending_read);
	socklen_t linger_length = sizeof(linger_read);
	const bool read = set && getsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &receive_read, &receive_length) == 0 &&
		getsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &sending_read, &sending_length) == 0 &&
		getsockopt(fd, SOL_SOCKET, SO_LINGER, &linger_read, &linger_length) == 0;
	xsdk_log_write("net: options set %d, read %d: SO_RCVTIMEO %d.%06d s, SO_SNDTIMEO %d.%06d s, SO_LINGER {%d, %d}",
		set, read, (int)receive_read.tv_sec, (int)receive_read.tv_usec, (int)sending_read.tv_sec,
		(int)sending_read.tv_usec, linger_read.l_onoff, linger_read.l_linger);
	check("net: getsockopt reads SO_RCVTIMEO, SO_SNDTIMEO and SO_LINGER back as set",
		read && receive_read.tv_sec == 1 && receive_read.tv_usec == 500000 && sending_read.tv_sec == 0 &&
			sending_read.tv_usec == 250000 && linger_read.l_onoff == 1 && linger_read.l_linger == 0);
	if (fd >= 0)
		close(fd);
	char ending[LINE_BYTES] = "";
	const bool asked = ask_pc(pc, "last\n", ending);
	xsdk_log_write("net: closed with SO_LINGER {1, 0}, the PC saw %s", asked ? ending : "nothing");
	// XAM closes it without a FIN or a reset reaching the PC, as the probe found (docs/HARDWARE.md, "XAM's sockets and
	// loader, milestone 56"); a close without the linger sends a FIN.
	check("net: closed with SO_LINGER {1, 0}, the PC sees no FIN, the connection left open as XAM does",
		set && asked && strcmp(ending, "open") == 0);
}

// One more connection than XAM's own limit allows, which only xsdk_network_limits raised gives; the last is numbered
// past what select takes.
static void test_limits(const Host *pc)
{
	int fds[DEFAULT_STREAM_SOCKETS + 1];
	int opened = 0;
	while (opened < DEFAULT_STREAM_SOCKETS + 1 && (fds[opened] = connect_to(pc)) >= 0)
		opened++;
	const bool all = opened == DEFAULT_STREAM_SOCKETS + 1;
	const int last = all ? fds[DEFAULT_STREAM_SOCKETS] : -1;
	const bool echoed = all && echoes(last, "tcp past XAM's own limit\n");
	int buffer_bytes = 0;
	socklen_t length = sizeof(buffer_bytes);
	const bool read = all && getsockopt(fds[0], SOL_SOCKET, SO_RCVBUF, &buffer_bytes, &length) == 0;
	for (int i = 0; i < opened; i++)
		close(fds[i]);
	xsdk_log_write(
		"net: %d TCP connections open at once, the last descriptor %d; SO_RCVBUF %d", opened, last, buffer_bytes);
	check(
		"net: 33 TCP connections open at once, the last numbered past select's and echoing through poll; SO_RCVBUF "
		"at least the raised 63 KB",
		// XAM gave 65700 bytes for 63 KB, more than 63 times 1024, and 16384 for its default.
		all && last >= FD_SETSIZE && echoed && read && buffer_bytes >= RAISED_BUFFER_KB * BYTES_PER_KB);
}

static void test_many(void)
{
	int opened = 0;
	for (; opened < SOCKETS_IN_TURN; opened++) {
		const int fd = socket(AF_INET, SOCK_STREAM, 0);
		if (fd < 0) {
			xsdk_log_write("net: socket %d of %d failed: %d", opened + 1, SOCKETS_IN_TURN, errno);
			break;
		}
		close(fd);
	}
	check("net: 1000 sockets opened and closed in turn", opened == SOCKETS_IN_TURN);
}

void test_net(void)
{
	Host pc;
	const bool known = read_host(&pc);
	check("net: net_host.txt names the PC", known);
	if (!known)
		return;
	struct sockaddr_in console = { 0 };
	test_tcp(&pc, &console);
	test_link_address(&console);
	test_udp(&pc);
	test_accept(&pc);
	test_names();
	test_refused(&console);
	test_non_blocking(&pc);
	test_receive_timeout(&pc);
	test_send_timeout(&pc);
	test_options(&pc);
	test_limits(&pc);
	test_many();
}
