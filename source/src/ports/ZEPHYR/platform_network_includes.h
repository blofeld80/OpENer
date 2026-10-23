/*
 * Socket names used by the OpENer core, mapped onto Zephyr's native network API (<zephyr/net/socket.h>,
 * <zephyr/net/net_ip.h>). No POSIX layer: CONFIG_POSIX_API and CONFIG_NET_NAMESPACE_COMPAT_MODE are off, Zephyr
 * names everything net_* / NET_* / zsock_*, this header gives the OpENer core the BSD spellings it uses.
 */
#ifndef PLATFORM_NETWORK_INCLUDES_H_
#define PLATFORM_NETWORK_INCLUDES_H_

#include <errno.h>
#include <stdint.h>
#include <sys/types.h>

#include <zephyr/net/net_ip.h>
#include <zephyr/net/socket.h>

#if !defined(CONFIG_NET_NAMESPACE_COMPAT_MODE)
#define sockaddr    net_sockaddr
#define sockaddr_in net_sockaddr_in
#define in_addr     net_in_addr
#define socklen_t   net_socklen_t

#define SOCK_STREAM NET_SOCK_STREAM
#define SOCK_DGRAM  NET_SOCK_DGRAM
#define IPPROTO_IP  NET_IPPROTO_IP
#define IPPROTO_TCP NET_IPPROTO_TCP
#define IPPROTO_UDP NET_IPPROTO_UDP

#define htons(x) net_htons(x)
#define htonl(x) net_htonl(x)
#define ntohs(x) net_ntohs(x)
#define ntohl(x) net_ntohl(x)

#define INADDR_ANY       NET_INADDR_ANY
#define INADDR_BROADCAST NET_INADDR_BROADCAST
#define INET_ADDRSTRLEN  NET_INET_ADDRSTRLEN
#endif /* !CONFIG_NET_NAMESPACE_COMPAT_MODE */

/*
 * The OpENer core uses AF_INET as the EtherNet/IP wire value of sin_family in socket address items
 * (ListIdentity, Forward_Open sockaddr info: 2, big endian) and stores it in the addresses it hands to the
 * port. Zephyr's NET_AF_INET is 1: the port sets NET_AF_INET where such an address reaches a socket
 * (SendUdpData). OpENer itself never opens sockets.
 */
#undef AF_INET
#define AF_INET 2

typedef uint16_t in_port_t;

/*
 * generic_networkhandler.h declares the select() state of OpENer's reference handler (extern fd_set ...).
 * The Zephyr handler polls and never defines or touches it: an incomplete type keeps it that way.
 */
typedef struct opener_select_not_used fd_set;

/* used by ciptcpipinterface.c only with OPENER_TCPIP_IFACE_CFG_SETTABLE */
typedef uint32_t in_addr_t;
#define INADDR_LOOPBACK 0x7f000001u
#define IN_CLASSA(a)    ((((uint32_t)(a)) & 0x80000000u) == 0u)
#define IN_CLASSA_NET   0xff000000u
#define IN_CLASSB(a)    ((((uint32_t)(a)) & 0xc0000000u) == 0x80000000u)
#define IN_CLASSC(a)    ((((uint32_t)(a)) & 0xe0000000u) == 0xc0000000u)

/* the only socket calls of the OpENer core itself (encap.c): delayed ListIdentity replies and the session
 * lookup of Forward_Open */
static inline ssize_t sendto(int sock, const void *buf, size_t len, int flags,
			     const struct net_sockaddr *dest_addr, net_socklen_t addrlen)
{
	return zsock_sendto(sock, buf, len, flags, dest_addr, addrlen);
}

static inline int getpeername(int sock, struct net_sockaddr *addr, net_socklen_t *addrlen)
{
	return zsock_getpeername(sock, addr, addrlen);
}

#endif /* PLATFORM_NETWORK_INCLUDES_H_ */
