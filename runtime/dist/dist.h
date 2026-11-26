/* Distributed actors (docs/design.md §5.13): the "node" port driver.
 *
 * A node link is a "node" port (cfg net.NodeLink{Self, Peer, Relay,
 * Agent, Name, Key}) that gogo/node opens once its handshake over TCP
 * succeeded. Its owner (the link actor) pumps bytes: what the peer sent
 * goes to the port as port.Out{Data} commands, and every frame for the
 * peer comes out as one port.Chunk event, which the owner writes to TCP.
 * The driver does the rest in C: the frame MACs, the wire codec, the proxy
 * table that makes remote PIDs ordinary PID terms (term.h,
 * DIST_SLOT_BASE), the export table (the local PIDs a peer can reach,
 * besides the door at PID 0), remote links, monitors and Kill, the Calls
 * in flight, and the relay of frames between links (the server of a
 * star), with NODEUP / NODEDOWN / GONE news.
 *
 * The VM core knows only the hook table (VmDist, actor.h), which the first
 * open installs as vm->dist; the state is allocated then (the VM's
 * allocator) and lives as long as the VM, so a stale proxy stays stale.
 *
 * Wire format (little-endian):
 *   frame   u16 len, then len bytes: u8 type, u8 flags (0), u16 dst node,
 *           u16 src node, the type's fields, then a 16-byte MAC:
 *           HMAC-SHA-256(session key, frame number (u64) || sender id (u16)
 *           || body), truncated (DIST_FRAME_MAX in all; a bad MAC ends
 *           the link)
 *   SEND    u32 dst PID (0: the door), term
 *   REPLY   u32 dst PID, u64 Ref serial, term
 *   LINK, UNLINK   u32 from, u32 to
 *   EXIT, KILL     u32 from, u32 to, u32 code
 *   MONITOR, DEMONITOR   u32 watcher, u32 target, u64 serial
 *   DOWN    u32 watcher, u32 target, u64 serial, u32 code
 *   NODEDOWN   u16 node; NODEUP   u16 node, name (from the relay)
 *   GONE    u32 pid (the sender's actor exited; the relay passes it on)
 * Terms: a tag byte (DT_*, dist.c) and its payload; LEB128 varints, ints
 * zigzag. PIDs travel as (node id, that node's term), Refs as (node id,
 * 48-bit serial; a peer may not send this node's own). Ints, int64, bool,
 * string, Bytes, []byte, slices, nil, actor PIDs, Refs, reserved structs
 * and a module's own structs cross; an own struct goes by its qualified
 * name, field count and shape hash (the kinds of its fields) and the
 * receiver drops it unless its module has the same type. Anything else (a
 * closure, a Buffer, a pointer's target or a map, a port PID, nesting
 * deeper than DIST_DEPTH, a frame over DIST_FRAME_MAX) crashes the sender
 * with R_BAD_MSG, as at a module boundary.
 *
 * Scheduler thread only, like every port callback. */
#ifndef GOGO_DIST_H
#define GOGO_DIST_H

#include <stdint.h>
#include "port.h"

struct Vm;

#define DIST_FRAME_MAX 4096u   /* bytes of a frame, its length prefix included */
#define DIST_LINKS     8u      /* node ports open at once */
#define DIST_NAME_MAX  32u     /* bytes of a node name */
#define DIST_PROXIES   64u     /* remote PIDs known at once */
#define DIST_EXPORTS   64u     /* local PIDs given out (reachable by peers) */
#define DIST_WATCHES   32u     /* remote links and monitors, both directions */
#define DIST_CALLS     16u     /* Calls to remote PIDs waiting at once (a lost node fails them) */
#define DIST_DEPTH     16u     /* nesting of a term on the wire */
#define DIST_WORDS_PER_BYTE 2u /* most heap words a received term may take per frame byte */

extern const PortDriver dist_node_driver;

/* Counters, for tests and diagnostics. */
typedef struct DistStats {
    uint32_t sent;       /* frames handed to a link's owner */
    uint32_t received;   /* frames taken from a link */
    uint32_t relayed;    /* frames passed from one link to another */
    uint32_t dropped;    /* messages lost: no route, stale proxy, a full table or mailbox */
    uint32_t bad;        /* frames refused: malformed, unknown, spoofed or not allowed */
} DistStats;

/* The counters, or NULL if the VM never opened a node link. */
const DistStats *dist_stats(const struct Vm *vm);

#endif
