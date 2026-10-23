/*
 * Zephyr port of OpENer: OpENer's network handler on zsock_poll() (networkhandler.c) and the interface
 * hooks of opener_api.h on net_if (networkconfig.c).
 */
#ifndef OPENER_ZEPHYR_H_
#define OPENER_ZEPHYR_H_

#include <stdint.h>

/* snapshot of the EtherNet/IP state, updated by NetworkHandlerProcessCyclic() */
struct opener_zephyr_stats {
	uint8_t tcp_connections;      /* accepted TCP connections on 44818 */
	uint8_t sessions;             /* registered encapsulation sessions */
	uint8_t io_connections;       /* established class 0/1 connections */
	uint8_t explicit_connections; /* established class 3 connections */
	uint32_t cycles;              /* NetworkHandlerProcessCyclic() calls */
};

void opener_zephyr_stats_get(struct opener_zephyr_stats *st);

#endif /* OPENER_ZEPHYR_H_ */
