/*
 * OpENer network handler on Zephyr's native sockets (zsock_*), multiplexed with zsock_poll().
 *
 * Behaviour after OpENer's ports/generic_networkhandler.c (Copyright (c) 2009, Rockwell Automation, Inc.,
 * adapted BSD license, third_party/OpENer/license.txt), restructured for poll():
 * - TCP 44818 listener; one slot per encapsulation session (OPENER_NUMBER_OF_SUPPORTED_SESSIONS) with its own
 *   receive buffer, so encapsulation messages split over several TCP segments are reassembled;
 * - UDP 44818 bound to the interface address (unicast) and to INADDR_ANY (broadcast, e.g. ListIdentity);
 *   replies leave from the unicast socket;
 * - one UDP 2222 socket for all class 0/1 connections (consumed and produced, unicast and multicast T->O),
 *   created on first use and kept open: Zephyr hands a datagram to one socket per port only;
 * - the encapsulation inactivity timeout (TCP/IP object attribute 13) closes idle TCP connections;
 * - ManageConnections() and the registered timeout checkers every kOpenerTimerTickInMilliSeconds.
 *
 * Differences to the reference: setsockopt() argument types as Zephyr expects them (IP_TOS one byte,
 * IP_MULTICAST_TTL an int, IP_MULTICAST_IF a struct ip_mreqn), no SO_BROADCAST (Zephyr receives broadcasts
 * without it and does not implement the option), a short TCP send closes the connection, the inactivity
 * timeout also covers TCP connections without a registered session, and an idle connection is closed with
 * RemoveSession() alone (the reference also passes GetSessionFromSocket()'s 0-based index to
 * CloseClass3ConnectionBasedOnSession(), which expects the 1-based session handle).
 */
#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/socket.h>
#include <zephyr/sys/atomic.h>
#include <zephyr/sys/byteorder.h>
#include <zephyr/sys/fdtable.h>
#include <zephyr/sys/util.h>

#include "generic_networkhandler.h"
#include "opener_error.h"
#include "encap.h"
#include "cipconnectionobject.h"
#include "cipethernetlink.h"
#include "ciptcpipinterface.h"
#include "cipqos.h"
#include "opener_user_conf.h"

#include "opener_zephyr.h"

LOG_MODULE_REGISTER(eip_net, LOG_LEVEL_INF);

#define TCP_BACKLOG          4
#define UDP_IO_BURST         16  /* datagrams read from UDP 2222 per cycle */
#define TIMEOUT_CHECKERS_MAX 10
#define HOUSEKEEPING_MS      100
#define TCP_SEND_TIMEOUT_MS  500
#define POLL_FIXED           4   /* listener, UDP unicast, UDP broadcast, UDP I/O */

struct tcp_client {
	int fd;
	MilliSeconds last_activity;
	size_t len;      /* bytes in buf */
	size_t msg_len;  /* length of the message in buf once its header is complete */
	size_t discard;  /* bytes of an oversized message still to be dropped */
	uint8_t buf[PC_OPENER_ETHERNET_BUFFER_SIZE];
};

/* globals of generic_networkhandler.h used by the OpENer core */
SocketTimer g_timestamps[OPENER_NUMBER_OF_SUPPORTED_SESSIONS];
int g_current_active_tcp_socket = -1;
MilliSeconds g_actual_time;
MilliSeconds g_last_time;
NetworkStatus g_network_status = {
	.tcp_listener = -1,
	.udp_unicast_listener = -1,
	.udp_global_broadcast_listener = -1,
	.udp_io_messaging = -1,
};

static struct tcp_client clients[OPENER_NUMBER_OF_SUPPORTED_SESSIONS];
static TimeoutCheckerFunction timeout_checkers[TIMEOUT_CHECKERS_MAX];
static struct zsock_pollfd fds[POLL_FIXED + OPENER_NUMBER_OF_SUPPORTED_SESSIONS];
static uint8_t rx_buf[PC_OPENER_ETHERNET_BUFFER_SIZE];
static ENIPMessage tx_msg;
static MilliSeconds housekeeping_ms;

