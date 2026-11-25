/* B_FS (vfs/builtin.c) on a real VM against the memfs fake: every op's
 * results and Err numbers, the `file` port (owner check, charge, Handoff,
 * owner exit), Read's limits, the frame checks of the hook, and the
 * interpreter's B_FS case. Calls go straight through vm_fs_builtin on an
 * actor with an NREGS-register frame: r0 (r1) = B_FS(r2 = op, r3.. = args). */
#include "test.h"
#include "actor.h"
#include "heap.h"
#include "alloc.h"
#include "image.h"
#include "interp.h"
#include "opcodes.h"
#include "signals.h"
#include "port.h"
#include "bytes.h"
#include "evq.h"
#include "platform.h"
#include "term.h"
#include "vfs.h"
#include "memfs.h"
#include <string.h>
#include <stdarg.h>
#include <stdio.h>

extern void (*platform_unix_err_hook)(const char *, size_t);
static char err_buf[256]; static size_t err_len;
static void capture_err(const char *s, size_t n) {
    if (err_len + n < sizeof err_buf) { memcpy(err_buf + err_len, s, n); err_len += n; err_buf[err_len] = 0; }
}

#define NREGS 8u
#define CHARGE (GOGO_FS_SLOT_BYTES / 4u)

/* An image with the one const "main" (for direct calls; no code runs). */
#define IMG_WORDS 18
static uint32_t w[IMG_WORDS];
static Image img;
static Vm vm;

static void setup(void) {
    va_reset();
    evq_reset();
    vfs_reset();
    memfs_init(0, false, true, 4000);        /* /flash: LittleFS-like, budget */
    memfs_init(1, true, false, -1);          /* /sd: FAT-like */
    CHECK(vfs_mount("/flash", &memfs_be[0]));
    CHECK(vfs_mount("/sd", &memfs_be[1]));
    vfs_install();
    memset(w, 0, sizeof w);
    w[0] = IMAGE_MAGIC; w[1] = IMAGE_VERSION; w[2] = 1; w[3] = 0;
    w[4] = 3; w[5] = 1; w[6] = 0; w[7] = 0;
    w[8] = 0; w[9] = 0 | (NREGS << 16); w[10] = 0;
    w[11] = hdr_make(3, K_STRING); w[12] = 4; memcpy(&w[13], "main", 4);
    w[14] = OP_MAKE(OP_RET, 0, 0, 0);
    w[15] = 1u;                              /* v7 const map: "main" at 0 */
    CHECK_EQ(image_load(&img, w, IMG_WORDS), IMG_OK);
    CHECK_EQ(vm_init(&vm, &img, 8), 0);
}

static Actor *actor(uint16_t max_words) {
    Actor *a = actor_new(&vm, 64);
    CHECK(a != NULL);
    a->max_words = max_words;
    a->state = A_WAITING;
    CHECK(heap_push_frame(&vm, a, NREGS, -1));
    return a;
}

static void done(Actor *a) { actor_exit(&vm, a, R_NORMAL, T_EMPTYSTR); }

#define R(a) ((term *)(a)->mem + (a)->fp)

static term heap_obj(Actor *a, uint32_t kind, const void *p, uint32_t len) {
    uint32_t words = 2u + (len + 3u) / 4u;
    uint32_t off = heap_alloc(&vm, a, words, NULL, 0);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(words, kind);
    a->mem[off + 1] = len;
    a->mem[off + words - 1] = 0;
    memcpy(a->mem + off + 2, p, len);
    return term_from_ref(off);
}

/* A []byte of s (a K_SLICE over a K_BYTEARR). */
static term byteslice(Actor *a, const char *s) {
    uint32_t len = (uint32_t)strlen(s);
    term arr = heap_obj(a, K_BYTEARR, s, len);
    uint32_t off = heap_alloc(&vm, a, 5, &arr, 1);
    CHECK(off != UINT32_MAX);
    a->mem[off] = hdr_make(5, K_SLICE);
    a->mem[off + 1] = 0; a->mem[off + 2] = len; a->mem[off + 3] = len; a->mem[off + 4] = arr;
    return term_from_ref(off);
}

/* r0 (r1) = B_FS(op, args): fmt per argument: 's' C string (heap string),
 * 'B' C string as a heap Bytes, 'b' C string as a []byte, 'i' int, 'I' boxed
 * int, 't' term. */
static uint32_t fs_at(Actor *a, uint32_t ra, int op, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    uint32_t n = 0;
    R(a)[2] = term_from_int(op);
    for (const char *f = fmt; *f; f++, n++) {
        term t;
        switch (*f) {
        case 's': { const char *s = va_arg(ap, const char *); t = heap_obj(a, K_STRING, s, (uint32_t)strlen(s)); break; }
        case 'B': { const char *s = va_arg(ap, const char *); t = heap_obj(a, K_BYTES, s, (uint32_t)strlen(s)); break; }
        case 'b': t = byteslice(a, va_arg(ap, const char *)); break;
        case 'i': t = term_from_int(va_arg(ap, int)); break;
        case 'I': {                                            /* boxed K_INT */
            int32_t v = va_arg(ap, int);
            uint32_t off = heap_alloc(&vm, a, 2, NULL, 0);
            CHECK(off != UINT32_MAX);
            a->mem[off] = hdr_make(2, K_INT); a->mem[off + 1] = (uint32_t)v;
            t = term_from_ref(off);
            break;
        }
        default:  t = va_arg(ap, term); break;
        }
        R(a)[3 + n] = t;
    }
    va_end(ap);
    R(a)[0] = R(a)[1] = T_NIL;
    return vm_fs_builtin(&vm, a, ra, 2, 1 + n, NREGS);
}
#define fs(a, op, ...) fs_at(a, 0, op, __VA_ARGS__)

