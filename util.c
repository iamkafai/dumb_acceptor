#include <arpa/inet.h>
#include <netinet/in.h>
#include <time.h>
#include <string.h>
#include <assert.h>

#include "util.h"

uint64_t time_get_ns(void)
{
	struct timespec ts;

	clock_gettime(CLOCK_MONOTONIC, &ts);
	return ts.tv_sec * 1000000000ull + ts.tv_nsec;
}

size_t get_address_len(uint16_t family)
{
	switch (family) {
	case AF_INET:
		return sizeof(struct sockaddr_in);
	case AF_INET6:
		return sizeof(struct sockaddr_in6);
	default:
		assert(0);
	}
}

void init_saddr(struct sockaddr *saddr, uint16_t family,
		const uint32_t *addr, uint16_t start_port)
{
	struct sockaddr_in *v4 = (struct sockaddr_in *)saddr;
	struct sockaddr_in6 *v6 = (struct sockaddr_in6 *)saddr;

	switch (family) {
	case AF_INET:
		memset(v4, 0, sizeof(*v4));
		v4->sin_family = AF_INET;
		v4->sin_addr.s_addr = *addr;
		v4->sin_port = htons(start_port);
		break;
	case AF_INET6:
		memset(v6, 0, sizeof(*v6));
		v6->sin6_family = AF_INET6;
		memcpy(v6->sin6_addr.s6_addr32, addr,
		       sizeof(v6->sin6_addr.s6_addr32));
		v6->sin6_port = htons(start_port);
		break;
	default:
		assert(0);
	}
}

static int get_next_saddr_v4(struct sockaddr_in *v4,
			     uint32_t start_addr, uint32_t end_addr,
			     uint16_t start_port, uint16_t end_port)
{
	uint16_t port;
	uint32_t addr;

	port = ntohs(v4->sin_port);

	addr = ntohl(v4->sin_addr.s_addr);

	if (end_port > start_port) {
		if (port == end_port)
			port = start_port;
		else
			port++;
		v4->sin_port = htons(port);

		/* Finish iterating port first before
		 * going to the next address.
		 */
		if (port != start_port)
			goto done;
	}

	if (end_addr > start_addr) {
		if (addr == end_addr)
			addr = start_addr;
		else
			addr++;
		v4->sin_addr.s_addr = htonl(addr);
	}

done:
	/* Check if we have wrapped around */
	return addr == start_addr && port == start_port;
}

static int get_next_saddr_v6(struct sockaddr_in6 *v6,
			     uint32_t start_addr, uint32_t end_addr,
			     uint16_t start_port, uint16_t end_port)
{
	uint16_t port;
	uint32_t addr;

	port = ntohs(v6->sin6_port);

	addr = ntohl(v6->sin6_addr.s6_addr32[3]);

	if (end_port > start_port) {
		if (port == end_port)
			port = start_port;
		else
			port++;
		v6->sin6_port = htons(port);

		/* Finish iterating port first before
		 * going to the next address.
		 */
		if (port != start_port)
			goto done;
	}

	if (end_addr > start_addr) {
		if (addr == end_addr)
			addr = start_addr;
		else
			addr++;
		v6->sin6_addr.s6_addr32[3] = htonl(addr);
	}

done:
	/* Check if we have wrapped around */
	return addr == start_addr && port == start_port;
}

int get_next_saddr(struct sockaddr *saddr, uint16_t family,
		   uint32_t start_addr, uint32_t nr_addrs,
		   uint16_t start_port, uint16_t nr_ports)
{
	switch (family) {
	case AF_INET:
		return get_next_saddr_v4((struct sockaddr_in *)saddr,
					 start_addr, nr_addrs,
					 start_port, nr_ports);
	case AF_INET6:
		return get_next_saddr_v6((struct sockaddr_in6 *)saddr,
					 start_addr, nr_addrs,
					 start_port, nr_ports);
	default:
		assert(0);
	}
}
