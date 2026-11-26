/* Two VMs in one process, each with its own board (driver instances over
 * HAL tables whose ctx is that board's chips) and InputState: key events,
 * power samples, open ports and the drop counters never cross. */
#include "test.h"
#include "test_host.h"
#include "input_int.h"
#include "actor.h"
#include "heap.h"
#include "msg.h"
#include "alloc.h"
#include "image.h"
#include "opcodes.h"
#include "port.h"
#include "bytes.h"
#include <string.h>

/* ---- two tiny boards: one key each, a power chip each ---- */

typedef struct {
    struct { uint16_t raw; bool down; } q[8];
    unsigned qh, qt;
    int32_t mv;
    int opens, closes;
} Hw;
static Hw hw[2];

static const KbdKey keys[2] = { { 0x04, 0 }, { 0x05, 0 } };   /* raw 0 = 'a', raw 1 = 'b' */

/* One set of hooks; ctx says which board's chips. */
static int kopen(void *ctx) { ((Hw *)ctx)->opens++; return 0; }
static void kclose(void *ctx) { ((Hw *)ctx)->closes++; }
static bool kpend(void *ctx) { const Hw *h = ctx; return h->qh != h->qt; }
static int kread(void *ctx, uint16_t *raw, bool *down) {
    Hw *h = ctx;
    if (h->qh == h->qt) return 0;
    *raw = h->q[h->qh % 8].raw; *down = h->q[h->qh % 8].down; h->qh++;
    return 1;
}
static int pread(void *ctx, int32_t *mv, int8_t *ch, int8_t *ext) {
    *mv = ((Hw *)ctx)->mv; *ch = 0; *ext = 0; return 0;
}

static const KbdHal kbd_hal[2] = {
    { &hw[0], keys, 2, kopen, kclose, kpend, kread, NULL },
    { &hw[1], keys, 2, kopen, kclose, kpend, kread, NULL },
};
static const PowerHal power_hal[2] = {
    { &hw[0], pread, NULL, NULL, NULL, false },
    { &hw[1], pread, NULL, NULL, NULL, false },
};
static const PortDriver kbd_drv[2] = { INPUT_KBD_DRIVER(&kbd_hal[0]), INPUT_KBD_DRIVER(&kbd_hal[1]) };
static const PortDriver power_drv[2] = { INPUT_POWER_DRIVER(&power_hal[0]), INPUT_POWER_DRIVER(&power_hal[1]) };
static const PortDriver *const drivers[2][3] = {
    { &kbd_drv[0], &power_drv[0], NULL },
    { &kbd_drv[1], &power_drv[1], NULL },
};
static InputState st[2];
static Vm vm[2];

static void push(int i, uint16_t raw, bool down) {
    hw[i].q[hw[i].qt % 8].raw = raw;
    hw[i].q[hw[i].qt % 8].down = down;
    hw[i].qt++;
}

/* ---- VM helpers ---- */

#define MINI_WORDS 25
static uint32_t img_w[MINI_WORDS];
static Image img;

static void mini_image(void) {
    uint32_t *w = img_w;
    memset(w, 0, sizeof img_w);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 2;
    w[4] = 6; w[5] = 1; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (1u << 16); w[10] = 0;
    w[11] = 2; w[12] = 3; w[13] = 2; w[14] = 3;
    w[15] = hdr_make(3, K_STRING); w[16] = 4; memcpy(&w[17], "main", 4);
    w[18] = hdr_make(3, K_STRING); w[19] = 3; memcpy(&w[20], "Cfg", 3);
    w[21] = OP_MAKE(OP_RET, 0, 0, 0);
    CHECK_EQ(image_load(&img, w, img_seal(w)), IMG_OK);
}

static uint64_t now;
static uint64_t clock_fn(void) { return now; }

