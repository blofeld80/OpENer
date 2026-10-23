/*
 * Interface configuration for OpENer (opener_api.h: IfaceGetConfiguration, IfaceGetMacAddress,
 * IfaceWaitForIp, GetHostName) on net_if. The board has one interface: the TcpIpInterface name passed
 * by OpENer is not used. Bringing the interface up (static IPv4, DHCPv4) is the application's job.
 */
#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/net/hostname.h>
#include <zephyr/net/net_if.h>
#include <zephyr/net/net_ip.h>

#include "opener_api.h"
#include "cipstring.h"

#define WAIT_POLL_MS 100

static struct net_if *iface_of(TcpIpInterface *iface)
{
	ARG_UNUSED(iface);
	return net_if_get_default();
}

EipStatus IfaceGetMacAddress(TcpIpInterface *iface, uint8_t *const physical_address)
{
	struct net_if *nif = iface_of(iface);
	struct net_linkaddr *ll = nif != NULL ? net_if_get_link_addr(nif) : NULL;

	if (ll == NULL || ll->len != 6) {
		errno = ENODEV;
		return kEipStatusError;
	}
	memcpy(physical_address, ll->addr, 6);
	return kEipStatusOk;
}

EipStatus IfaceGetConfiguration(TcpIpInterface *iface, CipTcpIpInterfaceConfiguration *iface_cfg)
{
	struct net_if *nif = iface_of(iface);
	struct net_in_addr *addr = nif != NULL ? net_if_ipv4_get_global_addr(nif, NET_ADDR_PREFERRED) : NULL;
	struct net_in_addr mask, gw;

	if (addr == NULL) {
		errno = EADDRNOTAVAIL;
		return kEipStatusError;
	}
	mask = net_if_ipv4_get_netmask_by_addr(nif, addr);
	gw = net_if_ipv4_get_gw(nif);

	ClearCipString(&iface_cfg->domain_name);
	iface_cfg->ip_address = addr->s_addr;
	iface_cfg->network_mask = mask.s_addr;
	iface_cfg->gateway = gw.s_addr;
	iface_cfg->name_server = 0;
	iface_cfg->name_server_2 = 0;
	return kEipStatusOk;
}

EipStatus IfaceWaitForIp(TcpIpInterface *const iface, int timeout, volatile int *const abort_wait)
{
	struct net_if *nif = iface_of(iface);
	int64_t end = k_uptime_get() + (int64_t)timeout * 1000;

	while (nif == NULL || net_if_ipv4_get_global_addr(nif, NET_ADDR_PREFERRED) == NULL) {
		if ((abort_wait != NULL && *abort_wait != 0) || (timeout >= 0 && k_uptime_get() >= end)) {
			errno = ETIMEDOUT;
			return kEipStatusError;
		}
		k_msleep(WAIT_POLL_MS);
	}
	return kEipStatusOk;
}

void GetHostName(CipString *hostname)
{
	SetCipStringByCstr(hostname, net_hostname_get());
}
