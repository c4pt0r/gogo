/* The "tcp" and "tcp_listen" ports on POSIX sockets (hostnet.h), speaking
 * the messages of the MCU stack's (lib/net/net.gg): Dial / Connected,
 * the stream protocol (Chunk, Out, Done, Closed), Shutdown, Resume, Listen
 * / Accepted. Differences: Done means the kernel took all of the Out (not
 * that the peer acknowledged it), any local address is fine, and there is
 * no orphan close (closing without Shutdown resets, as on the MCU; after
 * Shutdown the kernel finishes the close).
 *
 * Received bytes are read with MSG_PEEK and consumed only once their Chunk
 * is in the owner's mailbox, so a full mailbox loses nothing: the socket
 * stops being read (the peer's TCP window closes) and is retried after
 * RETRY_MS. One gogovm, one Vm: the state is this file's. */
#define _DARWIN_C_SOURCE
#define _DEFAULT_SOURCE
#include "hostnet.h"
#include "platform_unix.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "opcodes.h"
#include "host.h"
#include "stream.h"
#include "dist.h"

#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#define CHUNK_MAX 1460u      /* bytes per Chunk, as the MCU stack's segments */
#define RETRY_MS  50u        /* an owner without room is tried again after this */

enum { K_FREE = 0, K_CONN = 1, K_LISTEN = 2, K_CARRIED = 3 };
#define CARRY_OUT_MAX (256u * 1024u)   /* bytes a carried link may have unsent */

typedef struct HConn {
    int fd;
    uint8_t kind;
    uint8_t connecting;      /* a Dial in progress */
    uint8_t paused;          /* accepted, not handed off or resumed yet */
    uint8_t rd_done;         /* EOF seen and Closed{Normal} sent, or the socket failed */
    uint8_t dead;            /* the connection ended (terminal Closed sent or owed) */
    uint8_t connected_owed;  /* Connected not delivered yet */
    uint8_t closed_owed;     /* a Closed not delivered yet (closed_reason) */
    uint8_t shut;            /* Shutdown: 1 asked, 2 done */
    int32_t closed_reason;
    uint32_t off;            /* bytes of the head Out written */
    uint32_t seq;            /* Seq of the last Chunk */
    uint64_t deadline;       /* the Dial's TimeoutMs (0: none) */
    uint64_t retry_at;       /* an owner without room: not before this */
    short revents;           /* from the last poll */
    StreamOut so;
    /* K_CARRIED: the connection belongs to node port `node` (its index is
     * this entry's); out holds what is not sent yet. */
    Port *node;
    uint8_t *out;
    uint32_t out_len, out_cap;
} HConn;

/* One HConn per port index (the Vm's port table grows; so does this). */
static HConn *conns;
static uint32_t nconns;
static struct pollfd *pfds;       /* poll_now's scratch, nconns long */
static uint32_t *pidx;
static Vm *hvm;

/* Room for port index i; false if out of memory. */
static bool conns_reach(uint32_t i) {
    if (i < nconns) return true;
    uint32_t n = nconns ? nconns : 16u;
    while (n <= i) n *= 2u;
    HConn *c = realloc(conns, n * sizeof *c);
    struct pollfd *f = realloc(pfds, n * sizeof *f);
    uint32_t *x = realloc(pidx, n * sizeof *x);
    if (c) conns = c;
    if (f) pfds = f;
    if (x) pidx = x;
    if (!c || !f || !x) return false;
    for (uint32_t k = nconns; k < n; k++) {
        memset(&conns[k], 0, sizeof conns[k]);
        conns[k].fd = -1;
    }
    nconns = n;
    return true;
}
static int accept_fd = -1;   /* the socket the next tcp open takes (accept) */

/* The port indexes hostnet may have a socket at. */
static uint32_t lim(const Vm *vm) { return nconns < vm->port_n ? nconns : vm->port_n; }