static struct {
	atomic_t tcp, sessions, io, explicit_msg, cycles;
} stats;

/* ----------------------------------------------------------------- platform part (networkhandler.h) */
MilliSeconds GetMilliSeconds(void)
{
	return (MilliSeconds)k_uptime_get();
}

MicroSeconds GetMicroSeconds(void)
{
	return (MicroSeconds)k_ticks_to_us_floor64(k_uptime_ticks());
}

EipStatus NetworkHandlerInitializePlatform(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(clients); i++) {
		clients[i].fd = -1;
	}
	return kEipStatusOk;
}

void ShutdownSocketPlatform(int socket_handle)
{
	if (zsock_shutdown(socket_handle, ZSOCK_SHUT_RDWR) < 0) {
		LOG_DBG("shutdown %d: %d", socket_handle, errno);
	}
}

void CloseSocketPlatform(int socket_handle)
{
	(void)zsock_close(socket_handle);
}

int SetSocketToNonBlocking(int socket_handle)
{
	int flags = zsock_fcntl(socket_handle, ZVFS_F_GETFL, 0);

	if (flags < 0) {
		return flags;
	}
	return zsock_fcntl(socket_handle, ZVFS_F_SETFL, flags | ZVFS_O_NONBLOCK);
}

int SetQosOnSocket(const int socket, CipUsint qos_value)
{
	/* CIP Vol. 2, 5-7.4.2: the DSCP value goes into the upper six bits of the ToS byte */
	uint8_t tos = (uint8_t)(qos_value << 2);

	return zsock_setsockopt(socket, NET_IPPROTO_IP, ZSOCK_IP_TOS, &tos, sizeof(tos));
}

/* ----------------------------------------------------------------- errors (opener_error.h) */
int GetSocketErrorNumber(void)
{
	return errno;
}

char *GetErrorMessage(int error_number)
{
	/* constant strings: nothing to free */
	return (char *)strerror(error_number);
}

void FreeErrorMessage(char *error_message)
{
	ARG_UNUSED(error_message);
}

/* ----------------------------------------------------------------- sockets */
static bool would_block(int err)
{
	return err == EAGAIN || err == EWOULDBLOCK;
}

/* slot of an open TCP connection, or a free slot for fd = -1 */
static struct tcp_client *client_of(int fd)
{
	for (size_t i = 0; i < ARRAY_SIZE(clients); i++) {
		if (clients[i].fd == fd) {
			return &clients[i];
		}
	}
	return NULL;
}

/* socket with SO_REUSEADDR, non-blocking, bound to addr (network order):port */
static int open_socket(int type, int proto, uint32_t addr, uint16_t port)
{
	struct net_sockaddr_in sa = {
		.sin_family = NET_AF_INET,
		.sin_port = net_htons(port),
		.sin_addr.s_addr = addr,
	};
	int one = 1;
	int fd = zsock_socket(NET_AF_INET, type, proto);

	if (fd < 0) {
		LOG_ERR("socket(%d): %d", type, errno);
		return -1;
	}
	if (zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_REUSEADDR, &one, sizeof(one)) < 0 ||
	    SetSocketToNonBlocking(fd) < 0 ||
	    zsock_bind(fd, (struct net_sockaddr *)&sa, sizeof(sa)) < 0) {
		LOG_ERR("socket(%d) port %u: %d", type, port, errno);
		zsock_close(fd);
		return -1;
	}
	return fd;
}

static void set_explicit_qos(int fd)
{
	if (SetQosOnSocket(fd, CipQosGetDscpPriority(kConnectionObjectPriorityExplicit)) != 0) {
		LOG_WRN("QoS on socket %d: %d", fd, errno);
	}
}