static term heap_str(Vm *v, Actor *a, const char *s) {
    uint32_t len = (uint32_t)strlen(s), words = 2u + (len + 3u) / 4u;
    uint32_t off = heap_alloc(v, a, words, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(words, K_STRING);
    a->mem[off + 1] = len;
    a->mem[off + words - 1] = 0;
    memcpy(a->mem + off + 2, s, len);
    return term_from_ref(off);
}

static term heap_cfg(Vm *v, Actor *a, uint32_t type, int x, int y) {
    uint32_t off = heap_alloc(v, a, 4, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(4, K_STRUCT);
    a->mem[off + 1] = type;
    a->mem[off + 2] = term_from_int(x);
    a->mem[off + 3] = term_from_int(y);
    return term_from_ref(off);
}

static term open_port(Vm *v, Actor *a, const char *name, uint32_t cfg_type) {
    a->hp = 0;
    term n = heap_str(v, a, name);
    return port_open(v, a, n, heap_cfg(v, a, cfg_type, 0, 0));
}

static int mbox_len(const Actor *a) {
    int n = 0;
    for (const Msg *m = a->mbox_head; m; m = m->next) n++;
    return n;
}

/* The int field i of a's nth message. */
static int32_t field(const Actor *a, int nth, int i) {
    const Msg *m = a->mbox_head;
    while (m && nth--) m = m->next;
    CHECK(m != NULL);
    return m ? term_to_int(m->data[2 + i]) : -999;
}

TEST(input_two_vms_do_not_cross) {
    test_host_arena_forget();
    test_host_set_clock(clock_fn);
    now = 0;
    mini_image();
    memset(hw, 0, sizeof hw);
    hw[0].mv = 3700;
    hw[1].mv = 4100;
    Actor *own[2];
    term kbd[2], pwr[2];
    for (int i = 0; i < 2; i++) {
        va_reset(&vm[i]);
        bytes_reset(&vm[i]);
        CHECK_EQ(test_vm_init(&vm[i], &img, 16), 0);
        port_set_drivers(&vm[i], drivers[i]);
        input_install(&vm[i], &st[i]);
        CHECK(vm[i].input == &st[i]);
        own[i] = actor_new(&vm[i], 256);
        CHECK(own[i] != NULL);
        own[i]->state = A_WAITING;
        kbd[i] = open_port(&vm[i], own[i], "kbd", TYPE_KBD_CONFIG);
        CHECK(test_port_is(kbd[i]));
        CHECK_EQ(hw[i].opens, 1);
    }
    CHECK(st[0].kbd.port != st[1].kbd.port);

    /* A key on board 0 reaches only vm 0's owner, and the other way round. */
    push(0, 0, true);
    port_drain(&vm[0], 1);
    port_drain(&vm[1], 1);
    CHECK_EQ(mbox_len(own[0]), 1);
    CHECK_EQ(mbox_len(own[1]), 0);
    CHECK_EQ(field(own[0], 0, 0), 0x04);
    CHECK_EQ(st[0].kbd.nheld, 1);
    CHECK_EQ(st[1].kbd.nheld, 0);
    push(1, 1, true);
    port_drain(&vm[0], 2);
    port_drain(&vm[1], 2);
    CHECK_EQ(mbox_len(own[0]), 1);
    CHECK_EQ(mbox_len(own[1]), 1);
    CHECK_EQ(field(own[1], 0, 0), 0x05);

    /* Power: each samples its own chip. */
    for (int i = 0; i < 2; i++) {
        pwr[i] = open_port(&vm[i], own[i], "power", TYPE_POWER_CONFIG);
        CHECK(test_port_is(pwr[i]));
    }
    port_drain(&vm[0], 3);
    port_drain(&vm[1], 3);
    CHECK_EQ(mbox_len(own[0]), 2);
    CHECK_EQ(mbox_len(own[1]), 2);
    CHECK_EQ(field(own[0], 1, 0), 3700);
    CHECK_EQ(field(own[1], 1, 0), 4100);

    /* Closing vm 0's kbd leaves vm 1's open; the counters are per Vm. */
    port_close(&vm[0], kbd[0]);
    CHECK_EQ(hw[0].closes, 1);
    CHECK_EQ(hw[1].closes, 0);
    CHECK(st[0].kbd.port == NULL);
    CHECK(st[1].kbd.port != NULL);
    st[1].dropped = 5;
    CHECK_EQ(input_dropped(&vm[0]), 0u);
    CHECK_EQ(input_dropped(&vm[1]), 5u);

    /* A Vm without input_install has no input ports. */
    Vm bare;
    va_reset(&bare);
    bytes_reset(&bare);
    CHECK_EQ(test_vm_init(&bare, &img, 16), 0);
    port_set_drivers(&bare, drivers[0]);
    Actor *b = actor_new(&bare, 256);
    CHECK(b != NULL);
    CHECK(!test_port_is(open_port(&bare, b, "kbd", TYPE_KBD_CONFIG)));
    CHECK(input_failed(&bare));

    port_close_all(&bare);
    for (int i = 0; i < 2; i++) port_close_all(&vm[i]);
    test_host_set_clock(NULL);
}