static HConn *conn_of(const Vm *vm, const Port *p) { return &conns[port_index(vm, p)]; }

static bool event_new(Vm *vm, const Port *p, uint32_t type, term a, bool with_reason, int32_t reason) {
    Actor *o = port_owner(vm, p);
    uint32_t words = type == TYPE_NET_CONNECTED ? 2u : with_reason ? 4u : 3u;
    Msg *m = o ? va_alloc(vm, msg_alloc_bytes((uint16_t)words)) : NULL;
    if (!m) return false;
    m->next = NULL;
    m->words = (uint16_t)words;
    m->flags = 0;
    m->root = term_from_ref(0);
    m->data[0] = hdr_make(words, K_STRUCT);
    m->data[1] = type;
    if (words > 2u) m->data[2] = a;
    if (words > 3u) m->data[3] = term_from_int(reason);
    if (msg_try_enqueue(vm, o, m)) return true;
    msg_free(vm, m);
    return false;
}

static void set_nonblock(int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, FD_CLOEXEC);
#ifdef SO_NOSIGPIPE
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_NOSIGPIPE, &one, sizeof one);
#endif
}

/* The connection ended with reason: the Outs in flight fail, then the
 * Closed is sent (or owed). */
static void end(Vm *vm, Port *p, HConn *c, int32_t reason) {
    if (c->dead) return;
    c->dead = 1;
    c->connecting = 0;
    while (c->so.n) stream_out_fail(vm, p, &c->so);
    c->closed_owed = 1;
    c->closed_reason = reason;
}

/* ---- tcp ---- */

static int t_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    if (!conns_reach(index)) return -1;
    HConn *c = &conns[index];
    memset(c, 0, sizeof *c);
    c->fd = -1;
    if (n == 0u) {                     /* an accepted connection (hostnet's own open) */
        if (accept_fd < 0) return -1;
        c->fd = accept_fd;
        accept_fd = -1;
        c->kind = K_CONN;
        c->paused = 1;
        return 0;
    }
    if (n < 2u || cfg[1] <= 0 || cfg[1] > 65535) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    set_nonblock(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)cfg[1]);
    sa.sin_addr.s_addr = htonl((uint32_t)cfg[0]);
    c->fd = fd;
    c->kind = K_CONN;
    if (connect(fd, (struct sockaddr *)&sa, sizeof sa) == 0) {
        c->connected_owed = 1;
    } else if (errno == EINPROGRESS) {
        c->connecting = 1;
        if (n >= 3u && cfg[2] > 0) c->deadline = vm_now(vm) + (uint32_t)cfg[2];
    } else {
        end(vm, p, c, GOGO_E_REFUSED);
    }
    return 0;
}