static void remove_socket_timer(int fd)
{
	SocketTimer *t;

	while ((t = SocketTimerArrayGetSocketTimer(g_timestamps, OPENER_NUMBER_OF_SUPPORTED_SESSIONS, fd)) != NULL) {
		SocketTimerClear(t);
	}
}

void CloseSocket(const int socket_handle)
{
	struct tcp_client *c;

	if (socket_handle < 0) {
		return;
	}
	c = client_of(socket_handle);
	if (c != NULL) {
		c->fd = -1;
	}
	if (socket_handle == g_network_status.udp_io_messaging) {
		g_network_status.udp_io_messaging = -1;
	}
	CloseSocketPlatform(socket_handle);
}

void CloseUdpSocket(int socket_handle)
{
	/* the I/O socket is shared by all connections and stays open */
	if (socket_handle != g_network_status.udp_io_messaging) {
		CloseSocket(socket_handle);
	}
}

void CloseTcpSocket(int socket_handle)
{
	if (socket_handle < 0) {
		return;
	}
	ShutdownSocketPlatform(socket_handle);
	remove_socket_timer(socket_handle);
	CloseSocket(socket_handle);
}

/* peer closed, error or timeout: TCP connection and its session (with its class 3 connections) */
static void close_client(int fd)
{
	CloseTcpSocket(fd);
	RemoveSession(fd);
}

int CreateUdpSocket(void)
{
	if (g_network_status.udp_io_messaging < 0) {
		/* ENIP: produced data leaves from port 2222, consumed data arrives on it */
		g_network_status.udp_io_messaging = open_socket(NET_SOCK_DGRAM, NET_IPPROTO_UDP, NET_INADDR_ANY,
								kOpenerEipIoUdpPort);
	}
	return g_network_status.udp_io_messaging;
}

EipStatus SendUdpData(const struct sockaddr_in *const address, const ENIPMessage *const outgoing_message)
{
	/* OpENer fills sin_family with the wire value AF_INET (2), see platform_network_includes.h */
	struct net_sockaddr_in to = *address;
	ssize_t n;

	if (g_network_status.udp_io_messaging < 0) {
		return kEipStatusError;
	}
	to.sin_family = NET_AF_INET;
	n = zsock_sendto(g_network_status.udp_io_messaging, outgoing_message->message_buffer,
			 outgoing_message->used_message_length, 0, (const struct net_sockaddr *)&to, sizeof(to));
	if (n < 0) {
		LOG_DBG("sendto: %d", errno);
		return kEipStatusError;
	}
	return (size_t)n == outgoing_message->used_message_length ? kEipStatusOk : kEipStatusError;
}

int SetQos(CipUsint qos_for_socket)
{
	if (SetQosOnSocket(g_network_status.udp_io_messaging, CipQosGetDscpPriority(qos_for_socket)) != 0) {
		LOG_WRN("QoS on I/O socket: %d", errno);
		return errno;
	}
	return 0;
}

int SetSocketOptionsMulticastProduce(void)
{
	int ttl = g_tcpip.mcast_ttl_value;
	struct net_ip_mreqn mreqn = {
		.imr_ifindex = net_if_get_by_iface(net_if_get_default()),
	};

	if (zsock_setsockopt(g_network_status.udp_io_messaging, NET_IPPROTO_IP, ZSOCK_IP_MULTICAST_TTL, &ttl,
			     sizeof(ttl)) < 0) {
		LOG_WRN("IP_MULTICAST_TTL %d: %d", ttl, errno);
		return errno;
	}
	if (zsock_setsockopt(g_network_status.udp_io_messaging, NET_IPPROTO_IP, ZSOCK_IP_MULTICAST_IF, &mreqn,
			     sizeof(mreqn)) < 0) {
		LOG_WRN("IP_MULTICAST_IF: %d", errno);
		return errno;
	}
	return 0;
}

