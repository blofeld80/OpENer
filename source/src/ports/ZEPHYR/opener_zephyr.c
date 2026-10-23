/*
 * Optional autostart of the stack (CONFIG_OPENER_AUTOSTART): one thread waits for the link and an IPv4
 * address (static through NET_CONFIG_SETTINGS or DHCPv4), initialises the CIP stack the way OpENer's
 * ports/STM32/opener.c does and then runs the network handler, and with it every OpENer callback.
 */
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>
#include <zephyr/net/net_if.h>
#include <zephyr/random/random.h>
#include <zephyr/sys/byteorder.h>

#include "opener_api.h"
#include "devicedata.h"
#include "cipconnectionobject.h"
#include "cipethernetlink.h"
#include "ciptcpipinterface.h"
#include "doublylinkedlist.h"
#include "generic_networkhandler.h"
#include "xorshiftrandom.h"

LOG_MODULE_REGISTER(opener, LOG_LEVEL_INF);

#define LINK_POLL_MS 100
#define RETRY_MS     100

K_THREAD_STACK_DEFINE(opener_stack, CONFIG_OPENER_THREAD_STACK_SIZE);
static struct k_thread opener_thread;

static void opener_main(void *p1, void *p2, void *p3)
{
	struct net_if *iface = net_if_get_default();
	uint8_t mac[6];

	ARG_UNUSED(p1);
	ARG_UNUSED(p2);
	ARG_UNUSED(p3);

	while (iface == NULL || !net_if_is_up(iface)) {
		k_msleep(LINK_POLL_MS);
	}
	if (IfaceWaitForIp(NULL, -1, NULL) != kEipStatusOk ||
	    IfaceGetMacAddress(NULL, mac) != kEipStatusOk) {
		LOG_ERR("no IPv4 address or MAC address");
		return;
	}

	/* serial number from the (per chip) MAC address */
	SetDeviceSerialNumber(sys_get_be32(&mac[2]));
	DoublyLinkedListInitialize(&connection_list, CipConnectionObjectListArrayAllocator,
				   CipConnectionObjectListArrayFree);
	/* connection IDs: OpENer seeds its generator with time(NULL) otherwise */
	SetXorShiftSeed(sys_rand32_get() | 1u);
	if (CipStackInit(sys_rand16_get()) != kEipStatusOk) {
		LOG_ERR("CipStackInit failed");
		return;
	}
	CipEthernetLinkSetMac(mac);
	g_tcpip.config_control = IS_ENABLED(CONFIG_NET_DHCPV4) ? kTcpipCfgCtrlDhcp : kTcpipCfgCtrlStaticIp;
	GetHostName(&g_tcpip.hostname);
	/* the network handler binds its sockets to this address */
	if (IfaceGetConfiguration(NULL, &g_tcpip.interface_configuration) != kEipStatusOk) {
		LOG_ERR("no interface configuration");
		return;
	}
	if (NetworkHandlerInitialize() != kEipStatusOk) {
		LOG_ERR("network handler initialisation failed");
		return;
	}
	LOG_INF("EtherNet/IP adapter running (%s)", OPENER_DEVICE_NAME);

	for (;;) {
		if (NetworkHandlerProcessCyclic() != kEipStatusOk) {
			/* zsock_poll() failed (logged): keep the device alive and retry */
			k_msleep(RETRY_MS);
		}
	}
}

static int opener_start(void)
{
	k_thread_create(&opener_thread, opener_stack, K_THREAD_STACK_SIZEOF(opener_stack), opener_main,
			NULL, NULL, NULL, CONFIG_OPENER_THREAD_PRIORITY, 0, K_NO_WAIT);
	k_thread_name_set(&opener_thread, "opener");
	return 0;
}

SYS_INIT(opener_start, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