static void t_close(struct Vm *vm, Port *p) {
    HConn *c = conn_of(vm ? vm : hvm, p);
    if (c->fd >= 0) {
        if (c->shut != 2) {            /* no Shutdown: reset, as the MCU stack */
            struct linger l = { 1, 0 };
            setsockopt(c->fd, SOL_SOCKET, SO_LINGER, &l, sizeof l);
        }
        close(c->fd);
    }
    stream_out_close(vm, p, &c->so);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static bool t_can_event(const struct Vm *vm, const Port *p) {
    const HConn *c = conn_of(vm, p);
    return !c->dead || c->closed_owed;
}

static void t_handoff(struct Vm *vm, Port *p) { conn_of(vm, p)->paused = 0; }

static void try_shutdown(HConn *c) {
    if (c->shut == 1 && !c->so.n && !c->connecting && !c->dead) {
        shutdown(c->fd, SHUT_WR);
        c->shut = 2;
    }
}

static bool refuse_all(const Port *p, const uint8_t *d, uint32_t len) {
    (void)p; (void)d; (void)len;
    return false;
}

static void t_command(struct Vm *vm, const Actor *from, Port *p, term msg) {
    HConn *c = conn_of(vm, p);
    const uint32_t *o = obj_of(vm, from, msg);
    if (!o || hdr_kind(o[0]) != K_STRUCT) return;
    switch (o[1]) {
    case TYPE_NET_SHUTDOWN:
        if (!c->shut) c->shut = 1;
        try_shutdown(c);
        return;
    case TYPE_NET_RESUME:
        if (from->id == p->owner) c->paused = 0;
        return;
    default:
        break;
    }
    /* An Out: refused (Done{-1}) once ended or shut down. */
    stream_out_accept(vm, from, p, &c->so, msg, c->dead || c->shut ? refuse_all : NULL);
}

/* Writes what it can of the Outs; false if the socket failed. */
static bool write_outs(Vm *vm, Port *p, HConn *c) {
    while (c->so.n) {
        uint32_t len;
        const uint8_t *d = stream_out_head(vm, &c->so, &len);
        if (!d) break;
        ssize_t w = send(c->fd, d + c->off, len - c->off, 0);
        if (w < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        c->off += (uint32_t)w;
        if (c->off < len) break;
        c->off = 0;
        stream_out_done(vm, p, &c->so);
    }
    try_shutdown(c);
    return true;
}

/* Reads one Chunk if the owner can take it; false if the socket failed. */
static bool read_chunk(Vm *vm, Port *p, HConn *c, uint64_t now) {
    uint8_t buf[CHUNK_MAX];
    ssize_t r = recv(c->fd, buf, sizeof buf, MSG_PEEK);
    if (r < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
    if (r == 0) {                      /* the peer finished sending */
        c->rd_done = 1;
        c->closed_owed = 1;
        c->closed_reason = GOGO_E_OK;
        return true;
    }
    Actor *o = port_owner(vm, p);
    uint8_t *d;
    uint32_t seq = c->seq >= 0x7FFFFFFFu ? 1u : c->seq + 1u;
    Msg *m = o ? stream_chunk_new(vm, p->id, seq, (uint32_t)now, 0, (uint32_t)r, &d) : NULL;
    if (m) {
        memcpy(d, buf, (size_t)r);
        if (msg_try_enqueue(vm, o, m)) {
            c->seq = seq;
            (void)recv(c->fd, buf, (size_t)r, 0);
            return true;
        }
        msg_free(vm, m);
    }
    c->retry_at = now + RETRY_MS;      /* no room: the kernel keeps the bytes */
    return true;
}

static void conn_step(Vm *vm, Port *p, HConn *c, uint64_t now) {
    short ev = c->revents;
    c->revents = 0;
    if (c->connecting) {
        if (ev & (POLLOUT | POLLERR | POLLHUP)) {
            int err = 0;
            socklen_t el = sizeof err;
            getsockopt(c->fd, SOL_SOCKET, SO_ERROR, &err, &el);
            c->connecting = 0;
            if (err) end(vm, p, c, err == ETIMEDOUT ? GOGO_E_TIMEOUT : GOGO_E_REFUSED);
            else c->connected_owed = 1;
        } else if (c->deadline && now >= c->deadline) {
            end(vm, p, c, GOGO_E_TIMEOUT);
        }
    }
    if (c->connected_owed) {
        if (!event_new(vm, p, TYPE_NET_CONNECTED, 0, false, 0)) {
            c->retry_at = now + RETRY_MS;
            return;
        }
        c->connected_owed = 0;
    }
    stream_out_flush(vm, p, &c->so);
    if (!c->dead && !c->connecting) {
        if ((ev & POLLOUT) && !write_outs(vm, p, c)) end(vm, p, c, GOGO_E_RESET);
        if (!c->dead && !c->rd_done && !c->paused && !c->closed_owed && (ev & (POLLIN | POLLHUP | POLLERR)) &&
            now >= c->retry_at && !read_chunk(vm, p, c, now))
            end(vm, p, c, GOGO_E_RESET);
    }
    if (c->closed_owed && !c->paused) {
        if (event_new(vm, p, TYPE_CLOSED, p->id, true, c->closed_reason)) c->closed_owed = 0;
        else c->retry_at = now + RETRY_MS;
    }
}

/* ---- tcp_listen ---- */

static int l_open(struct Vm *vm, Port *p, uint16_t index, const int32_t *cfg, uint32_t n) {
    (void)vm; (void)p;
    if (!conns_reach(index)) return -1;
    HConn *c = &conns[index];
    memset(c, 0, sizeof *c);
    c->fd = -1;
    if (n < 1u || cfg[0] <= 0 || cfg[0] > 65535) return -1;
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) return -1;
    int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    set_nonblock(fd);
    struct sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((uint16_t)cfg[0]);
    sa.sin_addr.s_addr = htonl(INADDR_ANY);
    if (bind(fd, (struct sockaddr *)&sa, sizeof sa) != 0 || listen(fd, 8) != 0) {
        close(fd);
        return -1;
    }
    c->fd = fd;
    c->kind = K_LISTEN;
    return 0;
}

static void l_close(struct Vm *vm, Port *p) {
    HConn *c = conn_of(vm ? vm : hvm, p);
    if (c->fd >= 0) close(c->fd);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

static bool l_can_event(const struct Vm *vm, const Port *p) { (void)vm; (void)p; return true; }

static void listen_step(Vm *vm, Port *p, HConn *c, uint64_t now) {
    if (!(c->revents & POLLIN) || now < c->retry_at) return;
    c->revents = 0;
    Actor *o = port_owner(vm, p);
    if (!o) return;
    int fd = accept(c->fd, NULL, NULL);
    if (fd < 0) return;
    set_nonblock(fd);
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof one);
    accept_fd = fd;
    term conn = port_open_with(vm, o, &hostnet_tcp_driver, NULL, 0);
    if (accept_fd >= 0) {              /* not taken */
        close(accept_fd);
        accept_fd = -1;
    }
    if (conn == T_NIL) {
        c = &conns[port_index(vm, p)];   /* the open may have moved conns (conns_reach) */
        c->retry_at = now + RETRY_MS;
        return;
    }
    if (!event_new(vm, p, TYPE_NET_ACCEPTED, conn, false, 0)) port_close(vm, conn);
}

const PortDriver hostnet_tcp_driver = {
    .name = PORT_NAME_TCP, .cfg_type = TYPE_NET_DIAL, .event_type = TYPE_NET_CONNECTED, .open = t_open,
    .close = t_close, .can_event = t_can_event, .handoff = t_handoff, .command_term = t_command
};

const PortDriver hostnet_listen_driver = {
    .name = PORT_NAME_TCP_LISTEN, .cfg_type = TYPE_NET_LISTEN, .event_type = TYPE_NET_ACCEPTED, .open = l_open,
    .close = l_close, .can_event = l_can_event
};

/* ---- carried node links (DistCarrier) ---- */

/* Sends what it can of c's queue; false if the socket failed. */
static bool carried_flush(HConn *c) {
    while (c->out_len) {
        ssize_t w = send(c->fd, c->out, c->out_len, 0);
        if (w < 0) return errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR;
        memmove(c->out, c->out + w, c->out_len - (uint32_t)w);
        c->out_len -= (uint32_t)w;
    }
    return true;
}

static bool carried_queue(HConn *c, const uint8_t *d, uint32_t n) {
    if (c->out_len + n > CARRY_OUT_MAX) return false;
    if (c->out_len + n > c->out_cap) {
        uint32_t cap = c->out_cap ? c->out_cap : 4096u;
        while (cap < c->out_len + n) cap *= 2u;
        uint8_t *b = realloc(c->out, cap);
        if (!b) return false;
        c->out = b;
        c->out_cap = cap;
    }
    memcpy(c->out + c->out_len, d, n);
    c->out_len += n;
    return true;
}

static void carried_step(Vm *vm, HConn *c) {
    short ev = c->revents;
    c->revents = 0;
    Port *node = c->node;
    if ((ev & POLLOUT) && !carried_flush(c)) {
        dist_carried_end(vm, node);
        return;
    }
    if (c->paused || !(ev & (POLLIN | POLLHUP | POLLERR))) return;
    uint8_t buf[4096];
    for (int k = 0; k < 16; k++) {            /* at most 64 KB a pass */
        ssize_t r = recv(c->fd, buf, sizeof buf, 0);
        if (r < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) return;
        if (r <= 0) {
            dist_carried_end(vm, node);
            return;
        }
        dist_carried_input(vm, node, buf, (uint32_t)r);
        if (c->kind != K_CARRIED || c->node != node) return;   /* the link ended */
    }
}

static bool car_take(Vm *vm, term conn, Port *node, void **ctx) {
    uint32_t ci = term_pid_slot(conn) - vm->port_base, ni = port_index(vm, node);
    if (!term_is_pid(conn) || term_pid_slot(conn) < vm->port_base || ci >= lim(vm) || !conns_reach(ni)) return false;
    Port *cp = port_at(vm, ci);
    HConn *c = &conns[ci], *n = &conns[ni];
    if (cp->id != conn || cp->drv != &hostnet_tcp_driver || cp->owner != node->owner || c->fd < 0 || c->dead ||
        c->connecting || c->rd_done || c->closed_owed)
        return false;
    memset(n, 0, sizeof *n);
    n->fd = c->fd;
    n->kind = K_CARRIED;
    n->node = node;
    n->paused = 1;
    /* What conn had not sent yet goes first. */
    for (uint32_t k = 0; k < c->so.n; k++) {
        uint32_t len;
        const uint8_t *d = stream_out_at(vm, &c->so, k, &len);
        if (d) carried_queue(n, d + (k ? 0 : c->off), len - (k ? 0 : c->off));
    }
    c->fd = -1;
    port_close(vm, conn);
    *ctx = (void *)(uintptr_t)ni;
    return true;
}

static bool car_write(Vm *vm, void *ctx, const uint8_t *d, uint32_t n) {
    (void)vm;
    HConn *c = &conns[(uintptr_t)ctx];
    if (!c->out_len) {                        /* try at once: no wait for a poll */
        ssize_t w = send(c->fd, d, n, 0);
        if (w > 0) {
            d += w;
            n -= (uint32_t)w;
        }
        if (!n) return true;
    }
    return carried_queue(c, d, n);
}

static void car_resume(Vm *vm, void *ctx) {
    (void)vm;
    conns[(uintptr_t)ctx].paused = 0;
}

static void car_release(Vm *vm, void *ctx) {
    (void)vm;
    HConn *c = &conns[(uintptr_t)ctx];
    if (c->fd >= 0) close(c->fd);
    free(c->out);
    memset(c, 0, sizeof *c);
    c->fd = -1;
}

const DistCarrier hostnet_carrier = { car_take, car_write, car_resume, car_release };

/* ---- the event source ---- */


static short interest(const Vm *vm, uint32_t i, uint64_t now) {
    const Port *p = port_at(vm, i);
    const HConn *c = &conns[i];
    if (c->kind == K_CARRIED) return (short)((c->paused ? 0 : POLLIN) | (c->out_len ? POLLOUT : 0));
    if (!p->drv || c->fd < 0 || (p->drv != &hostnet_tcp_driver && p->drv != &hostnet_listen_driver)) return 0;
    if (now < c->retry_at) return 0;
    if (c->kind == K_LISTEN) return POLLIN;
    if (c->dead) return 0;
    if (c->connecting) return POLLOUT;
    short e = 0;
    if (!c->rd_done && !c->paused && !c->closed_owed) e |= POLLIN;
    if (c->so.n) e |= POLLOUT;
    return e;
}

/* Polls the sockets (no wait); true if any is ready. */
static bool poll_now(const Vm *vm) {
    struct pollfd *p = pfds;
    uint32_t *idx = pidx, n = 0;
    uint64_t now = vm_now(vm);
    for (uint32_t i = 0; i < lim(vm); i++) {
        short e = interest(vm, i, now);
        if (!e) continue;
        p[n].fd = conns[i].fd;
        p[n].events = e;
        p[n].revents = 0;
        idx[n++] = i;
    }
    if (!n || poll(p, n, 0) <= 0) return false;
    bool any = false;
    for (uint32_t k = 0; k < n; k++) {
        conns[idx[k]].revents |= p[k].revents;
        any |= p[k].revents != 0;
    }
    return any;
}

static bool owes(const Vm *vm, uint32_t i, uint64_t now) {
    if (conns[i].kind == K_CARRIED) return conns[i].revents != 0;
    const Port *p = port_at(vm, i);
    const HConn *c = &conns[i];
    if (!p->drv || p->drv != &hostnet_tcp_driver || now < c->retry_at) return false;
    return c->connected_owed || (c->closed_owed && !c->paused) || c->revents ||
           (c->so.owed_n != 0) || (c->connecting && c->deadline && now >= c->deadline);
}

static bool s_pending(const Vm *vm, void *ctx) {
    (void)ctx;
    uint64_t now = vm_now(vm);
    for (uint32_t i = 0; i < lim(vm); i++) {
        if (owes(vm, i, now) || (conns[i].kind == K_LISTEN && conns[i].revents)) return true;
    }
    return poll_now(vm);
}

static void s_drain(Vm *vm, void *ctx, uint64_t now) {
    (void)ctx;
    for (uint32_t i = 0; i < lim(vm); i++) {
        Port *p = port_at(vm, i);
        HConn *c = &conns[i];
        if (!p->drv || c->fd < 0) continue;
        if (c->kind == K_CARRIED) carried_step(vm, c);
        else if (p->drv == &hostnet_tcp_driver) conn_step(vm, p, c, now);
        else if (p->drv == &hostnet_listen_driver) listen_step(vm, p, c, now);
    }
}

static bool s_any_live(const Vm *vm, void *ctx) { (void)vm; (void)ctx; return false; }

static bool s_next_deadline(const Vm *vm, void *ctx, uint64_t *deadline) {
    (void)ctx;
    bool have = false;
    for (uint32_t i = 0; i < lim(vm); i++) {
        const Port *p = port_at(vm, i);
        const HConn *c = &conns[i];
        if (!p->drv || c->fd < 0 || (p->drv != &hostnet_tcp_driver && p->drv != &hostnet_listen_driver)) continue;
        uint64_t d = 0;
        if (c->retry_at) d = c->retry_at;
        if (c->connecting && c->deadline && (!d || c->deadline < d)) d = c->deadline;
        if (d && (!have || d < *deadline)) {
            *deadline = d;
            have = true;
        }
    }
    return have;
}

static const PortSource hostnet_source = { s_pending, s_drain, s_any_live, s_next_deadline, NULL };

/* platform_wait's extra fds: every socket with something to wait for
 * (p NULL: how many there are). */
static int extra_fds(struct pollfd *p, int max) {
    int n = 0;
    if (!hvm) return 0;
    uint64_t now = vm_now(hvm);
    for (uint32_t i = 0; i < lim(hvm) && (!p || n < max); i++) {
        short e = interest(hvm, i, now);
        if (!e) continue;
        if (!p) {
            n++;
            continue;
        }
        p[n].fd = conns[i].fd;
        p[n].events = e;
        p[n].revents = 0;
        n++;
    }
    return n;
}

void hostnet_install(Vm *vm) {
    hvm = vm;
    platform_unix_extra_fds = extra_fds;
    (void)port_add_source(vm, &hostnet_source, NULL);
}