static int32_t ival(term t) { CHECK(term_is_int(t)); return term_to_int(t); }
#define ERR1(a) ival(R(a)[0])      /* single-result ops */
#define ERR2(a) ival(R(a)[1])      /* two-result ops */

static bool bytes_is(Actor *a, term t, const char *s) {
    const uint8_t *p;
    uint32_t n;
    if (!bytes_view(&vm, a, t, &p, &n)) return false;
    return n == strlen(s) && (n == 0 || memcmp(p, s, n) == 0);
}
static uint32_t bytes_len(Actor *a, term t) {
    const uint8_t *p;
    uint32_t n;
    CHECK(bytes_view(&vm, a, t, &p, &n));
    return n;
}

static int open_slots(void) {
    int n = 0;
    for (int i = 0; i < (int)GOGO_FS_MAX_OPEN; i++) n += vfs_slot_mode(i) >= 0;
    return n;
}
static int open_ports(void) {
    int n = 0;
    for (unsigned i = 0; i < GOGO_MAX_PORTS; i++) n += vm.ports[i].drv != NULL;
    return n;
}

/* A file of n bytes '0'..'9' repeating, written straight through vfs. */
static void make_file(const char *path, uint32_t n) {
    static uint8_t buf[4200];
    int s;
    for (uint32_t i = 0; i < n; i++) buf[i] = (uint8_t)('0' + i % 10u);
    CHECK_EQ(vfs_open(path, (uint32_t)strlen(path), VFS_WRITE, &s), VFS_OK);
    CHECK_EQ(vfs_write(s, buf, n), VFS_OK);
    vfs_close(s);
}

TEST(fs_open_write_read) {
    setup();
    Actor *a = actor(0);
    uint32_t base = actor_charged_words(a);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/a", VFS_WRITE), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_OK);
    term f = R(a)[0];
    CHECK(port_is(f));
    CHECK_EQ(actor_charged_words(a), base + CHARGE);
    CHECK_EQ(open_slots(), 1);
    CHECK_EQ(fs(a, FS_WRITE, "ts", f, "hel"), R_NORMAL);       /* string */
    CHECK_EQ(ERR1(a), VFS_OK);
    CHECK_EQ(fs(a, FS_WRITE, "tB", f, "lo"), R_NORMAL);        /* Bytes */
    CHECK_EQ(ERR1(a), VFS_OK);
    CHECK_EQ(fs(a, FS_WRITE, "tb", f, "!!"), R_NORMAL);        /* []byte */
    CHECK_EQ(ERR1(a), VFS_OK);
    CHECK_EQ(fs(a, FS_WRITE, "tt", f, T_EMPTYBYTES), R_NORMAL);
    CHECK_EQ(fs(a, FS_WRITE, "tt", f, T_NILSLICE), R_NORMAL);
    CHECK_EQ(fs(a, FS_WRITE, "tt", f, T_EMPTYSTR), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_OK);
    CHECK_EQ(fs(a, FS_WRITE, "ti", f, 5), R_BAD_CODE);         /* not data */
    CHECK_EQ(fs(a, FS_SYNC, "t", f), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_OK);
    CHECK_EQ(fs(a, FS_SIZE, "t", f), R_NORMAL);
    CHECK_EQ(ival(R(a)[0]), 7);
    CHECK_EQ(ERR2(a), VFS_OK);
    CHECK_EQ(fs(a, FS_READ, "ti", f, 10), R_NORMAL);          /* write-mode file */
    CHECK_EQ(R(a)[0], T_EMPTYBYTES);
    CHECK_EQ(ERR2(a), VFS_CLOSED);
    port_close(&vm, f);                                        /* f.Close() */
    CHECK_EQ(actor_charged_words(a), base);
    CHECK_EQ(open_slots(), 0);

    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/a", VFS_READ), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_OK);
    f = R(a)[0];
    CHECK_EQ(fs(a, FS_READ, "ti", f, 3), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_OK);
    CHECK(bytes_is(a, R(a)[0], "hel"));
    CHECK_EQ(fs(a, FS_READ, "ti", f, 0), R_NORMAL);           /* max <= 0: empty, OK */
    CHECK_EQ(R(a)[0], T_EMPTYBYTES);
    CHECK_EQ(ERR2(a), VFS_OK);
    CHECK_EQ(fs(a, FS_READ, "ti", f, -5), R_NORMAL);
    CHECK_EQ(R(a)[0], T_EMPTYBYTES);
    CHECK_EQ(ERR2(a), VFS_OK);
    CHECK_EQ(fs(a, FS_READ, "ti", f, 100), R_NORMAL);
    CHECK(bytes_is(a, R(a)[0], "lo!!"));
    CHECK_EQ(fs(a, FS_READ, "ti", f, 100), R_NORMAL);         /* end of file */
    CHECK_EQ(R(a)[0], T_EMPTYBYTES);
    CHECK_EQ(ERR2(a), VFS_OK);
    CHECK_EQ(fs(a, FS_WRITE, "ts", f, "x"), R_NORMAL);        /* read-mode file */
    CHECK_EQ(ERR1(a), VFS_CLOSED);
    CHECK_EQ(fs(a, FS_SIZE, "t", f), R_NORMAL);
    CHECK_EQ(ival(R(a)[0]), 7);
    CHECK_EQ(fs(a, FS_READ, "tt", f, T_TRUE), R_BAD_CODE);    /* max not an int */

    /* Append adds at the end. */
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/a", VFS_APPEND), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_OK);
    term g = R(a)[0];
    CHECK_EQ(fs(a, FS_WRITE, "ts", g, "?"), R_NORMAL);
    CHECK_EQ(fs(a, FS_SIZE, "t", g), R_NORMAL);
    CHECK_EQ(ival(R(a)[0]), 8);
    done(a);                                                   /* closes both */
    CHECK_EQ(open_slots(), 0);
    CHECK_EQ(open_ports(), 0);
}