EipUint32 GetPeerAddress(void)
{
	struct net_sockaddr_in peer;
	net_socklen_t len = sizeof(peer);

	if (zsock_getpeername(g_current_active_tcp_socket, (struct net_sockaddr *)&peer, &len) < 0) {
		LOG_WRN("getpeername %d: %d", g_current_active_tcp_socket, errno);
		return net_htonl(NET_INADDR_ANY);
	}
	return peer.sin_addr.s_addr;
}

void RegisterTimeoutChecker(TimeoutCheckerFunction timeout_checker_function)
{
	for (size_t i = 0; i < ARRAY_SIZE(timeout_checkers); i++) {
		if (timeout_checkers[i] == NULL) {
			timeout_checkers[i] = timeout_checker_function;
			return;
		}
	}
	LOG_ERR("no room for timeout checker %p", timeout_checker_function);
}

/* ----------------------------------------------------------------- init / finish */
EipStatus NetworkHandlerInitialize(void)
{
	int fd;

	if (NetworkHandlerInitializePlatform() != kEipStatusOk) {
		return kEipStatusError;
	}
	SocketTimerArrayInitialize(g_timestamps, OPENER_NUMBER_OF_SUPPORTED_SESSIONS);
	/* activate the current DSCP values */
	CipQosUpdateUsedSetQosValues();
	/* multicast address range from the current IP address */
	CipTcpIpCalculateMulticastIp(&g_tcpip);
	/* freeze IP and mask matching the socket setup (off-subnet multicast check) */
	g_network_status.ip_address = g_tcpip.interface_configuration.ip_address;
	g_network_status.network_mask = g_tcpip.interface_configuration.network_mask;
	/* the encapsulation layer reads the IP address */
	EncapsulationInit();

	fd = open_socket(NET_SOCK_STREAM, NET_IPPROTO_TCP, g_network_status.ip_address, kOpenerEthernetPort);
	if (fd < 0) {
		return kEipStatusError;
	}
	/* DSCP explicit already on the SYN/ACK */
	set_explicit_qos(fd);
	if (zsock_listen(fd, TCP_BACKLOG) < 0) {
		LOG_ERR("listen: %d", errno);
		zsock_close(fd);
		return kEipStatusError;
	}
	g_network_status.tcp_listener = fd;

	fd = open_socket(NET_SOCK_DGRAM, NET_IPPROTO_UDP, g_network_status.ip_address, kOpenerEthernetPort);
	if (fd < 0) {
		return kEipStatusError;
	}
	set_explicit_qos(fd);
	g_network_status.udp_unicast_listener = fd;

	fd = open_socket(NET_SOCK_DGRAM, NET_IPPROTO_UDP, net_htonl(NET_INADDR_ANY), kOpenerEthernetPort);
	if (fd < 0) {
		return kEipStatusError;
	}
	set_explicit_qos(fd);
	g_network_status.udp_global_broadcast_listener = fd;

	g_actual_time = g_last_time = GetMilliSeconds();
	housekeeping_ms = g_actual_time;
	g_network_status.elapsed_time = 0;
	return kEipStatusOk;
}

EipStatus NetworkHandlerFinish(void)
{
	for (size_t i = 0; i < ARRAY_SIZE(clients); i++) {
		if (clients[i].fd >= 0) {
			close_client(clients[i].fd);
		}
	}
	CloseTcpSocket(g_network_status.tcp_listener);
	CloseSocket(g_network_status.udp_unicast_listener);
	CloseSocket(g_network_status.udp_global_broadcast_listener);
	CloseSocket(g_network_status.udp_io_messaging);
	g_network_status.tcp_listener = -1;
	g_network_status.udp_unicast_listener = -1;
	g_network_status.udp_global_broadcast_listener = -1;
	return kEipStatusOk;
}

