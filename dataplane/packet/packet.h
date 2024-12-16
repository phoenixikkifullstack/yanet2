#ifndef PACKET_H
#define PACKET_H

#include <stdint.h>
#include <stdlib.h>

#include "rte_ether.h"
#include "rte_ip.h"
#include "rte_mbuf.h"

#define PACKET_HEADER_TYPE_UNKNOWN 0

struct network_header {
	uint16_t type;
	uint16_t offset;
};

struct transport_header {
	uint16_t type;
	uint16_t offset;
};

struct pipeline;

struct packet {
	struct packet *next;
	struct rte_mbuf *mbuf;

	struct pipeline *pipeline;

	uint32_t hash;

	uint16_t rx_device_id;
	uint16_t tx_device_id;

	uint16_t tx_result;

	uint16_t flags;
	uint16_t vlan;

	struct network_header network_header;
	struct transport_header transport_header;
};

struct packet_list {
	struct packet *first;
	struct packet **last;
};

static inline void
packet_list_init(struct packet_list *list) {
	list->first = NULL;
	list->last = &list->first;
}

static inline void
packet_list_add(struct packet_list *list, struct packet *packet) {
	*list->last = packet;
	packet->next = NULL;
	list->last = &packet->next;
}

static inline struct packet *
packet_list_first(struct packet_list *list) {
	return list->first;
}

static inline void
packet_list_concat(struct packet_list *dst, struct packet_list *src) {
	// Nothing to do if src is empty
	if (src->first == NULL)
		return;

	// Replace dst with src if dst is empty
	if (dst->first == NULL) {
		*dst = *src;
		return;
	}

	*dst->last = packet_list_first(src);
	dst->last = src->last;
}

static inline struct packet *
packet_list_pop(struct packet_list *packets) {
	struct packet *res = packets->first;
	if (res == NULL)
		return res;

	packets->first = res->next;
	if (packets->first == NULL)
		packets->last = &packets->first;

	return res;
}

int
parse_packet(struct packet *packet);

static inline struct rte_mbuf *
packet_to_mbuf(const struct packet *packet) {
	return packet->mbuf;
}

static inline struct packet *
mbuf_to_packet(struct rte_mbuf *mbuf) {
	return (struct packet *)((void *)mbuf->buf_addr);
}

struct ipv6_ext_2byte {
	uint8_t next_type;
	uint8_t size;
} __rte_packed;

struct ipv6_ext_fragment {
	uint8_t next_type;
	uint8_t reserved;
	uint16_t offset_flag;
	uint32_t identification;
} __rte_packed;

#endif