/* File.WriteAt: in bounds, never extends, Write mode only, owner only. */
TEST(fs_write_at) {
    setup();
    Actor *a = actor(0), *b = actor(0);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/w", VFS_WRITE), R_NORMAL);
    term f = R(a)[0];
    CHECK_EQ(fs(a, FS_WRITE, "ts", f, "hello"), R_NORMAL);
    CHECK_EQ(fs(a, FS_WRITEAT, "tis", f, 1, "EL"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_OK);
    CHECK_EQ(fs(a, FS_WRITEAT, "tiB", f, 3, "xy"), R_NORMAL);          /* Bytes */
    CHECK_EQ(ERR1(a), VFS_OK);
    CHECK_EQ(fs(a, FS_WRITEAT, "tis", f, 4, "xy"), R_NORMAL);          /* past the end */
    CHECK_EQ(ERR1(a), VFS_BADPATH);
    CHECK_EQ(fs(a, FS_WRITEAT, "tis", f, -1, "x"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_BADPATH);
    CHECK_EQ(fs(a, FS_WRITEAT, "tis", f, 0, ""), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_OK);
    CHECK_EQ(fs(a, FS_WRITEAT, "tti", f, T_TRUE, 1), R_BAD_CODE);      /* off not an int */
    CHECK_EQ(fs(a, FS_WRITEAT, "tii", f, 0, 5), R_BAD_CODE);           /* not data */
    CHECK_EQ(fs(b, FS_WRITEAT, "tis", f, 0, "x"), R_NORMAL);           /* not the owner */
    CHECK_EQ(ERR1(b), VFS_CLOSED);
    CHECK_EQ(fs(a, FS_WRITE, "ts", f, "!"), R_NORMAL);                 /* still appends */
    CHECK_EQ(fs(a, FS_SIZE, "t", f), R_NORMAL);
    CHECK_EQ(ival(R(a)[0]), 6);
    port_close(&vm, f);
    CHECK_EQ(fs(a, FS_WRITEAT, "tis", f, 0, "x"), R_NORMAL);           /* closed */
    CHECK_EQ(ERR1(a), VFS_CLOSED);

    CHECK_EQ(fs(a, FS_READFILE, "si", "/flash/w", 100), R_NORMAL);
    CHECK(bytes_is(a, R(a)[0], "hELxy!"));
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/w", VFS_READ), R_NORMAL);    /* Read mode */
    term r = R(a)[0];
    CHECK_EQ(fs(a, FS_WRITEAT, "tis", r, 0, "x"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_CLOSED);
    port_close(&vm, r);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/w", VFS_APPEND), R_NORMAL);  /* Append mode */
    term g = R(a)[0];
    CHECK_EQ(fs(a, FS_WRITEAT, "tis", g, 0, "x"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_CLOSED);
    done(a);
    done(b);
    CHECK_EQ(open_slots(), 0);
}

TEST(fs_open_errors) {
    setup();
    Actor *a = actor(0);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/none", VFS_READ), R_NORMAL);
    CHECK_EQ(R(a)[0], T_ZEROPID);
    CHECK_EQ(ERR2(a), VFS_NOTFOUND);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flashx/a", VFS_WRITE), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_NOTMOUNTED);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash//a", VFS_WRITE), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_BADPATH);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/a", 3), R_NORMAL);   /* bad mode */
    CHECK_EQ(ERR2(a), VFS_BADPATH);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/a", -1), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_BADPATH);
    char longp[120] = "/flash/";
    memset(longp + 7, 'a', 100);                               /* 107 bytes */
    CHECK_EQ(fs(a, FS_OPEN, "si", longp, VFS_WRITE), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_BADPATH);
    CHECK_EQ(fs(a, FS_OPEN, "ti", T_EMPTYSTR, VFS_WRITE), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_BADPATH);
    CHECK_EQ(fs(a, FS_OPEN, "ii", 5, VFS_WRITE), R_BAD_CODE);  /* path not a string */
    CHECK_EQ(fs(a, FS_OPEN, "ss", "/flash/a", "x"), R_BAD_CODE);
    CHECK_EQ(fs(a, FS_OPEN, "s", "/flash/a"), R_BAD_CODE);     /* argument count */
    CHECK_EQ(fs(a, FS_OPEN, "e", T_NIL), R_BAD_CODE);

    /* One writer per path; readers unrestricted. */
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/a", VFS_WRITE), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_OK);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/a", VFS_APPEND), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_BUSY);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/a", VFS_READ), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_OK);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/b", VFS_WRITE), R_NORMAL);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/c", VFS_WRITE), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_OK);
    CHECK_EQ(open_slots(), (int)GOGO_FS_MAX_OPEN);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/d", VFS_WRITE), R_NORMAL);
    CHECK_EQ(R(a)[0], T_ZEROPID);
    CHECK_EQ(ERR2(a), VFS_TOOMANY);
    done(a);
    CHECK_EQ(open_slots(), 0);

    /* The slot charge does not fit the actor: NoMemory, nothing opened and
     * the existing file not truncated. */
    make_file("/flash/keep", 10);
    Actor *s = actor(0);
    s->max_words = (uint16_t)(s->mem_words + actor_charged_words(s) + CHARGE - 1u);
    CHECK_EQ(fs(s, FS_OPEN, "si", "/flash/keep", VFS_WRITE), R_NORMAL);
    CHECK_EQ(R(s)[0], T_ZEROPID);
    CHECK_EQ(ERR2(s), VFS_NOMEMORY);
    CHECK_EQ(open_slots(), 0);
    CHECK_EQ(open_ports(), 0);
    int32_t sz; bool d;
    CHECK_EQ(vfs_stat("/flash/keep", 11, &sz, &d), VFS_OK);
    CHECK_EQ(sz, 10);
    done(s);
}

