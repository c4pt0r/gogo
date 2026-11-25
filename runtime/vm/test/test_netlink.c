#define _POSIX_C_SOURCE 200809L
/* unix net link (platform_unix.c, net_unix.h): framing both ways over a
 * socketpair, and the simulated clock driven by advance messages. */
#include "test.h"
#include "platform_unix.h"
#include "netif.h"
#include "net_unix.h"
#include <string.h>
#include <stdint.h>
#include <unistd.h>
#include <poll.h>
#include <sys/socket.h>
#include <pthread.h>

static uint8_t got[64];
static uint32_t got_len;
static int got_n;

static void sink(const uint8_t *f, uint32_t n) {
    got_n++;
    got_len = n;
    memcpy(got, f, n < sizeof got ? n : sizeof got);
}

static void put(int fd, uint8_t type, const uint8_t *p, uint32_t n) {
    uint8_t h[3] = { (uint8_t)(n >> 8), (uint8_t)n, type };
    CHECK_EQ(write(fd, h, 3), 3);
    if (n) CHECK_EQ(write(fd, p, n), (long long)n);
}

static void advance(int fd, uint32_t ms) {
    uint8_t b[4] = { (uint8_t)(ms >> 24), (uint8_t)(ms >> 16), (uint8_t)(ms >> 8), (uint8_t)ms };
    put(fd, NET_MSG_ADVANCE, b, 4);
}

/* Reads one message (header + up to cap bytes) from fd; -1 if none within 1 s. */
static int take(int fd, uint8_t *type, uint8_t *p, uint32_t cap) {
    struct pollfd q = { fd, POLLIN, 0 };
    if (poll(&q, 1, 1000) != 1) return -1;
    uint8_t h[3];
    if (read(fd, h, 3) != 3) return -1;
    uint32_t n = ((uint32_t)h[0] << 8) | h[1];
    *type = h[2];
    if (n > cap || (n && read(fd, p, n) != (ssize_t)n)) return -1;
    return (int)n;
}

static bool readable(int fd) {
    struct pollfd q = { fd, POLLIN, 0 };
    return poll(&q, 1, 0) == 1;
}

TEST(netlink_frames_both_ways) {
    int sv[2];
    CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    uint8_t mac[6];
    CHECK(!unix_netif.mac(NULL, mac));
    CHECK(!unix_netif.link_up(NULL));
    platform_unix_net_open(sv[0], false, NULL);
    platform_unix_net_set_sink(sink);
    got_n = 0;
    CHECK(unix_netif.mac(NULL, mac));
    CHECK_EQ(mac[0], 0x02);
    CHECK(unix_netif.link_up(NULL));

    /* VM -> peer. */
    const uint8_t out[5] = { 1, 2, 3, 4, 5 };
    CHECK_EQ(unix_netif.send(NULL, out, 5), 0);
    uint8_t t, buf[64];
    CHECK_EQ(take(sv[1], &t, buf, sizeof buf), 5);
    CHECK_EQ(t, NET_MSG_FRAME);
    CHECK(memcmp(buf, out, 5) == 0);

    /* peer -> VM: one frame per wait. */
    const uint8_t in1[3] = { 9, 8, 7 }, in2[2] = { 6, 5 };
    put(sv[1], NET_MSG_FRAME, in1, 3);
    put(sv[1], NET_MSG_FRAME, in2, 2);
    uint32_t before = platform_unix_net_rx_count();
    platform_wait(UINT32_MAX);
    CHECK_EQ(got_n, 1);
    CHECK_EQ(got_len, 3u);
    CHECK(memcmp(got, in1, 3) == 0);
    platform_wait(UINT32_MAX);
    CHECK_EQ(got_n, 2);
    CHECK_EQ(got_len, 2u);
    CHECK_EQ(got[0], 6);
    CHECK_EQ(platform_unix_net_rx_count() - before, 2u);

    /* Real clock: an advance is acknowledged at once. */
    advance(sv[1], 1000);
    platform_wait(UINT32_MAX);
    CHECK_EQ(take(sv[1], &t, buf, sizeof buf), 0);
    CHECK_EQ(t, NET_MSG_ACK);

    platform_unix_net_set_sink(NULL);
    platform_unix_net_close();
    close(sv[0]);
    close(sv[1]);
}

/* Peer thread: waits for the VM's ACK, then sends one frame (which ends
 * the VM's idle block). */
static int peer_fd, peer_acks;
static void *peer(void *arg) {
    (void)arg;
    uint8_t t, buf[8];
    peer_acks = 0;
    if (take(peer_fd, &t, buf, sizeof buf) == 0 && t == NET_MSG_ACK) peer_acks++;
    const uint8_t f[1] = { 77 };
    put(peer_fd, NET_MSG_FRAME, f, 1);
    return NULL;
}

/* One platform_wait(ms) that is expected to idle: the peer thread sees the
 * ACK and wakes it with a frame. */
static void idle_wait(int fd, uint32_t ms) {
    pthread_t th;
    peer_fd = fd;
    int n = got_n;
    CHECK_EQ(pthread_create(&th, NULL, peer, NULL), 0);
    platform_wait(ms);
    pthread_join(th, NULL);
    CHECK_EQ(peer_acks, 1);
    CHECK_EQ(got_n, n + 1);
    CHECK_EQ(got[0], 77);
}

TEST(netlink_sim_clock) {
    int sv[2];
    CHECK_EQ(socketpair(AF_UNIX, SOCK_STREAM, 0, sv), 0);
    platform_unix_net_open(sv[0], true, NULL);
    platform_unix_net_set_sink(sink);
    got_n = 0;
    CHECK_EQ(platform_now_ms(), 0u);

    advance(sv[1], 100);
    platform_wait(30);                 /* reads the advance; time not moved yet */
    CHECK_EQ(platform_now_ms(), 0u);
    platform_wait(30);                 /* a 30 ms sleep, in simulated time */
    CHECK_EQ(platform_now_ms(), 30u);
    platform_wait(30);
    CHECK_EQ(platform_now_ms(), 60u);
    CHECK(!readable(sv[1]));           /* no ACK while time is still owed */

    /* Input comes first, before any step. */
    const uint8_t f[1] = { 42 };
    put(sv[1], NET_MSG_FRAME, f, 1);
    platform_wait(70);
    CHECK_EQ(got_n, 1);
    CHECK_EQ(got[0], 42);
    CHECK_EQ(platform_now_ms(), 60u);

    /* 70 > the 40 left: time goes to the target, one ACK, then the wait
     * blocks until input. */
    idle_wait(sv[1], 70);
    CHECK_EQ(platform_now_ms(), 100u);

    /* No timeout at all: same. Two advances, one ACK. */
    advance(sv[1], 10);
    advance(sv[1], 15);
    platform_wait(UINT32_MAX);
    platform_wait(UINT32_MAX);
    CHECK_EQ(platform_now_ms(), 100u);
    idle_wait(sv[1], UINT32_MAX);
    CHECK_EQ(platform_now_ms(), 125u);
    CHECK(!readable(sv[1]));

    /* A sync (advance 0) at the target: ACK at the next idle wait. */
    advance(sv[1], 0);
    platform_wait(5);
    idle_wait(sv[1], 5);
    CHECK_EQ(platform_now_ms(), 125u);

    /* A notify ends a wait without moving time. */
    platform_notify();
    platform_wait(UINT32_MAX);
    CHECK_EQ(platform_now_ms(), 125u);
    CHECK(!readable(sv[1]));

    platform_unix_net_set_sink(NULL);
    platform_unix_net_close();
    close(sv[0]);
    close(sv[1]);
}
