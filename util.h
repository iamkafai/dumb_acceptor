#ifndef __UTIL_H_
#define __UTIL_H_

#include <stdint.h>

extern size_t get_address_len(uint16_t family);
extern void init_saddr(struct sockaddr *saddr, uint16_t family,
		       const uint32_t *addr, uint16_t start_port);
extern int get_next_saddr(struct sockaddr *saddr, uint16_t family,
			  uint32_t start_addr, uint32_t nr_addrs,
			  uint16_t start_port, uint16_t nr_ports);
extern uint64_t time_get_ns(void);

#endif