/* Review Focus 5: stale handles never crash, they return Closed. */
TEST(fs_stale_and_foreign_handles) {
    setup();
    Actor *a = actor(0), *b = actor(0);
    make_file("/flash/a", 5);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/a", VFS_READ), R_NORMAL);
    term f = R(a)[0];
    /* Another actor holding the PID: Closed for every per-file op. */
    CHECK_EQ(fs(b, FS_READ, "ti", f, 3), R_NORMAL);
    CHECK_EQ(R(b)[0], T_EMPTYBYTES);
    CHECK_EQ(ERR2(b), VFS_CLOSED);
    CHECK_EQ(fs(b, FS_WRITE, "ts", f, "x"), R_NORMAL);
    CHECK_EQ(ERR1(b), VFS_CLOSED);
    CHECK_EQ(fs(b, FS_SYNC, "t", f), R_NORMAL);
    CHECK_EQ(ERR1(b), VFS_CLOSED);
    CHECK_EQ(fs(b, FS_SIZE, "t", f), R_NORMAL);
    CHECK_EQ(ival(R(b)[0]), 0);
    CHECK_EQ(ERR2(b), VFS_CLOSED);
    /* ... but anyone holding it may close it. */
    port_close(&vm, f);
    CHECK_EQ(open_slots(), 0);
    CHECK_EQ(fs(a, FS_READ, "ti", f, 3), R_NORMAL);           /* after Close */
    CHECK_EQ(ERR2(a), VFS_CLOSED);
    CHECK_EQ(fs(a, FS_SYNC, "t", f), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_CLOSED);
    CHECK_EQ(fs(a, FS_READ, "ti", a->id, 3), R_NORMAL);       /* an actor PID */
    CHECK_EQ(ERR2(a), VFS_CLOSED);
    CHECK_EQ(fs(a, FS_READ, "ti", T_ZEROPID, 3), R_NORMAL);   /* the zero PID */
    CHECK_EQ(ERR2(a), VFS_CLOSED);
    CHECK_EQ(fs(a, FS_WRITE, "ts", term_from_int(3), "x"), R_NORMAL);   /* not a PID */
    CHECK_EQ(ERR1(a), VFS_CLOSED);
    /* A reopened file reuses the port slot under a new generation: the old
     * PID stays stale. */
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/a", VFS_READ), R_NORMAL);
    term g = R(a)[0];
    CHECK(g != f);
    CHECK_EQ(fs(a, FS_READ, "ti", f, 3), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_CLOSED);
    CHECK_EQ(fs(a, FS_READ, "ti", g, 3), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_OK);
    /* `file` is not a platform driver: port.Open("file", ...) cannot make one. */
    for (const PortDriver *const *l = platform_port_drivers(); l && *l; l++)
        CHECK(strcmp((*l)->name, "file") != 0);
    done(a);
    done(b);
}

/* Review Focus 5: Handoff moves the charge; the old owner gets Closed. */
TEST(fs_handoff) {
    setup();
    Actor *a = actor(0), *b = actor(0);
    uint32_t ba = actor_charged_words(a), bb = actor_charged_words(b);
    make_file("/flash/a", 6);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/a", VFS_READ), R_NORMAL);
    term f = R(a)[0];
    CHECK_EQ(fs(a, FS_READ, "ti", f, 2), R_NORMAL);
    CHECK(bytes_is(a, R(a)[0], "01"));
    CHECK(port_handoff(&vm, a, f, b->id));
    CHECK_EQ(actor_charged_words(a), ba);
    CHECK_EQ(actor_charged_words(b), bb + CHARGE);
    CHECK_EQ(fs(a, FS_READ, "ti", f, 2), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_CLOSED);
    CHECK_EQ(fs(b, FS_READ, "ti", f, 2), R_NORMAL);           /* the position moved too */
    CHECK_EQ(ERR2(b), VFS_OK);
    CHECK(bytes_is(b, R(b)[0], "23"));
    done(a);                                                   /* no longer closes it */
    CHECK_EQ(open_slots(), 1);
    done(b);
    CHECK_EQ(open_slots(), 0);
    CHECK_EQ(open_ports(), 0);
}

/* Review Focus 1: the owner dies mid-write: the file is closed (committed),
 * the slot and charge go, and another actor can open it for Write at once. */