/* ----------------------------------------------------------------- receive paths */
static void tcp_accept(void)
{
	struct tcp_client *c = client_of(-1);
	struct zsock_timeval tv = { .tv_sec = 0, .tv_usec = TCP_SEND_TIMEOUT_MS * 1000 };
	int fd = zsock_accept(g_network_status.tcp_listener, NULL, NULL);

	if (fd < 0) {
		if (!would_block(errno)) {
			LOG_WRN("accept: %d", errno);
		}
		return;
	}
	if (c == NULL) {
		LOG_WRN("TCP: all %d sessions in use, connection refused", OPENER_NUMBER_OF_SUPPORTED_SESSIONS);
		zsock_close(fd);
		return;
	}
	/* replies are sent blocking, bounded by SO_SNDTIMEO; receiving uses MSG_DONTWAIT */
	if (zsock_setsockopt(fd, ZSOCK_SOL_SOCKET, ZSOCK_SO_SNDTIMEO, &tv, sizeof(tv)) < 0) {
		LOG_WRN("SO_SNDTIMEO: %d", errno);
	}
	set_explicit_qos(fd);
	c->fd = fd;
	c->len = 0;
	c->msg_len = 0;
	c->discard = 0;
	c->last_activity = g_actual_time;
}

static void tcp_process(struct tcp_client *c)
{
	struct net_sockaddr peer = { 0 };
	net_socklen_t peer_len = sizeof(peer);
	int fd = c->fd, remaining = 0;
	EipStatus status;
	SocketTimer *t;

	if (zsock_getpeername(fd, &peer, &peer_len) < 0) {
		LOG_WRN("getpeername %d: %d", fd, errno);
	}
	InitializeENIPMessage(&tx_msg);
	g_current_active_tcp_socket = fd;
	status = HandleReceivedExplictTcpData(fd, c->buf, c->len, &remaining, &peer, &tx_msg);
	g_current_active_tcp_socket = -1;
	c->len = 0;
	c->msg_len = 0;
	if (c->fd != fd) {
		return; /* closed by the encapsulation layer (UnRegisterSession) */
	}
	t = SocketTimerArrayGetSocketTimer(g_timestamps, OPENER_NUMBER_OF_SUPPORTED_SESSIONS, fd);
	if (t != NULL) {
		SocketTimerSetLastUpdate(t, g_actual_time);
	}
	if (status == kEipStatusOkSend) {
		ssize_t n = zsock_send(fd, tx_msg.message_buffer, tx_msg.used_message_length, 0);

		if (n < 0 || (size_t)n != tx_msg.used_message_length) {
			LOG_WRN("TCP %d: reply not sent (%d), closing", fd, n < 0 ? errno : (int)n);
			close_client(fd);
		}
	}
}

static void tcp_receive(struct tcp_client *c)
{
	size_t want;
	ssize_t n;

	if (c->discard > 0) {
		want = MIN(c->discard, sizeof(c->buf));
	} else if (c->len < ENCAPSULATION_HEADER_LENGTH) {
		want = ENCAPSULATION_HEADER_LENGTH - c->len;
	} else {
		want = c->msg_len - c->len;
	}
	n = zsock_recv(c->fd, c->discard > 0 ? c->buf : c->buf + c->len, want, ZSOCK_MSG_DONTWAIT);
	if (n == 0) {
		LOG_DBG("TCP %d: closed by peer", c->fd);
		close_client(c->fd);
		return;
	}
	if (n < 0) {
		if (!would_block(errno)) {
			LOG_WRN("TCP %d: recv %d", c->fd, errno);
			close_client(c->fd);
		}
		return;
	}
	c->last_activity = g_actual_time;
	if (c->discard > 0) {
		c->discard -= (size_t)n;
		return;
	}
	c->len += (size_t)n;
	if (c->len == ENCAPSULATION_HEADER_LENGTH && c->msg_len == 0) {
		/* encapsulation header: command (2), length (2), ... */
		size_t total = ENCAPSULATION_HEADER_LENGTH + sys_get_le16(&c->buf[2]);

		if (total > sizeof(c->buf)) {
			LOG_WRN("TCP %d: %u byte message dropped", c->fd, (unsigned int)total);
			c->discard = total - c->len;
			c->len = 0;
			return;
		}
		c->msg_len = total;
	}
	if (c->msg_len != 0 && c->len == c->msg_len) {
		tcp_process(c);
	}
}

