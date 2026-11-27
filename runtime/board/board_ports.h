/* Board ports (gogo.h GOGO_BOARD_PORT): the drivers a firmware or gogovm
 * build registers through the stable host-port API, run by the same
 * layer as libgogo's host ports (runtime/embed: hostport.c, inbox.c,
 * poster.c). Built only with GOGO_BOARD_PORTS (gogovm always, the ESP32-S3
 * with CONFIG_GOGO_BOARD_PORTS); gogo_board_main calls it.
 *
 * The registrations are pointers in a linker section, laid there by the
 * macro: Mach-O "__DATA,gogo_bport" (section$start/section$end), ELF
 * "gogo_board_port" (__start_/__stop_), and on ESP-IDF ".gogo_board_port",
 * collected by platform/esp32s3/main/linker.lf (KEEP, SURROUND). Nothing
 * runs to register a port and nothing in them is mutable; the section
 * always holds this layer's own NULL entry, so it exists in every build. */
#ifndef GOGO_BOARD_PORTS_H
#define GOGO_BOARD_PORTS_H

#include <stdint.h>
#include "host.h"

struct Vm;
struct EmbedPorts;

/* The board's host-port shell if the build has board ports, from ph's
 * alloc (never freed: posters may outlive the run), else NULL (also out
 * of memory, said on h's stream 2). ph's alloc, free and wake must be
 * callable from any task (posts), unlike a board's VM arena. */
struct EmbedPorts *board_ports_new(const gogo_host *h, const gogo_host *ph);

/* After each vm_init of the boot: the first time, registers the ports on
 * vm's board (in name order; a refusal is said on h's stream 2); every
 * time, installs the driver list and the post inbox's event source. 0, or
 * -1 if the Vm has no source slot left. */
int board_ports_attach(struct EmbedPorts *ep, struct Vm *vm, const gogo_host *h);

/* The image loaded: no more registrations. */
void board_ports_loaded(struct EmbedPorts *ep);

/* The run has ended: posts are closed (a poster's return GOGO_ECLOSED)
 * and whatever is queued is dropped. */
void board_ports_end(struct EmbedPorts *ep);

/* The names the build's board ports provide on board b (what attach
 * accepts: the requirement names, for `gogo device info`), in
 * registration order: at most cap of them into out; returns how many
 * there are. Any task; needs no VM. */
uint32_t board_ports_names(const VmBoard *b, const char **out, uint32_t cap);

#endif