TEST(fs_owner_killed_mid_write) {
    setup();
    Actor *a = actor(0), *b = actor(0);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/log", VFS_WRITE), R_NORMAL);
    term f = R(a)[0];
    CHECK_EQ(fs(a, FS_WRITE, "ts", f, "part"), R_NORMAL);
    CHECK_EQ(fs(b, FS_OPEN, "si", "/flash/log", VFS_WRITE), R_NORMAL);
    CHECK_EQ(ERR2(b), VFS_BUSY);
    CHECK_EQ(fs(b, FS_REMOVE, "s", "/flash/log"), R_NORMAL);
    CHECK_EQ(ERR1(b), VFS_BUSY);
    term apid = a->id;
    sig_kill(&vm, b, apid, R_KILLED);                          /* actor.Kill(a, Killed) */
    uint32_t code; term fn;
    CHECK(actor_exit_pending(a, &code, &fn));
    platform_unix_err_hook = capture_err;                      /* the crash report */
    actor_exit(&vm, a, code, fn);
    platform_unix_err_hook = NULL;
    CHECK(actor_lookup(&vm, apid) == NULL);
    CHECK_EQ(open_slots(), 0);
    CHECK_EQ(open_ports(), 0);
    int32_t sz; bool d;
    CHECK_EQ(vfs_stat("/flash/log", 10, &sz, &d), VFS_OK);
    CHECK_EQ(sz, 4);                                           /* committed */
    uint32_t bb = actor_charged_words(b);
    CHECK_EQ(fs(b, FS_OPEN, "si", "/flash/log", VFS_APPEND), R_NORMAL);
    CHECK_EQ(ERR2(b), VFS_OK);
    CHECK_EQ(actor_charged_words(b), bb + CHARGE);
    CHECK_EQ(fs(b, FS_WRITE, "ts", R(b)[0], "+more"), R_NORMAL);
    CHECK_EQ(ERR1(b), VFS_OK);
    done(b);
    CHECK_EQ(vfs_stat("/flash/log", 10, &sz, &d), VFS_OK);
    CHECK_EQ(sz, 9);
}

/* Read: max clamps to 1..4096; a result over the caller's limit is
 * NoMemory with the actor alive and the position unchanged; results up to
 * GOGO_BYTES_INLINE bytes are heap Bytes, larger ones off-heap. */
TEST(fs_read_limits) {
    setup();
    memfs_set_budget(0, -1);
    make_file("/flash/big", 4200);
    Actor *a = actor(0);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/big", VFS_READ), R_NORMAL);
    term f = R(a)[0];
    CHECK_EQ(fs(a, FS_READ, "ti", f, GOGO_BYTES_INLINE), R_NORMAL);
    CHECK(term_is_ref(R(a)[0]));                               /* heap K_BYTES */
    CHECK_EQ(bytes_len(a, R(a)[0]), GOGO_BYTES_INLINE);
    CHECK_EQ(fs(a, FS_READ, "tI", f, 1 << 30), R_NORMAL);      /* Read(1<<30): a boxed int */
    CHECK_EQ(ERR2(a), VFS_OK);
    CHECK(term_is_handle(R(a)[0]));                            /* off-heap */
    CHECK_EQ(bytes_len(a, R(a)[0]), VFS_READ_MAX);
    CHECK_EQ(fs(a, FS_READ, "ti", f, TERM_INT_MAX), R_NORMAL);   /* the rest */
    CHECK_EQ(bytes_len(a, R(a)[0]), 4200u - GOGO_BYTES_INLINE - VFS_READ_MAX);
    done(a);

    /* A small actor: 1000 bytes off-heap do not fit. */
    Actor *s = actor(0);
    s->max_words = (uint16_t)(s->mem_words + actor_charged_words(s) + CHARGE + 40u);
    CHECK_EQ(fs(s, FS_OPEN, "si", "/flash/big", VFS_READ), R_NORMAL);
    CHECK_EQ(ERR2(s), VFS_OK);
    f = R(s)[0];
    CHECK_EQ(fs(s, FS_READ, "ti", f, 1000), R_NORMAL);
    CHECK_EQ(R(s)[0], T_EMPTYBYTES);
    CHECK_EQ(ERR2(s), VFS_NOMEMORY);
    uint32_t c; term fn;
    CHECK(!actor_exit_pending(s, &c, &fn));                    /* alive */
    CHECK_EQ(fs(s, FS_READ, "ti", f, 4), R_NORMAL);           /* position unchanged */
    CHECK_EQ(ERR2(s), VFS_OK);
    CHECK(bytes_is(s, R(s)[0], "0123"));
    done(s);
}

