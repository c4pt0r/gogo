/* gogovm --host-net (or GOGO_NET=host): the "tcp" and "tcp_listen" ports on
 * the host's own sockets instead of the simulated MCU stack, so a host VM
 * can be a real network peer (the server node of gogo/node). */
#ifndef GOGO_HOSTNET_H
#define GOGO_HOSTNET_H

#include "port.h"

struct Vm;

extern const PortDriver hostnet_tcp_driver;
extern const PortDriver hostnet_listen_driver;
/* Node ports take their tcp connections over (runtime/dist, DistCarrier). */
extern const struct DistCarrier hostnet_carrier;

/* Attaches the sockets' event source to vm (once, after vm_boot). */
void hostnet_install(struct Vm *vm);

#endif