static void udp_explicit_receive(int fd, bool unicast)
{
	struct net_sockaddr_in from = { 0 };
	net_socklen_t from_len = sizeof(from);
	int remaining = 0;
	EipStatus status;
	ssize_t n;

	n = zsock_recvfrom(fd, rx_buf, sizeof(rx_buf), 0, (struct net_sockaddr *)&from, &from_len);
	if (n <= 0) {
		if (n < 0 && !would_block(errno)) {
			LOG_WRN("UDP %d: recvfrom %d", fd, errno);
		}
		return;
	}
	InitializeENIPMessage(&tx_msg);
	/* replies (and delayed ListIdentity replies) leave from the unicast socket */
	status = HandleReceivedExplictUdpData(g_network_status.udp_unicast_listener, &from, rx_buf, (size_t)n,
					      &remaining, unicast, &tx_msg);
	if (status == kEipStatusOkSend) {
		n = zsock_sendto(g_network_status.udp_unicast_listener, tx_msg.message_buffer,
				 tx_msg.used_message_length, 0, (struct net_sockaddr *)&from, sizeof(from));
		if (n < 0 || (size_t)n != tx_msg.used_message_length) {
			LOG_WRN("UDP reply not sent: %d", n < 0 ? errno : (int)n);
		}
	}
}

static void udp_io_receive(void)
{
	for (int i = 0; i < UDP_IO_BURST && g_network_status.udp_io_messaging >= 0; i++) {
		struct net_sockaddr_in from = { 0 };
		net_socklen_t from_len = sizeof(from);
		ssize_t n = zsock_recvfrom(g_network_status.udp_io_messaging, rx_buf, sizeof(rx_buf), 0,
					   (struct net_sockaddr *)&from, &from_len);

		if (n < 0) {
			if (!would_block(errno)) {
				LOG_WRN("UDP I/O: recvfrom %d", errno);
			}
			return;
		}
		if (n > 0) {
			HandleReceivedConnectedData(rx_buf, (int)n, &from);
		}
	}
}

/* ----------------------------------------------------------------- cyclic part */
static void check_inactivity(void)
{
	MilliSeconds limit = 1000UL * g_tcpip.encapsulation_inactivity_timeout;

	if (limit == 0) {
		return; /* disabled */
	}
	for (size_t i = 0; i < ARRAY_SIZE(clients); i++) {
		if (clients[i].fd >= 0 && g_actual_time - clients[i].last_activity >= limit) {
			LOG_INF("TCP %d: encapsulation inactivity timeout", clients[i].fd);
			close_client(clients[i].fd);
		}
	}
}

static void housekeeping(void)
{
	struct net_if *iface = net_if_get_default();
	int tcp = 0, sessions = 0, io = 0, explicit_msg = 0;

	/* Ethernet Link object, interface flags bit 0: link status */
	if (iface != NULL && net_if_is_carrier_ok(iface)) {
		g_ethernet_link[0].interface_flags |= 1u;
	} else {
		g_ethernet_link[0].interface_flags &= ~1u;
	}

	for (size_t i = 0; i < ARRAY_SIZE(clients); i++) {
		if (clients[i].fd >= 0) {
			tcp++;
			if (GetSessionFromSocket(clients[i].fd) < OPENER_NUMBER_OF_SUPPORTED_SESSIONS) {
				sessions++;
			}
		}
	}
	for (DoublyLinkedListNode *n = connection_list.first; n != NULL; n = n->next) {
		CipConnectionObject *conn = n->data;

		if (ConnectionObjectGetState(conn) != kConnectionObjectStateEstablished) {
			continue;
		}
		if (ConnectionObjectGetInstanceType(conn) == kConnectionObjectInstanceTypeExplicitMessaging) {
			explicit_msg++;
		} else {
			io++;
		}
	}
	atomic_set(&stats.tcp, tcp);
	atomic_set(&stats.sessions, sessions);
	atomic_set(&stats.io, io);
	atomic_set(&stats.explicit_msg, explicit_msg);
}