TEST(fs_readfile_writefile) {
    setup();
    Actor *a = actor(0);
    CHECK_EQ(fs(a, FS_WRITEFILE, "ss", "/flash/f", "data"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_OK);
    CHECK_EQ(fs(a, FS_READFILE, "si", "/flash/f", 100), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_OK);
    CHECK(bytes_is(a, R(a)[0], "data"));
    CHECK_EQ(fs(a, FS_READFILE, "si", "/flash/f", 4), R_NORMAL);
    CHECK(bytes_is(a, R(a)[0], "data"));
    CHECK_EQ(fs(a, FS_READFILE, "si", "/flash/f", 3), R_NORMAL);   /* size > max */
    CHECK_EQ(R(a)[0], T_EMPTYBYTES);
    CHECK_EQ(ERR2(a), VFS_NOMEMORY);
    CHECK_EQ(open_slots(), 0);
    CHECK_EQ(fs(a, FS_WRITEFILE, "sB", "/flash/f", "new!"), R_NORMAL);   /* replace, Bytes */
    CHECK_EQ(ERR1(a), VFS_OK);
    CHECK_EQ(fs(a, FS_READFILE, "si", "/flash/f", 100), R_NORMAL);
    CHECK(bytes_is(a, R(a)[0], "new!"));
    CHECK_EQ(fs(a, FS_WRITEFILE, "sb", "/flash/e", ""), R_NORMAL);       /* empty []byte */
    CHECK_EQ(ERR1(a), VFS_OK);
    CHECK_EQ(fs(a, FS_READFILE, "si", "/flash/e", 0), R_NORMAL);
    CHECK_EQ(R(a)[0], T_EMPTYBYTES);
    CHECK_EQ(ERR2(a), VFS_OK);
    CHECK_EQ(fs(a, FS_READFILE, "si", "/flash/none", 10), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_NOTFOUND);
    CHECK_EQ(fs(a, FS_MKDIR, "s", "/flash/d"), R_NORMAL);
    CHECK_EQ(fs(a, FS_READFILE, "si", "/flash/d", 10), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_ISDIR);
    CHECK_EQ(fs(a, FS_WRITEFILE, "ss", "/flash/d", "x"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_ISDIR);
    CHECK_EQ(fs(a, FS_WRITEFILE, "ss", "/flash/x/y", "x"), R_NORMAL);   /* no parent */
    CHECK_EQ(ERR1(a), VFS_NOTFOUND);
    CHECK_EQ(fs(a, FS_WRITEFILE, "ss", "/zz/y", "x"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_NOTMOUNTED);
    CHECK_EQ(fs(a, FS_WRITEFILE, "si", "/flash/f", 1), R_BAD_CODE);
    /* An open file: the replace is Busy, the old contents stay, no .tmp left. */
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/f", VFS_READ), R_NORMAL);
    term f = R(a)[0];
    CHECK_EQ(fs(a, FS_WRITEFILE, "ss", "/flash/f", "zzz"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_BUSY);
    port_close(&vm, f);
    CHECK_EQ(fs(a, FS_READFILE, "si", "/flash/f", 100), R_NORMAL);
    CHECK(bytes_is(a, R(a)[0], "new!"));
    int32_t sz; bool d;
    CHECK_EQ(vfs_stat("/flash/f.tmp", 12, &sz, &d), VFS_NOTFOUND);
    /* FAT-like /sd: remove + rename. */
    CHECK_EQ(fs(a, FS_WRITEFILE, "ss", "/sd/f", "one"), R_NORMAL);
    CHECK_EQ(fs(a, FS_WRITEFILE, "ss", "/sd/f", "two"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_OK);
    CHECK_EQ(fs(a, FS_READFILE, "si", "/sd/F", 100), R_NORMAL);
    CHECK(bytes_is(a, R(a)[0], "two"));
    /* A 32-byte name: the temporary name is cut to fit. */
    const char *lp = "/flash/abcdefghijklmnopqrstuvwxyz012345";
    CHECK_EQ(fs(a, FS_WRITEFILE, "ss", lp, "L"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_OK);
    CHECK_EQ(fs(a, FS_READFILE, "si", lp, 10), R_NORMAL);
    CHECK(bytes_is(a, R(a)[0], "L"));
    /* No space: the partial temporary file is removed. */
    memfs_set_budget(0, 30);
    CHECK_EQ(fs(a, FS_WRITEFILE, "ss", "/flash/g", "0123456789012345678901234567890123456789"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_NOSPACE);
    CHECK_EQ(vfs_stat("/flash/g.tmp", 12, &sz, &d), VFS_NOTFOUND);
    CHECK_EQ(vfs_stat("/flash/g", 8, &sz, &d), VFS_NOTFOUND);
    CHECK_EQ(open_slots(), 0);
    done(a);
}

/* FAT-like /sd: the final rename of WriteFile fails after the old file was
 * removed. The temporary is then the only copy and must stay (spec §5.6);
 * a failure while the target still exists still removes it. */
TEST(fs_writefile_keeps_tmp_when_target_gone) {
    setup();
    Actor *a = actor(0);
    int32_t sz; bool d;
    CHECK_EQ(fs(a, FS_WRITEFILE, "ss", "/sd/cfg", "OLD"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_OK);
    memfs_fail_next_rename(1, VFS_IO);
    CHECK_EQ(fs(a, FS_WRITEFILE, "ss", "/sd/cfg", "NEW"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_IO);
    CHECK_EQ(vfs_stat("/sd/cfg", 7, &sz, &d), VFS_NOTFOUND);
    CHECK_EQ(fs(a, FS_READFILE, "si", "/sd/cfg.tmp", 100), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_OK);
    CHECK(bytes_is(a, R(a)[0], "NEW"));
    CHECK_EQ(open_slots(), 0);
    /* the next WriteFile replaces the stale temporary and succeeds */
    CHECK_EQ(fs(a, FS_WRITEFILE, "ss", "/sd/cfg", "NEWER"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_OK);
    CHECK_EQ(vfs_stat("/sd/cfg.tmp", 11, &sz, &d), VFS_NOTFOUND);
    /* the target is still there (Busy): the temporary goes */
    CHECK_EQ(fs(a, FS_OPEN, "si", "/sd/cfg", VFS_READ), R_NORMAL);
    term f = R(a)[0];
    CHECK_EQ(fs(a, FS_WRITEFILE, "ss", "/sd/cfg", "zzz"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_BUSY);
    port_close(&vm, f);
    CHECK_EQ(vfs_stat("/sd/cfg.tmp", 11, &sz, &d), VFS_NOTFOUND);
    CHECK_EQ(fs(a, FS_READFILE, "si", "/sd/cfg", 100), R_NORMAL);
    CHECK(bytes_is(a, R(a)[0], "NEWER"));
    done(a);
}

TEST(fs_stat_readdir) {
    setup();
    Actor *a = actor(0);
    make_file("/flash/a", 5);
    CHECK_EQ(fs(a, FS_MKDIR, "s", "/flash/d"), R_NORMAL);
    CHECK_EQ(fs(a, FS_STAT, "s", "/flash/a"), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_OK);
    const uint32_t *o = obj_of(&vm, a, R(a)[0]);
    CHECK(o && o[0] == hdr_make(4, K_STRUCT) && o[1] == TYPE_FS_INFO);
    CHECK_EQ(o[2], term_from_int(5));
    CHECK_EQ(o[3], T_FALSE);
    CHECK_EQ(fs(a, FS_STAT, "s", "/flash"), R_NORMAL);         /* the mount root */
    o = obj_of(&vm, a, R(a)[0]);
    CHECK(o && o[1] == TYPE_FS_INFO && o[3] == T_TRUE);
    CHECK_EQ(fs(a, FS_STAT, "s", "/flash/none"), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_NOTFOUND);
    o = obj_of(&vm, a, R(a)[0]);                               /* the zero Info */
    CHECK(o && o[1] == TYPE_FS_INFO && o[2] == term_from_int(0) && o[3] == T_FALSE);
    CHECK_EQ(fs(a, FS_STAT, "s", "/x/a"), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_NOTMOUNTED);

    CHECK_EQ(fs(a, FS_READDIR, "s", "/flash"), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_OK);
    o = obj_of(&vm, a, R(a)[0]);
    CHECK(o && o[0] == hdr_make(5, K_SLICE));
    CHECK_EQ(o[1], 0u);
    CHECK_EQ(o[2], 2u);
    CHECK_EQ(o[3], 2u);
    const uint32_t *arr = obj_of(&vm, a, o[4]);
    CHECK(arr && arr[0] == hdr_make(4, K_ARRAY) && arr[1] == 2u);
    const uint8_t *p; uint32_t n;
    CHECK(name_bytes(&vm, a, arr[2], &p, &n) && n == 1 && p[0] == 'a');
    CHECK(name_bytes(&vm, a, arr[3], &p, &n) && n == 2 && !memcmp(p, "d/", 2));
    CHECK_EQ(fs(a, FS_READDIR, "s", "/flash/d"), R_NORMAL);   /* empty */
    CHECK_EQ(ERR2(a), VFS_OK);
    CHECK_EQ(R(a)[0], T_NILSLICE);
    CHECK_EQ(fs(a, FS_READDIR, "s", "/flash/a"), R_NORMAL);
    CHECK_EQ(R(a)[0], T_NILSLICE);
    CHECK_EQ(ERR2(a), VFS_NOTDIR);
    CHECK_EQ(fs(a, FS_READDIR, "s", "/flash/none"), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_NOTFOUND);
    /* Many entries survive the GC the result's allocation may run. */
    for (int i = 0; i < 12; i++) {
        char p2[32];
        snprintf(p2, sizeof p2, "/flash/d/file_number_%02d", i);
        make_file(p2, 1);
    }
    for (int k = 0; k < 20; k++) {
        CHECK_EQ(fs(a, FS_READDIR, "s", "/flash/d"), R_NORMAL);
        o = obj_of(&vm, a, R(a)[0]);
        CHECK(o && o[2] == 12u);
        arr = obj_of(&vm, a, o[4]);
        CHECK(name_bytes(&vm, a, arr[2 + 11], &p, &n) && n == 14 && !memcmp(p, "file_number_11", 14));
    }
    done(a);
}

TEST(fs_path_ops) {
    setup();
    Actor *a = actor(0);
    CHECK_EQ(fs(a, FS_MKDIR, "s", "/flash/d"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_OK);
    CHECK_EQ(fs(a, FS_MKDIR, "s", "/flash/d"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_EXISTS);
    CHECK_EQ(fs(a, FS_MKDIR, "s", "/flash"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_BADPATH);
    CHECK_EQ(fs(a, FS_MKDIR, "s", "/flash/x/y"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_NOTFOUND);
    make_file("/flash/d/f", 3);
    CHECK_EQ(fs(a, FS_REMOVE, "s", "/flash/d"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_NOTEMPTY);
    CHECK_EQ(fs(a, FS_REMOVE, "s", "/flash/none"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_NOTFOUND);
    CHECK_EQ(fs(a, FS_RENAME, "ss", "/flash/d/f", "/flash/g"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_OK);
    make_file("/flash/h", 1);
    CHECK_EQ(fs(a, FS_RENAME, "ss", "/flash/g", "/flash/h"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_EXISTS);
    CHECK_EQ(fs(a, FS_RENAME, "ss", "/flash/g", "/sd/g"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_BADPATH);
    CHECK_EQ(fs(a, FS_RENAME, "si", "/flash/g", 1), R_BAD_CODE);
    CHECK_EQ(fs(a, FS_OPEN, "si", "/flash/g", VFS_READ), R_NORMAL);
    term g = R(a)[0];
    CHECK_EQ(fs(a, FS_RENAME, "ss", "/flash/g", "/flash/k"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_BUSY);
    CHECK_EQ(fs(a, FS_REMOVE, "s", "/flash/g"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_BUSY);
    port_close(&vm, g);
    CHECK_EQ(fs(a, FS_REMOVE, "s", "/flash/g"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_OK);
    CHECK_EQ(fs(a, FS_REMOVE, "s", "/flash/d"), R_NORMAL);
    CHECK_EQ(ERR1(a), VFS_OK);

    CHECK_EQ(fs(a, FS_USAGE, "s", "/flash"), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_OK);
    const uint32_t *o = obj_of(&vm, a, R(a)[0]);
    CHECK(o && o[0] == hdr_make(4, K_STRUCT) && o[1] == TYPE_FS_SPACE);
    CHECK_EQ(o[2], term_from_int(1));                          /* /flash/h */
    CHECK_EQ(o[3], term_from_int(4000));
    CHECK_EQ(fs(a, FS_USAGE, "s", "/nope"), R_NORMAL);
    CHECK_EQ(ERR2(a), VFS_NOTMOUNTED);
    o = obj_of(&vm, a, R(a)[0]);
    CHECK(o && o[1] == TYPE_FS_SPACE && o[2] == term_from_int(0) && o[3] == term_from_int(0));
    /* Totals beyond the small-int range come back boxed. */
    memfs_set_budget(0, 0x7FFFFFF0);
    CHECK_EQ(fs(a, FS_USAGE, "s", "/flash"), R_NORMAL);
    o = obj_of(&vm, a, R(a)[0]);
    int32_t v;
    CHECK(o && int_val(&vm, a, o[3], &v) && v == 0x7FFFFFF0);
    done(a);
}

/* The hook's own checks: op and argument count, the second result
 * register for two-result ops only. */
TEST(fs_frame_bounds) {
    setup();
    Actor *a = actor(0);
    CHECK_EQ(fs_at(a, NREGS - 1u, FS_MKDIR, "s", "/flash/d"), R_NORMAL);   /* ra+1 outside: fine */
    CHECK_EQ(ival(R(a)[NREGS - 1u]), VFS_OK);
    CHECK_EQ(fs_at(a, NREGS - 1u, FS_STAT, "s", "/flash/d"), R_BAD_CODE);  /* needs ra+1 */
    CHECK_EQ(fs_at(a, NREGS - 1u, FS_OPEN, "si", "/flash/q", VFS_WRITE), R_BAD_CODE);
    CHECK_EQ(open_slots(), 0);                                 /* checked before acting */
    CHECK_EQ(fs_at(a, NREGS - 2u, FS_STAT, "s", "/flash/d"), R_NORMAL);
    CHECK_EQ(fs(a, 13, "s", "/flash/d"), R_BAD_CODE);
    CHECK_EQ(fs(a, -1, "s", "/flash/d"), R_BAD_CODE);
    CHECK_EQ(fs(a, FS_MKDIR, "ss", "/flash/d", "/flash/e"), R_BAD_CODE);
    CHECK_EQ(fs(a, FS_MKDIR, ""), R_BAD_CODE);
    R(a)[2] = T_TRUE;                                          /* op not an int */
    CHECK_EQ(vm_fs_builtin(&vm, a, 0, 2, 1, NREGS), R_BAD_CODE);
    CHECK_EQ(vm_fs_builtin(&vm, a, 0, 2, 0, NREGS), R_BAD_CODE);
    done(a);
}

/* Through the interpreter: main (3 regs) runs
 *   LOADK r0, op; LOADK r1, "/flash/d"; BUILTIN r2, r0, 2, B_FS; RECEIVE
 * so r2 is the last register and r2+1 is outside the frame. */
#define RUN_WORDS 29
static const char *run_fs(int op, Actor **out) {
    static uint32_t c[RUN_WORDS]; static Image im;
    memset(c, 0, sizeof c);
    c[0] = IMAGE_MAGIC; c[1] = IMAGE_VERSION; c[2] = 1; c[3] = 0;
    c[4] = 7; c[5] = 8; c[6] = 0; c[7] = 0;
    c[8] = 0; c[9] = 0 | (3u << 16); c[10] = 0;
    c[11] = hdr_make(3, K_STRING); c[12] = 4; memcpy(&c[13], "main", 4);
    c[14] = hdr_make(4, K_STRING); c[15] = 8; memcpy(&c[16], "/flash/d", 8);
    c[18] = OP_MAKE(OP_LOADK, 0, 0, 0); c[19] = term_from_int(op);
    c[20] = OP_MAKE(OP_LOADK, 1, 0, 0); c[21] = term_from_const(3);
    c[22] = OP_MAKE(OP_BUILTIN, 2, 0, 2); c[23] = B_FS;
    c[24] = OP_MAKE(OP_BUILTIN, 0, 0, 0); c[25] = B_RECEIVE;
    c[26] = 1u | 8u;                         /* v7 const map: strings at 0, 3 */
    CHECK_EQ(image_load(&im, c, RUN_WORDS), IMG_OK);
    CHECK_EQ(vm_init(&vm, &im, 4), 0);
    Actor *a = actor_new(&vm, 32);
    CHECK(a != NULL);
    CHECK(heap_push_frame(&vm, a, 3, -1));
    a->pc = im.code;
    err_len = 0; err_buf[0] = 0;
    platform_unix_err_hook = capture_err;
    int rc = interp_run(&vm, a);
    platform_unix_err_hook = NULL;
    *out = rc == RUN_EXIT ? NULL : a;
    return err_buf;
}

TEST(fs_interp) {
    setup();
    Actor *a;
    CHECK_EQ(run_fs(FS_MKDIR, &a)[0], 0);                      /* single result in the last reg */
    CHECK(a != NULL);
    CHECK_EQ(a->mem[a->fp + 2], term_from_int(VFS_OK));
    actor_free(&vm, a);
    int32_t sz; bool d;
    CHECK_EQ(vfs_stat("/flash/d", 8, &sz, &d), VFS_OK);
    CHECK(d);
    CHECK(strstr(run_fs(FS_STAT, &a), "crashed: badcode") != NULL);   /* r3 missing */
    CHECK(a == NULL);
    uint32_t (*hook)(struct Vm *, struct Actor *, uint32_t, uint32_t, uint32_t, uint32_t) = vm_fs_builtin;
    vm_fs_builtin = NULL;                                      /* no storage linked */
    CHECK(strstr(run_fs(FS_MKDIR, &a), "crashed: badcode") != NULL);
    CHECK(a == NULL);
    vm_fs_builtin = hook;
}

TEST(fs_image_v5_rejected) {
    setup();
    CHECK_EQ(IMAGE_VERSION, 11u);
    uint32_t v[IMG_WORDS];
    memcpy(v, w, sizeof v);
    v[1] = 5;
    Image im;
    CHECK_EQ(image_load(&im, v, IMG_WORDS), IMG_ERR_VERSION);
}