void opener_zephyr_stats_get(struct opener_zephyr_stats *st)
{
	st->tcp_connections = (uint8_t)atomic_get(&stats.tcp);
	st->sessions = (uint8_t)atomic_get(&stats.sessions);
	st->io_connections = (uint8_t)atomic_get(&stats.io);
	st->explicit_connections = (uint8_t)atomic_get(&stats.explicit_msg);
	st->cycles = (uint32_t)atomic_get(&stats.cycles);
}

static int build_poll_set(void)
{
	const int fixed[POLL_FIXED] = {
		g_network_status.tcp_listener,
		g_network_status.udp_unicast_listener,
		g_network_status.udp_global_broadcast_listener,
		g_network_status.udp_io_messaging,
	};
	int n = 0;

	for (int i = 0; i < POLL_FIXED; i++) {
		if (fixed[i] >= 0) {
			fds[n].fd = fixed[i];
			fds[n].events = ZSOCK_POLLIN;
			fds[n].revents = 0;
			n++;
		}
	}
	for (size_t i = 0; i < ARRAY_SIZE(clients); i++) {
		if (clients[i].fd >= 0) {
			fds[n].fd = clients[i].fd;
			fds[n].events = ZSOCK_POLLIN;
			fds[n].revents = 0;
			n++;
		}
	}
	return n;
}

static void dispatch(int nfds)
{
	for (int i = 0; i < nfds; i++) {
		int fd = fds[i].fd;
		struct tcp_client *c;

		if ((fds[i].revents & (ZSOCK_POLLIN | ZSOCK_POLLERR | ZSOCK_POLLHUP)) == 0) {
			continue;
		}
		if (fd == g_network_status.tcp_listener) {
			tcp_accept();
		} else if (fd == g_network_status.udp_unicast_listener) {
			udp_explicit_receive(fd, true);
		} else if (fd == g_network_status.udp_global_broadcast_listener) {
			udp_explicit_receive(fd, false);
		} else if (fd == g_network_status.udp_io_messaging) {
			udp_io_receive();
		} else {
			/* NULL if the socket was closed earlier in this cycle */
			c = client_of(fd);
			if (c != NULL) {
				tcp_receive(c);
			}
		}
	}
}

EipStatus NetworkHandlerProcessCyclic(void)
{
	MilliSeconds tick = kOpenerTimerTickInMilliSeconds;
	int timeout = g_network_status.elapsed_time < tick ? (int)(tick - g_network_status.elapsed_time) : 0;
	int nfds = build_poll_set();
	int ready = zsock_poll(fds, nfds, timeout);

	if (ready < 0) {
		if (errno == EINTR) {
			return kEipStatusOk;
		}
		LOG_ERR("poll: %d", errno);
		return kEipStatusError;
	}

	g_actual_time = GetMilliSeconds();
	g_network_status.elapsed_time += g_actual_time - g_last_time;
	g_last_time = g_actual_time;

	if (ready > 0) {
		dispatch(nfds);
	}
	check_inactivity();

	if (g_network_status.elapsed_time >= tick) {
		ManageConnections(g_network_status.elapsed_time);
		for (size_t i = 0; i < ARRAY_SIZE(timeout_checkers); i++) {
			if (timeout_checkers[i] != NULL) {
				timeout_checkers[i](g_network_status.elapsed_time);
			}
		}
		g_network_status.elapsed_time = 0;
	}

	if (g_actual_time - housekeeping_ms >= HOUSEKEEPING_MS) {
		housekeeping_ms = g_actual_time;
		housekeeping();
	}
	atomic_inc(&stats.cycles);
	return kEipStatusOk;
}
