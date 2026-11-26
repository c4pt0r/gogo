/* Bytecode interpreter and scheduler.
 *
 * Interpreter state lives in the actor (mem, fp, sp) plus three locals:
 * `pc` (code offset of the current instruction), `fn` (the function whose
 * frame is on top) and `nregs` (its register count). Registers of the top
 * frame are a->mem[a->fp + i]; R is a cached pointer to them.
 *
 * GC safety: heap_alloc and heap_push_frame may collect, which moves objects
 * and can move the whole block (changing a->mem and a->fp). Every term that
 * must survive is either on the stack (registers) or passed as a root, and R
 * is re-derived with RELOAD() after each such call. Ints taken out of terms
 * are plain C values and need no protection.
 *
 * Safety on malformed images: image_load proves the tables and the const
 * area well-formed, and that every const ref inside a const object (and
 * every func/type name) names an object start in the const map. Here
 * register indices are checked against the current function's n_regs (by
 * the decoder, from the op's row of op_info, before its case runs), the
 * pc against the code area, LOADK operands against the term kinds an image
 * may contain and, for const refs, against the const map (LOADK operands
 * are checked when executed, not at load: a jump may land on an ext word),
 * and RET/RETN check that the frame chain is consistent. Violations
 * crash the actor with `badcode`; func indexes, type ids and jump targets
 * are checked only when used. Not every malformed image is merely a
 * badcode: a const ref into the middle of an object would forge one that
 * the GC reads and writes out of bounds, which is why the const map is
 * checked at load and in LOADK (v7). docs/known-issues.md ("Hot loading")
 * notes what stays unchecked.
 *
 * Every way out of an actor (return from the entry frame, EXIT, a runtime
 * error, a pending exit mark) goes through actor_exit (signals.c), which
 * reports and frees it; interp_run then returns RUN_EXIT.
 *
 * Timed waits (ReceiveTimeout, Sleep) block like RECEIVE: the pc stays on
 * the BUILTIN, which runs again when the actor is woken. The first run arms
 * a wakeup timer (AF_WAKEUP_ARMED); when it fires it sets AF_TIMED_OUT and
 * the re-run returns. A message wakes a ReceiveTimeout as for RECEIVE, and
 * taking it cancels the wakeup. Sleep ignores messages: woken by one, it
 * finds its wakeup still armed and blocks again.
 *
 * Call blocks the same way but must send its request only once. The first
 * run sends it, leaves the Ref in the result register ra (a GC-visible
 * stack word, so the serial survives yields and collections), arms the
 * wakeup and sets AF_CALL_PENDING. Re-runs see the flag, do not read the
 * arguments (ra may overlap them), look for the reply with that serial
 * (msg_take_reply; other messages stay queued) and block again unless it
 * came or the wakeup fired. The flag is cleared on reply and on timeout;
 * an actor that dies takes it along. */
#include "interp.h"
#include "heap.h"
#include "msg.h"
#include "signals.h"
#include "timer.h"
#include "names.h"
#include "out.h"
#include "opcodes.h"
#include "host.h"
#include "evq.h"
#include "port.h"
#include "strs.h"
#include "bytes.h"
#include "module.h"
#include "floats.h"
#include <string.h>

#define REDUCTIONS     1000u
#define PRINT_MAX_DEPTH 16

/* ---- image helpers ---- */

/* The function containing code offset pc: the last one whose code_off is
 * not above pc. image_load guarantees code_offs ascend with the function
 * index, so this is a binary search. pc below function 0 maps to 0. */
static uint32_t func_of_pc(const Image *img, uint32_t pc) {
    uint32_t lo = 0, hi = img->n_funcs;   /* answer in [lo, hi) */
    while (hi - lo > 1u) {
        uint32_t mid = lo + (hi - lo) / 2u;
        if (image_func_code_off(img, mid) <= pc) lo = mid;
        else hi = mid;
    }
    return lo;
}

/* obj_of, name_bytes and int_val are in heap.h (shared with port.c). */

/* ---- module tags (term.h) ---- */

/* A struct of type word ty may be used: a reserved type (global), one of
 * the running module's own (tag), or of another module still loaded. */
static inline bool type_live(const Vm *vm, uint32_t tag, uint32_t ty) {
    return ty < TYPE_OWN_BASE || (ty & MOD_TAG_MASK) == tag || module_of_tag(vm, ty) != NULL;
}

/* False if t names something of a module that is gone: a const ref that
 * no longer resolves, or a heap closure or module struct whose tag names a
 * gone module. The one check every operation that answers about a value
 * without using it as a specific kind (EQ/NE, TYPEIS, println) applies, so
 * a stale value is `badcode` there too rather than "not equal" / "not
 * this type" / "?". Operations that need a kind fail through obj_of (NULL),
 * type_live or the CALLV/Spawn tag check. */
static bool term_live(const Vm *vm, const Actor *a, term t) {
    if (term_is_const(t)) return const_obj_of(vm, t) != NULL;
    if (!term_is_ref(t)) return true;
    const uint32_t *o = obj_of(vm, a, t);
    if (!o) return true;                 /* not an object: the kind checks decide */
    uint32_t k = hdr_kind(o[0]);
    if (k == K_CLOSURE) return module_of_tag(vm, o[1]) != NULL;
    if (k == K_STRUCT) return o[1] < TYPE_OWN_BASE || module_of_tag(vm, o[1]) != NULL;
    return true;
}

/* deep_live's limit on the terms it visits. It walks without marking what
 * it has seen (a mark set would cost RAM), so a value that reaches the same
 * subvalue by many paths is walked once per path, and the number of paths
 * can grow as width^depth for a value of a few hundred words. A heap value
 * without such sharing cannot reach the limit: every visit but the first
 * reads a distinct word of the actor's heap (at most HEAP_MAX_WORDS), or
 * of a constant area (the image's constant structs, few and small). It is
 * the println walk (out_term) printing nothing, so println never prints
 * what was not checked. */
#define WALK_BUDGET 65536

/* deep_live's verdicts besides "live" (the budget left, >= 0). */
#define WALK_CUT   (-1)
#define WALK_STALE (-2)

/* term_live for everything println would print: t and, as out_term walks
 * them, struct fields and slice elements (to the same depth), each visit
 * spending one of left (WALK_BUDGET at depth 0). Returns what is left,
 * WALK_CUT once it is spent or WALK_STALE. The gate for what a port reads
 * in place (port.Open's name and cfg, a port command) is ">= 0": a stale
 * value there is badcode for the actor too, for every driver, rather than
 * a silently refused open or ignored command, and so is a value too
 * entangled to check within the budget. println's is "not WALK_STALE": it
 * cuts what it prints at the same point. */
static int32_t out_term(Out *o, const Vm *vm, const Actor *a, term t, int depth, int32_t left);
static int32_t deep_live(const Vm *vm, const Actor *a, term t, int depth, int32_t left) {
    return out_term(NULL, vm, a, t, depth, left);   /* the walk, printing nothing */
}

/* ---- ints ---- */

/* Term for v, boxing it into a K_INT on the heap when it does not fit in 31
 * bits. May GC (the caller reloads register pointers). False on OOM. */
static bool make_int(Vm *vm, Actor *a, int32_t v, term *out) {
    if (term_int_fits(v)) {
        *out = term_from_int(v);
        return true;
    }
    return heap_box(vm, a, K_INT, 2, (uint32_t)v, out);
}

/* int64 values have two representations: a small-int immediate when the
 * value fits in 31 bits, or a K_INT64 box (heap or const area). The VM
 * cannot tell an int64 immediate from an int, so the compiler picks the
 * typed *64 ops from static types and boxes (BOX64) every int64 that goes
 * into an interface: inside `any` an int64 is always a box, which is how
 * TYPEIS tells it from an int. */

/* The K_INT64 object t names (heap or const area), or NULL. */
static const uint32_t *int64_box(const Vm *vm, const Actor *a, term t) {
    const uint32_t *o = obj_of(vm, a, t);
    return (o && hdr_kind(o[0]) == K_INT64 && hdr_words(o[0]) == 3u) ? o : NULL;
}

/* Value of an int64 in either representation; false for anything else
 * (a K_INT box is an int, never an int64). */
static bool int64_val(const Vm *vm, const Actor *a, term t, int64_t *out) {
    if (term_is_int(t)) {
        *out = term_to_int(t);
        return true;
    }
    const uint32_t *o = int64_box(vm, a, t);
    if (!o) return false;
    *out = (int64_t)((uint64_t)o[1] | ((uint64_t)o[2] << 32));
    return true;
}

static bool int64_fits(int64_t v) { return v >= TERM_INT_MIN && v <= TERM_INT_MAX; }

/* A new heap K_INT64 holding v. May GC (the caller reloads register
 * pointers). False on OOM. */
static bool box_int64(Vm *vm, Actor *a, int64_t v, term *out) {
    return heap_box(vm, a, K_INT64, 3, (uint64_t)v, out);
}

/* Term for the int64 v: an immediate when it fits in 31 bits (no
 * allocation), else a new box. May GC; false on OOM. */
static bool make_int64(Vm *vm, Actor *a, int64_t v, term *out) {
    if (int64_fits(v)) {
        *out = term_from_int((int32_t)v);
        return true;
    }
    return box_int64(vm, a, v, out);
}

/* Serial of a Ref: a K_REF object, or 0 for the zero Ref (T_ZEROREF);
 * false for anything else. */
static bool ref_serial(const Vm *vm, const Actor *a, term t, uint64_t *out) {
    if (t == T_ZEROREF) { *out = 0; return true; }
    const uint32_t *o = obj_of(vm, a, t);
    if (!o || hdr_kind(o[0]) != K_REF || hdr_words(o[0]) != 3u) return false;
    *out = (uint64_t)o[1] | ((uint64_t)o[2] << 32);
    return true;
}

/* Wrapping int32 arithmetic without signed-overflow UB. The conversion of
 * an out-of-range uint32 back to int32 is implementation-defined in C99;
 * every compiler we target defines it as two's complement. */
static int32_t wrap(uint32_t u) { return (int32_t)u; }
static int64_t wrap64(uint64_t u) { return (int64_t)u; }

/* A shift count (any integer, either representation) clamped to [-1, 64]:
 * every count >= the width acts alike, and -1 stands for any negative one. */
static bool count_val(const Vm *vm, const Actor *a, term t, int32_t *out) {
    int32_t v;
    int64_t n;
    if (int_val(vm, a, t, &v)) n = v;
    else if (!int64_val(vm, a, t, &n)) return false;
    *out = n < 0 ? -1 : n > 64 ? 64 : (int32_t)n;
    return true;
}

/* An integer operand: of an int64 op (wide) an int64 in either
 * representation, of an int op an int (a small int or a K_INT box; an
 * int64 box is badcode), sign-extended. False for anything else. */
static bool int_operand(const Vm *vm, const Actor *a, term t, bool wide, int64_t *out) {
    int32_t v;
    if (wide) return int64_val(vm, a, t, out);
    if (!int_val(vm, a, t, &v)) return false;
    *out = v;
    return true;
}

/* x op y for the int arithmetic ops with Go semantics (wrapping; MinInt
 * / -1 == MinInt; x % -1 == 0; shifts take a count from count_val, >> is
 * arithmetic; NEG ignores y). Returns 0 or the crash reason (division by
 * zero, negative shift count). */
static uint32_t arith32(uint32_t op, int32_t x, int32_t y, int32_t *r) {
    switch (op) {
    case OP_NEG: *r = wrap(0u - (uint32_t)x); return 0;
    case OP_ADD: *r = wrap((uint32_t)x + (uint32_t)y); return 0;
    case OP_SUB: *r = wrap((uint32_t)x - (uint32_t)y); return 0;
    case OP_MUL: *r = wrap((uint32_t)x * (uint32_t)y); return 0;
    case OP_AND: *r = x & y; return 0;
    case OP_OR:  *r = x | y; return 0;
    case OP_XOR: *r = x ^ y; return 0;
    case OP_ANDNOT: *r = x & ~y; return 0;
    case OP_SHL:
        if (y < 0) return R_BAD_SHIFT;
        *r = y > 31 ? 0 : wrap((uint32_t)x << y);
        return 0;
    case OP_SHR:
        if (y < 0) return R_BAD_SHIFT;
        *r = x >> (y > 31 ? 31 : y);
        return 0;
    case OP_DIV:
        if (y == 0) return R_DIV_ZERO;
        *r = (x == INT32_MIN && y == -1) ? INT32_MIN : x / y;
        return 0;
    default: /* OP_REM */
        if (y == 0) return R_DIV_ZERO;
        *r = (y == -1) ? 0 : x % y;     /* avoids INT32_MIN % -1 UB */
        return 0;
    }
}

/* The same for the int64 ops. */
static uint32_t arith64(uint32_t op, int64_t x, int64_t y, int64_t *r) {
    switch (op) {
    case OP_NEG64: *r = wrap64(0u - (uint64_t)x); return 0;
    case OP_ADD64: *r = wrap64((uint64_t)x + (uint64_t)y); return 0;
    case OP_SUB64: *r = wrap64((uint64_t)x - (uint64_t)y); return 0;
    case OP_MUL64: *r = wrap64((uint64_t)x * (uint64_t)y); return 0;
    case OP_AND64: *r = x & y; return 0;
    case OP_OR64:  *r = x | y; return 0;
    case OP_XOR64: *r = x ^ y; return 0;
    case OP_ANDNOT64: *r = x & ~y; return 0;
    case OP_SHL64:
        if (y < 0) return R_BAD_SHIFT;
        *r = y > 63 ? 0 : wrap64((uint64_t)x << y);
        return 0;
    case OP_SHR64:
        if (y < 0) return R_BAD_SHIFT;
        *r = x >> (y > 63 ? 63 : y);
        return 0;
    case OP_SHRU64:
        if (y < 0) return R_BAD_SHIFT;
        *r = y > 63 ? 0 : wrap64((uint64_t)x >> y);
        return 0;
    case OP_DIVU64: case OP_REMU64:
        if (y == 0) return R_DIV_ZERO;
        *r = wrap64(op == OP_DIVU64 ? (uint64_t)x / (uint64_t)y : (uint64_t)x % (uint64_t)y);
        return 0;
    case OP_DIV64:
        if (y == 0) return R_DIV_ZERO;
        *r = (x == INT64_MIN && y == -1) ? INT64_MIN : x / y;
        return 0;
    default: /* OP_REM64 */
        if (y == 0) return R_DIV_ZERO;
        *r = (y == -1) ? 0 : x % y;     /* avoids INT64_MIN % -1 UB */
        return 0;
    }
}

/* ---- println ---- */

/* out_str, unless only checking (o is NULL: deep_live). */
static void put(Out *o, const char *s) {
    if (o) out_str(o, s);
}

/* A slice, bare or inside a printed struct: [e0 e1 ...] (Go's println
 * prints [len/cap]address instead); a map header's entries (15.6: keys
 * and values alternate, the header printed "map" and set o->map to their
 * depth) as [k0:v0 k1:v1 ...]. out_slice and
 * out_term spend left as deep_live does and return what is left; once it
 * is spent the rest of the value is " ..." and no more (then -1). With o
 * NULL (deep_live) only a K_ARRAY's elements are walked. */
static int32_t out_slice(Out *o, const Vm *vm, const Actor *a, term t, const uint32_t *h, int depth,
                         int32_t left) {
    uint32_t off = h[1], len = h[2];
    bool map = o && o->map == depth;   /* a map's entries */
    term at = field_term(t, h[4]);   /* a const array (15.6) */
    const uint32_t *arr = obj_of(vm, a, at);
    if (depth >= PRINT_MAX_DEPTH) { put(o, "[...]"); return left; }
    put(o, "[");
    if (arr && len && off <= arr[1] && len <= arr[1] - off) {
        bool bytes = hdr_kind(arr[0]) == K_BYTEARR;
        for (uint32_t i = 0; i < len && left >= 0 && (o || hdr_kind(arr[0]) == K_ARRAY); i++) {
            if (i && left > 0) put(o, map && i & 1u ? ":" : " ");
            if (bytes) out_int(o, ((const uint8_t *)(arr + 2))[off + i]);
            else left = out_term(o, vm, a, field_term(at, arr[2 + off + i]), depth + 1, left);
        }
    }
    if (left >= 0) put(o, "]");
    return left;
}

/* The text of t into o. With o NULL, deep_live: the same walk printing
 * nothing, term_live of every term visited, WALK_STALE at the first that
 * is not. */
static int32_t out_term(Out *o, const Vm *vm, const Actor *a, term t, int depth, int32_t left) {
    if (left <= 0) {
        if (!left) put(o, " ...");
        return -1;
    }
    left--;
    if (!o && !term_live(vm, a, t)) return WALK_STALE;
    int32_t v;
    int64_t v64;
    const uint8_t *bp;
    uint32_t blen;
    const uint32_t *obj = obj_of(vm, a, t);
    uint32_t kind = obj ? hdr_kind(obj[0]) : 0;
    /* structs and slices first (no leaf below is one): deep_live walks them too */
    if (kind == K_STRUCT) {
        /* 15.6: a pointer's target after "&", a box as its one field, a
         * map as "map" and its entries slice (out_slice: o->map) */
        uint32_t fl = obj[1], box = fl & (STRUCT_BOX | STRUCT_MAP);
        if (fl & STRUCT_MAP) put(o, "map");
        else if (fl & STRUCT_MUT) put(o, "&");
        if (o) o->map = fl & STRUCT_MAP ? depth + 1 : -1;
        if (depth >= PRINT_MAX_DEPTH) { put(o, box ? "..." : "{...}"); return left; }
        if (!box) put(o, "{");
        for (uint32_t i = 2; i < hdr_words(obj[0]) && left >= 0; i++) {
            if (i > 2 && left > 0) put(o, " ");
            left = out_term(o, vm, a, field_term(t, obj[i]), depth + 1, left);
        }
        if (left >= 0 && !box) put(o, "}");
    } else if (kind == K_SLICE && hdr_words(obj[0]) == 5u) {
        left = out_slice(o, vm, a, t, obj, depth, left);
    } else if (!o) { }   /* deep_live: a leaf */
    else if (int_val(vm, a, t, &v)) out_int(o, v);
    else if (int64_val(vm, a, t, &v64)) out_int64(o, v64);
    else if (t == T_TRUE) out_str(o, "true");
    else if (t == T_FALSE) out_str(o, "false");
    else if (t == T_NIL) out_str(o, "nil");
    else if (t == T_EMPTYSTR) { }
    else if (t == T_ZEROREF) out_str(o, "<ref 0>");
    else if (t == T_NILSLICE) out_str(o, "[]");
    else if (bytes_view(vm, a, t, &bp, &blen)) {     /* a Bytes prints as its text */
        if (blen) out_bytes(o, (const char *)bp, blen);
    } else if (term_is_pid(t)) {
        out_str(o, "<pid ");
        const VmDist *d = vm_dist_of(vm, t);
        if (d) d->print(o, vm, t);   /* <pid N@node> */
        else out_int(o, term_pid_slot(t));
        out_str(o, ">");
    } else if (kind == K_STRING && obj[1] <= (hdr_words(obj[0]) - 2u) * 4u) {
        out_bytes(o, (const char *)(obj + 2), obj[1]);
    } else if (kind == K_CLOSURE) {
        out_str(o, "<func>");
    } else if (kind == K_BUF) {
        out_str(o, "<buffer>");
#if GOGO_FLOAT
    } else if (kind == K_FLOAT) {
        floatlib_print(o, obj);
#endif
    } else if (kind == K_REF && hdr_words(obj[0]) == 3u) {
        out_str(o, "<ref ");
        out_dec(o, obj[1], false);
        out_str(o, ">");
    } else {
        out_str(o, "?");
    }
    return left;
}

/* println(args...) on stream 1, bit i of mask marking args[i] a uint64
 * (an int64's bits), printed unsigned; panic(v) on stream 2, after
 * "panic: ". */
static void do_println(const Vm *vm, const Actor *a, const term *args, uint32_t n, uint32_t mask, int stream) {
    Out o;
    int64_t v;
    out_init(&o, vm, stream);
    if (stream == 2) out_str(&o, "panic: ");
    for (uint32_t i = 0; i < n; i++) {
        if (i) out_str(&o, " ");
        if (i < 30u && mask >> i & 1u && int64_val(vm, a, args[i], &v)) out_dec(&o, (uint64_t)v, false);
        else out_term(&o, vm, a, args[i], 0, WALK_BUDGET);
    }
    out_str(&o, "\n");
    out_flush(&o);
}

/* strs.h: the text println prints for t into o (B_SPRINT). */
bool term_text(Out *o, const Vm *vm, const Actor *a, term t) {
    if (deep_live(vm, a, t, 0, WALK_BUDGET) == WALK_STALE) return false;
    out_term(o, vm, a, t, 0, WALK_BUDGET);
    return true;
}

/* B_PANIC v: prints "panic: v" on stderr; returns the reason to crash
 * with, R_PANIC (R_BAD_CODE for a stale value). */
static uint32_t do_panic(const Vm *vm, const Actor *a, const term *v) {
    if (deep_live(vm, a, v[0], 0, WALK_BUDGET) == WALK_STALE) return R_BAD_CODE;
    do_println(vm, a, v, 1, 0, 2);
    return R_PANIC;
}

/* ---- actors ---- */

uint32_t vm_spawn_fn(Vm *vm, Actor *src, uint8_t mod, uint32_t f, term val, bool caps,
                     uint16_t max_words, bool link, term *pid) {
    const Image *img = &vm->mods[mod]->img;
    uint32_t nparams = image_func_nparams(img, f);
    uint32_t nregs = image_func_nregs(img, f);
    if (nparams > nregs) return R_BAD_CODE;

    Msg *m = NULL;
    uint32_t why;
    if (src && !(m = msg_build(vm, src, val, mod, &why))) return why;
    uint32_t words = nregs + 2u + (m ? m->words : 0u) + 8u;
    Actor *n = words <= HEAP_MAX_WORDS ? actor_new_in(vm, (uint16_t)words, mod) : NULL;
    /* The block is sized for the frame and the copy, so neither GCs; the
     * frame goes first anyway, so a GC could not drop the copy. */
    term copy = val;
    if (!n || !heap_push_frame(vm, n, (uint16_t)nregs, -1) || (m && !msg_unpack(vm, n, m, &copy))) {
        if (m) msg_free(vm, m);
        if (n) actor_free(vm, n);      /* never ran, never visible: not an exit */
        return R_OUT_OF_MEMORY;
    }
    if (m) msg_free(vm, m);
    if (max_words) n->max_words = max_words < words ? (uint16_t)words : max_words;
    if (caps) {
        const uint32_t *c = n->mem + term_ref_off(copy) + 2u;
        for (uint32_t i = 0; i < nparams; i++) n->mem[n->fp + i] = c[i];
    } else if (nparams) {
        n->mem[n->fp] = copy;
    }
    n->pc = img->code + image_func_code_off(img, f);
    if (link && !sig_link(vm, src, n->id)) {
        actor_free(vm, n);             /* never ran: not an exit */
        return R_OUT_OF_MEMORY;
    }
    /* Off-heap Bytes among the copied values are charged to the child in
     * full: if they take it past its limit, the child exits
     * with OutOfMemory before running (a visible exit: links and monitors
     * see it), as a holder over its limit does; the spawner is unaffected. */
    if ((uint32_t)n->mem_words + actor_charged_words(n) > actor_max_words(n))
        actor_mark_exit(vm, n, R_OUT_OF_MEMORY, T_EMPTYSTR);
    runq_push(vm, n);
    *pid = n->id;
    return R_NORMAL;
}

/* Spawns an actor running closure clo: its captures are copied into the new
 * actor like a message and become r0.. of the entry frame. The closure must
 * be one of a's own module (a closure of another module, live or gone, is
 * badcode): the child runs a's module, as every actor.Spawn child does.
 * max_words is the child's memory limit in words, 0 for the default; a
 * limit below the child's initial block is raised to the block size, so the
 * spawn succeeds (the child then has no room to grow). With `link`, the
 * child is linked to a before it is queued (SpawnLink); if that fails the
 * child is freed unrun. A child whose captured off-heap Bytes exceed its
 * limit is spawned but marked to exit with OutOfMemory before it runs. On
 * success sets *pid and returns R_NORMAL; otherwise returns the crash
 * reason for the caller. Allocates only in the new actor, so the caller's
 * heap does not move (msg_build marks it while copying and restores it
 * byte for byte). */
static uint32_t do_spawn(Vm *vm, Actor *a, term clo, uint16_t max_words, bool link, term *pid) {
    const Image *img = &vm->mods[a->mod]->img;
    const uint32_t *obj = obj_of(vm, a, clo);
    if (!obj || !term_is_ref(clo) || hdr_kind(obj[0]) != K_CLOSURE ||
        (obj[1] & MOD_TAG_MASK) != module_tag(vm, a->mod)) return R_BAD_CODE;
    uint32_t f = obj[1] & ~MOD_TAG_MASK;
    uint32_t ncaps = hdr_words(obj[0]) - 2u;
    if (f >= img->n_funcs || image_func_nparams(img, f) != ncaps) return R_BAD_CODE;
    return vm_spawn_fn(vm, a, a->mod, f, clo, true, max_words, link, pid);
}

/* The child limit SpawnOpt asks for: Options{MaxMemory} in bytes, as words
 * (0 or less: the default; rounded down but at least 1; at most
 * HEAP_MAX_WORDS). False if opts is not an Options struct. */
bool vm_spawn_opt_words(const Vm *vm, const Actor *a, term opts, uint16_t *out) {
    const uint32_t *obj = obj_of(vm, a, opts);
    int32_t bytes;
    if (!obj || hdr_kind(obj[0]) != K_STRUCT || obj[1] != TYPE_OPTIONS ||
        hdr_words(obj[0]) != 3u || !int_val(vm, a, obj[2], &bytes)) return false;
    uint32_t words = bytes > 0 ? (uint32_t)bytes / 4u : 0u;
    if (bytes > 0 && words == 0) words = 1;
    if (words > HEAP_MAX_WORDS) words = HEAP_MAX_WORDS;
    *out = (uint16_t)words;
    return true;
}

/* ---- timed waits ---- */

enum { WAIT_DONE, WAIT_BLOCK, WAIT_OOM };

/* The timer half of ReceiveTimeout(ms) and Sleep(ms), run each time the
 * BUILTIN executes: WAIT_BLOCK while the wait is on (arming the wakeup on
 * the first run), WAIT_DONE once it has timed out or for ms <= 0 (and
 * AF_TIMED_OUT is consumed), WAIT_OOM if the wakeup cannot be allocated. */
static int timed_wait(Vm *vm, Actor *a, int32_t ms) {
    if (ms <= 0 || (a->flags & AF_TIMED_OUT)) {
        a->flags &= (uint8_t)~AF_TIMED_OUT;
        return WAIT_DONE;
    }
    if (!timer_add_wakeup(vm, a, vm_now(vm) + (uint32_t)ms)) return WAIT_OOM;
    return WAIT_BLOCK;
}

/* ---- interpreter ---- */

/* What the decoder checks for each op before its case runs: whether an
 * ext word follows (OPF_EXT, as op_has_ext in opcodes.h) and which
 * operands name registers of the current frame (opcodes.h, the operand
 * column). The float ops check their own (float_exec); CALLV its
 * argument range rb+1 .. rb+c. */
#define OPF_EXT 1u     /* followed by an ext word */
#define OPF_A   2u     /* ra is a register */
#define OPF_B   4u     /* rb is a register */
#define OPF_C   8u     /* rc is a register */
#define OPF_BC  16u    /* rb .. rb+c-1 are registers */
#define OPF_AB  32u    /* ra .. ra+b-1 are registers */
#define OPF_W   64u    /* an int64 arithmetic op (the int ones run in 32 bits) */
#define OPF_SH  128u   /* rc is a shift count: any integer (count_val) */
#define OPF_AB_ (OPF_A | OPF_B)
#define OPF_ABC (OPF_A | OPF_B | OPF_C)
static const uint8_t op_info[] = {
    [OP_NOP] = 0, [OP_MOVE] = OPF_AB_, [OP_LOADK] = OPF_EXT | OPF_A, [OP_LOADI32] = OPF_EXT | OPF_A,
    [OP_ADD] = OPF_ABC, [OP_SUB] = OPF_ABC, [OP_MUL] = OPF_ABC, [OP_DIV] = OPF_ABC, [OP_REM] = OPF_ABC,
    [OP_NEG] = OPF_AB_, [OP_EQ] = OPF_ABC, [OP_NE] = OPF_ABC, [OP_LT] = OPF_ABC, [OP_LE] = OPF_ABC,
    [OP_NOT] = OPF_AB_, [OP_JMP] = OPF_EXT, [OP_JMPF] = OPF_EXT | OPF_A, [OP_JMPT] = OPF_EXT | OPF_A,
    [OP_CALL] = OPF_EXT | OPF_A | OPF_BC, [OP_RET] = OPF_A, [OP_NEWSTRUCT] = OPF_EXT | OPF_A | OPF_BC,
    [OP_GETFIELD] = OPF_AB_, [OP_MKCLOSURE] = OPF_EXT | OPF_A | OPF_BC, [OP_TYPEIS] = OPF_EXT | OPF_AB_,
    [OP_BUILTIN] = OPF_EXT | OPF_A | OPF_BC, [OP_CONV] = OPF_AB_,
    [OP_ADD64] = OPF_ABC | OPF_W, [OP_SUB64] = OPF_ABC | OPF_W, [OP_MUL64] = OPF_ABC | OPF_W, [OP_DIV64] = OPF_ABC | OPF_W, [OP_REM64] = OPF_ABC | OPF_W,
    [OP_NEG64] = OPF_AB_ | OPF_W, [OP_EQ64] = OPF_ABC, [OP_NE64] = OPF_ABC, [OP_LT64] = OPF_ABC, [OP_LE64] = OPF_ABC,
    [OP_BOX64] = OPF_AB_, [OP_CLEAR] = OPF_AB, [OP_RETN] = OPF_AB, [OP_CALLV] = OPF_AB_,
    [OP_AND] = OPF_ABC, [OP_OR] = OPF_ABC, [OP_XOR] = OPF_ABC, [OP_ANDNOT] = OPF_ABC,
    [OP_SHL] = OPF_ABC | OPF_SH, [OP_SHR] = OPF_ABC | OPF_SH,
    [OP_AND64] = OPF_ABC | OPF_W, [OP_OR64] = OPF_ABC | OPF_W, [OP_XOR64] = OPF_ABC | OPF_W, [OP_ANDNOT64] = OPF_ABC | OPF_W,
    [OP_SHL64] = OPF_ABC | OPF_W | OPF_SH, [OP_SHR64] = OPF_ABC | OPF_W | OPF_SH,
    [OP_ADDF] = 0, [OP_SUBF] = 0, [OP_MULF] = 0, [OP_DIVF] = 0, [OP_NEGF] = 0,
    [OP_EQF] = 0, [OP_NEF] = 0, [OP_LTF] = 0, [OP_LEF] = 0, [OP_CONVF] = 0,
    [OP_DIVU64] = OPF_ABC | OPF_W, [OP_REMU64] = OPF_ABC | OPF_W, [OP_LTU64] = OPF_ABC, [OP_LEU64] = OPF_ABC,
    [OP_SHRU64] = OPF_ABC | OPF_W | OPF_SH, [OP_SETFIELD] = OPF_AB_,
};
typedef char op_info_complete[sizeof op_info == OP_SETFIELD + 1u ? 1 : -1];

/* What OP_BUILTIN checks for each builtin before its case runs. The low
 * three bits: BI(n), exactly n arguments; BI_VAR, the builtin checks its
 * own count; 0, not a builtin (an id this VM does not have, B_FLOAT
 * without floats, the reserved ids), so an id left out of the table is
 * badcode, never "no arguments". BI_TWO: two results, ra+1 is a register
 * too. BI_PID: the first argument is a PID. BI_INT: the first argument is
 * an int, its value in iarg. */
#define BI(n)   ((n) + 1u)
#define BI_VAR  7u
#define BI_TWO  8u
#define BI_PID  16u
#define BI_INT  32u
static const uint8_t builtin_info[] = {
    [B_PRINTLN] = BI_VAR, [B_SPAWN] = BI(1), [B_SELF] = BI(0), [B_SEND] = BI(2), [B_RECEIVE] = BI(0),
    [B_MEMORY] = BI(1), [B_SPAWN_LINK] = BI(1), [B_SPAWN_OPT] = BI(2), [B_LINK] = BI(1) | BI_PID,
    [B_UNLINK] = BI(1) | BI_PID, [B_TRAP_EXIT] = BI(1), [B_EXIT] = BI(1) | BI_INT, [B_KILL] = BI(2) | BI_PID,
    [B_MONITOR] = BI(1) | BI_PID, [B_DEMONITOR] = BI(1), [B_MAKE_REF] = BI(0),
    [B_RECEIVE_TIMEOUT] = BI(1) | BI_INT | BI_TWO, [B_SEND_AFTER] = BI(3), [B_CANCEL_TIMER] = BI(1),
    [B_SLEEP] = BI(1) | BI_INT, [B_NOW] = BI(0), [B_CALL] = BI_VAR, [B_REPLY] = BI(2), [B_REGISTER] = BI(2),
    [B_WHEREIS] = BI(1) | BI_TWO, [B_UNREGISTER] = BI(1), [B_PORT_OPEN] = BI(2) | BI_TWO, [B_PORT_CLOSE] = BI(1),
    /* 28 .. 45, B_CONFIG_GET, B_CMP and B_SPRINT: strs_builtin checks their counts */
    [B_STR_CONCAT] = BI_VAR, [B_LEN] = BI_VAR, [B_INDEX] = BI_VAR, [B_SETINDEX] = BI_VAR, [B_SLICE] = BI_VAR,
    [B_MAKE_SLICE] = BI_VAR, [B_APPEND] = BI_VAR, [B_COPY] = BI_VAR, [B_CAP] = BI_VAR, [B_STR_FROM] = BI_VAR,
    [B_BYTESLICE_FROM] = BI_VAR, [B_STR_CMP] = BI_VAR, [B_BUF_NEW] = BI_VAR, [B_BUF_WRITE] = BI_VAR,
    [B_BUF_FREEZE] = BI_VAR, [B_BYTES_FROM] = BI_VAR, [B_BYTES_EQUAL] = BI_VAR, [B_BYTES_COPYTO] = BI_VAR,
    [B_PORT_HANDOFF] = BI(2), [B_NET_SUBSCRIBE] = BI(0), [B_CONFIG_GET] = BI_VAR | BI_TWO,
    [B_FS] = BI_VAR, [B_CODE] = BI_VAR, [B_UNIX_TIME] = BI(0), [B_SET_UNIX_TIME] = BI(1), [B_RANDOM] = BI(0),
    [B_FLOAT] = GOGO_FLOAT ? BI_VAR : 0u, [B_PRINTLNU] = BI_VAR, [B_PANIC] = BI(1), [B_CMP] = BI_VAR,
    [B_RESERVED_58] = 0, [B_RESERVED_59] = 0, [B_RESERVED_60] = 0, [B_SPRINT] = BI_VAR,
};
typedef char builtin_info_complete[sizeof builtin_info == B_SPRINT + 1u ? 1 : -1];

int interp_run(Vm *vm, Actor *a) {
    /* The actor's module is loaded while it lives (its live count). */
    const ModEntry *me = a->mod < GOGO_MAX_MODULES ? vm->mods[a->mod] : NULL;
    if (!me) {
        actor_exit(vm, a, R_BAD_CODE, T_EMPTYSTR);
        return RUN_EXIT;
    }
    const Image *img = &me->img;
    const uint32_t tag = module_tag(vm, a->mod);   /* ORed into what its image words become (term.h) */
    const uint32_t *code = img->code;
    uint32_t pc = (uint32_t)(a->pc - code);
    uint32_t fn = func_of_pc(img, pc);
    uint32_t nregs = image_func_nregs(img, fn);
    uint32_t budget = REDUCTIONS;
    uint32_t reason = R_BAD_CODE;
    term reason_func;
    term *R;
    int r;                       /* msg_send results */

#define RELOAD() (R = (term *)a->mem + a->fp)
#define CRASH(r) do { reason = (r); goto crash; } while (0)
#define CHECK(cond) do { if (!(cond)) CRASH(R_BAD_CODE); } while (0)
/* Registers r..r+n-1 all exist (n may be 0). */
#define REGS(r, n) do { if ((uint32_t)(r) + (uint32_t)(n) > nregs) CRASH(R_BAD_CODE); } while (0)

    /* Marked for exit while it was not running: exit before any instruction. */
    if (actor_exit_pending(a, &reason, &reason_func)) {
        actor_exit(vm, a, reason, reason_func);
        return RUN_EXIT;
    }
    RELOAD();
    /* The frame on top must be one of fn's: after a yield, fn is re-derived
     * from pc, which a malformed image could have moved into another
     * function. Check the frame's extent and its saved-fp/ret words. */
    {
        uint32_t top = (uint32_t)a->fp + nregs + 2u;
        CHECK(top <= a->mem_words && term_is_int(R[nregs]) && term_is_int(R[nregs + 1]));
        int32_t saved = term_to_int(R[nregs]);
        if (saved == 0) CHECK(top == a->mem_words && term_to_int(R[nregs + 1]) == -1);
        else CHECK(saved > 0 && (uint32_t)saved <= a->mem_words &&
                   a->mem_words - (uint32_t)saved == top);
    }
    for (;;) {
        CHECK(pc < img->code_words);
        uint32_t w = code[pc];
        uint32_t op = OP_OP(w), ra = OP_A(w), rb = OP_B(w), rc = OP_C(w);
        uint32_t ext = 0, next = pc + 1u, info;
        int64_t ires;      /* put_int: an integer result for ra, */
        bool wide;         /* an int64 (else an int: ires fits 32 bits) */
        CHECK(op < sizeof op_info);
        info = op_info[op];
        if (info & OPF_EXT) {
            CHECK(next < img->code_words);
            ext = code[next++];
        }
        {
            /* One past the highest register the operands name, from the a
             * side and the b/c side, against nregs: one compare, few
             * branches (an unknown op past the table never gets here). */
            uint32_t hi = info & OPF_A ? ra + 1u : info & OPF_AB ? ra + rb : 0u;
            uint32_t hb = info & OPF_B ? (info & OPF_C && rc > rb ? rc : rb) + 1u : info & OPF_BC ? rb + rc : 0u;
            CHECK((hi > hb ? hi : hb) <= nregs);
        }

        switch (op) {
        case OP_NOP:
            break;

        case OP_MOVE:
            R[ra] = R[rb];
            break;

        case OP_LOADK:
            /* An image may hold small ints, specials and untagged const refs
             * to an object start (image_const_start); never heap refs (they
             * would point into a random heap). A const ref gets the
             * module's tag (term.h). */
            CHECK(term_is_int(ext) || ext == T_NIL || ext == T_TRUE || ext == T_FALSE ||
                  ext == T_EMPTYSTR || ext == T_ZEROPID || ext == T_ZEROREF || ext == T_NILSLICE ||
                  ext == T_EMPTYBYTES ||
                  (term_is_const(ext) && !(ext & MOD_TAG_MASK) && image_const_start(img, term_const_off(ext))));
            R[ra] = term_is_const(ext) ? ext | tag : ext;
            break;

        case OP_LOADI32:
            ires = (int32_t)ext;
            wide = false;
            goto put_int;

        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_REM: case OP_NEG:
        case OP_AND: case OP_OR: case OP_XOR: case OP_ANDNOT: case OP_SHL: case OP_SHR:
        case OP_ADD64: case OP_SUB64: case OP_MUL64: case OP_DIV64: case OP_REM64: case OP_NEG64:
        case OP_AND64: case OP_OR64: case OP_XOR64: case OP_ANDNOT64:
        case OP_SHL64: case OP_SHR64: case OP_DIVU64: case OP_REMU64: case OP_SHRU64: {
            /* The int ops take ints (an int64 box is badcode) and run in
             * 32 bits (arith32); the int64 ops (OPF_W) take int64s in
             * either representation (arith64). A shift count (OPF_SH)
             * may be any integer; NEG has no rc. */
            int64_t x, y = 0;
            int32_t n, r32;
            uint32_t why;
            term t;
            wide = (info & OPF_W) != 0;
            /* a small int is the same value for both: no call */
            if (term_is_int(R[rb])) x = term_to_int(R[rb]);
            else CHECK(int_operand(vm, a, R[rb], wide, &x));
            if (info & OPF_SH) {
                CHECK(count_val(vm, a, R[rc], &n));
                y = n;
            } else if (info & OPF_C) {
                if (term_is_int(R[rc])) y = term_to_int(R[rc]);
                else CHECK(int_operand(vm, a, R[rc], wide, &y));
            }
            if (wide) why = arith64(op, x, y, &ires);
            else { why = arith32(op, (int32_t)x, (int32_t)y, &r32); ires = r32; }
            if (why != 0) CRASH(why);
        put_int:   /* LOADI32, CONV, the int-valued builtins */
            if (!(wide ? make_int64(vm, a, ires, &t) : make_int(vm, a, (int32_t)ires, &t))) CRASH(R_OUT_OF_MEMORY);
            RELOAD();
            R[ra] = t;
            break;
        }

        case OP_EQ64: case OP_NE64: case OP_LT64: case OP_LE64: case OP_LTU64: case OP_LEU64: {
            /* By value, whatever the representations. Unsigned: the
             * signed compare with both sign bits flipped. */
            int64_t x, y;
            bool r;
            CHECK(int64_val(vm, a, R[rb], &x) && int64_val(vm, a, R[rc], &y));
            if (op >= OP_LTU64) {
                x = wrap64((uint64_t)x ^ (uint64_t)INT64_MIN);
                y = wrap64((uint64_t)y ^ (uint64_t)INT64_MIN);
                op -= OP_LTU64 - OP_LT64;
            }
            switch (op) {
            case OP_EQ64: r = x == y; break;
            case OP_NE64: r = x != y; break;
            case OP_LT64: r = x < y; break;
            default:      r = x <= y; break;
            }
            R[ra] = term_from_bool(r);
            break;
        }

#if GOGO_FLOAT
        case OP_ADDF: case OP_SUBF: case OP_MULF: case OP_DIVF: case OP_NEGF:
        case OP_EQF: case OP_NEF: case OP_LTF: case OP_LEF: case OP_CONVF: {
            uint32_t why = float_exec(vm, a, w, nregs);   /* may GC */
            RELOAD();
            if (why != R_NORMAL) CRASH(why);
            break;
        }
#endif

        case OP_BOX64: {
            int64_t x;
            term t;
            CHECK(int64_val(vm, a, R[rb], &x));
            if (!term_is_int(R[rb])) { R[ra] = R[rb]; break; }   /* already a box */
            if (!box_int64(vm, a, x, &t)) CRASH(R_OUT_OF_MEMORY);
            RELOAD();
            R[ra] = t;
            break;
        }

        case OP_CONV: {
            /* c=0: int(rb) of an int64, truncating to the low 32 bits;
             * c=1: int64(rb) of an int, sign-extending; c=6: the same
             * zero-extending (of a uint32). c=2..5: uint8, uint16, int8,
             * int16 of an int or int64 (either representation): the low 8
             * (even c) or 16 bits, zero- (c < 4) or sign-extended. An
             * immediate operand whose value fits gives the same term
             * back (make_int, make_int64). */
            int32_t x;
            int64_t x64;
            bool box64 = !int_val(vm, a, R[rb], &x);   /* not an int: a K_INT64 */
            if (box64) CHECK(int64_val(vm, a, R[rb], &x64));
            else x64 = x;
            uint32_t u = (uint32_t)(uint64_t)x64, sh = rc & 1u ? 16u : 24u;
            CHECK(rc <= 6u && (rc == 0u ? box64 || term_is_int(R[rb]) : rc - 2u < 4u || !box64));
            wide = rc == 1u || rc == 6u;
            ires = rc == 6u ? (int64_t)u : rc == 1u ? x64 : rc == 0u ? wrap(u)
                 : rc >= 4u ? wrap(u << sh) >> sh : (int32_t)((u << sh) >> sh);
            goto put_int;
        }

        case OP_EQ: case OP_NE: {
            /* ints, bools, PIDs, Refs; an int64 box is badcode (EQ64). */
            int32_t x, y;
            bool eq;
            uint64_t sx, sy;
            if (int_val(vm, a, R[rb], &x) && int_val(vm, a, R[rc], &y)) eq = x == y;
            else if (!term_live(vm, a, R[rb]) || !term_live(vm, a, R[rc])) CRASH(R_BAD_CODE);
            else if (int64_box(vm, a, R[rb]) || int64_box(vm, a, R[rc])) CRASH(R_BAD_CODE);
            else if (ref_serial(vm, a, R[rb], &sx)) eq = ref_serial(vm, a, R[rc], &sy) && sx == sy;
            else if (strs_equal(vm, a, R[rb], R[rc], &eq)) {}   /* by content */
            else eq = R[rb] == R[rc];   /* a ref never equals a non-ref term */
            R[ra] = term_from_bool(op == OP_EQ ? eq : !eq);
            break;
        }

        case OP_LT: case OP_LE: {
            int32_t x, y;
            CHECK(int_val(vm, a, R[rb], &x) && int_val(vm, a, R[rc], &y));
            R[ra] = term_from_bool(op == OP_LT ? x < y : x <= y);
            break;
        }

        case OP_CLEAR:
            for (uint32_t i = 0; i < rb; i++) R[ra + i] = T_NIL;
            break;

        case OP_NOT:
            CHECK(R[rb] == T_TRUE || R[rb] == T_FALSE);
            R[ra] = term_from_bool(R[rb] == T_FALSE);
            break;

        case OP_JMP: case OP_JMPF: case OP_JMPT: {
            bool take = true;
            if (op != OP_JMP) {
                CHECK(R[ra] == T_TRUE || R[ra] == T_FALSE);
                take = (R[ra] == T_TRUE) == (op == OP_JMPT);
            }
            if (take) {
                CHECK(ext < img->code_words);
                if (ext <= pc) budget--;       /* backward jump */
                next = ext;
            }
            break;
        }

        case OP_CALL: {
            CHECK(ext < img->n_funcs);
            uint32_t callee_regs = image_func_nregs(img, ext);
            CHECK(image_func_nparams(img, ext) == rc && rc <= callee_regs);
            /* The caller's frame moves if the push GCs; remember it
             * top-relative, like the saved fp in the frame. */
            uint32_t caller_top = (uint32_t)a->mem_words - a->fp;
            if (!heap_push_frame(vm, a, (uint16_t)callee_regs, (int32_t)pc)) CRASH(R_OUT_OF_MEMORY);
            const term *args = a->mem + (a->mem_words - caller_top) + rb;
            for (uint32_t i = 0; i < rc; i++) a->mem[a->fp + i] = args[i];
            fn = ext;
            nregs = callee_regs;
            next = image_func_code_off(img, fn);
            RELOAD();
            budget--;
            break;
        }

        case OP_CALLV: {
            /* Closure in rb, args rb+1..rb+c. The callee's parameters are
             * the closure's captures followed by the args. A nil (or any
             * non-closure) value is badcode. */
            REGS(rb + 1u, rc);
            /* Only a closure of this module: another module's code cannot
             * run on this actor's frames (and a gone module's is stale). */
            const uint32_t *obj = obj_of(vm, a, R[rb]);
            CHECK(obj && term_is_ref(R[rb]) && hdr_kind(obj[0]) == K_CLOSURE &&
                  (obj[1] & MOD_TAG_MASK) == tag);
            uint32_t f = obj[1] & ~MOD_TAG_MASK;
            uint32_t ncaps = hdr_words(obj[0]) - 2u;
            CHECK(f < img->n_funcs);
            uint32_t callee_regs = image_func_nregs(img, f);
            CHECK(image_func_nparams(img, f) == ncaps + rc && ncaps + rc <= callee_regs);
            /* As for CALL; the closure is re-read through its register,
             * a GC root, after the push. */
            uint32_t caller_top = (uint32_t)a->mem_words - a->fp;
            if (!heap_push_frame(vm, a, (uint16_t)callee_regs, (int32_t)pc)) CRASH(R_OUT_OF_MEMORY);
            const term *args = a->mem + (a->mem_words - caller_top) + rb;
            const uint32_t *caps = a->mem + term_ref_off(args[0]) + 2u;
            for (uint32_t i = 0; i < ncaps; i++) a->mem[a->fp + i] = caps[i];
            for (uint32_t i = 0; i < rc; i++) a->mem[a->fp + ncaps + i] = args[1u + i];
            fn = f;
            nregs = callee_regs;
            next = image_func_code_off(img, fn);
            RELOAD();
            budget--;
            break;
        }

        case OP_RET: case OP_RETN: {
            /* RET returns ra into the CALL's destination register; RETN
             * returns ra..ra+b-1 into dst..dst+b-1. The callee frame is
             * popped first; its words are not touched until the copy, and
             * they lie below the caller's frame, so the ranges are
             * disjoint. */
            uint32_t n = op == OP_RET ? 1u : rb;
            uint32_t fp = a->fp;
            CHECK(term_is_int(R[nregs]) && term_is_int(R[nregs + 1]));
            int32_t saved = term_to_int(R[nregs]);
            int32_t ret = term_to_int(R[nregs + 1]);
            a->sp = (uint16_t)(fp + nregs + 2u);      /* pop the frame */
            if (ret == -1) {                           /* entry frame */
                actor_exit(vm, a, R_NORMAL, T_EMPTYSTR);
                return RUN_EXIT;
            }
            /* The caller's frame sits directly above ours. */
            CHECK(saved > 0 && (uint32_t)saved <= a->mem_words &&
                  a->mem_words - (uint32_t)saved == a->sp);
            CHECK(ret >= 0 && (uint32_t)ret + 1u < img->code_words &&
                  (OP_OP(code[ret]) == OP_CALL || OP_OP(code[ret]) == OP_CALLV));
            uint32_t caller = func_of_pc(img, (uint32_t)ret);
            uint32_t caller_regs = image_func_nregs(img, caller);
            uint32_t caller_fp = a->sp;
            CHECK(caller_fp + caller_regs + 2u <= a->mem_words);
            uint32_t dst = OP_A(code[ret]);
            CHECK(dst + n <= caller_regs);
            a->fp = (uint16_t)caller_fp;
            fn = caller;
            nregs = caller_regs;
            RELOAD();
            for (uint32_t i = 0; i < n; i++) R[dst + i] = a->mem[fp + ra + i];
            next = (uint32_t)ret + (OP_OP(code[ret]) == OP_CALL ? 2u : 1u);   /* CALL has an ext word, CALLV none */
            break;
        }

        case OP_NEWSTRUCT: case OP_MKCLOSURE: {
            uint32_t fl = 0;
            if (op == OP_NEWSTRUCT) {
                /* image_type_row, unrolled: a reserved row, or an own one;
                 * its word is the field count and the type's flags (term.h;
                 * image_load refuses a flagged reserved row) */
                uint32_t own = ext - TYPE_OWN_BASE, row = ext < img->n_reserved ? ext : own + img->n_reserved;
                CHECK((ext < img->n_reserved || own < img->n_types - img->n_reserved) &&
                      ((fl = img->types[row * 2u]) & ~STRUCT_FLAGS) == rc);
                fl &= STRUCT_FLAGS;
            }
            else CHECK(ext < img->n_funcs);
            uint32_t words = 2u + rc;
            uint32_t off = heap_alloc(vm, a, words, NULL, 0);
            if (off == UINT32_MAX) CRASH(R_OUT_OF_MEMORY);
            RELOAD();
            uint32_t *obj = a->mem + off;
            obj[0] = hdr_make(words, op == OP_NEWSTRUCT ? K_STRUCT : K_CLOSURE);
            /* The module's tag on its functions and its own types; a
             * reserved type stays bare (global). */
            obj[1] = ((op == OP_MKCLOSURE || ext >= TYPE_OWN_BASE) ? ext | tag : ext) | fl;
            for (uint32_t i = 0; i < rc; i++) obj[2 + i] = R[rb + i];
            R[ra] = term_from_ref(off);
            break;
        }

        case OP_GETFIELD: case OP_SETFIELD: {
            /* SETFIELD (15.6) writes in place, only a heap struct of a
             * mutable type: a const one is read-only (flash), an immutable
             * one may be shared. */
            uint32_t *obj = (uint32_t *)obj_of(vm, a, R[rb]);
            bool set = op == OP_SETFIELD;
            /* Past the end only of a reserved struct from an older image:
             * the fields appended since are ints, 0 the default (opcodes.h). */
            if (!(obj && hdr_kind(obj[0]) == K_STRUCT && (2u + rc < hdr_words(obj[0]) || (!set && obj[1] < TYPE_OWN_BASE)) &&
                  (!set || (term_is_ref(R[rb]) && (obj[1] & STRUCT_MUT))) &&
                  type_live(vm, tag, struct_type(R[rb], obj)))) CRASH(R[rb] == T_NIL ? R_NIL : R_BAD_CODE);
            if (set) obj[2 + rc] = R[ra];
            else R[ra] = 2u + rc < hdr_words(obj[0]) ? field_term(R[rb], obj[2 + rc]) : term_from_int(0);
            break;
        }

        case OP_TYPEIS: {
            term t = R[rb];
            uint32_t ty = UINT32_MAX;
            CHECK(term_live(vm, a, t));
            int32_t dummy;
            if (int_val(vm, a, t, &dummy)) ty = TY_INT;
            else if (t == T_TRUE || t == T_FALSE) ty = TY_BOOL;
            else if (t == T_EMPTYSTR) ty = TY_STRING;
            else if (term_is_pid(t)) ty = TY_PID;
            else if (t == T_ZEROREF) ty = TY_REF;
            else if (t == T_EMPTYBYTES || bytes_valid(vm, t)) ty = TY_BYTES;
            else {
                const uint32_t *obj = obj_of(vm, a, t);
                uint32_t kind = obj ? hdr_kind(obj[0]) : 0;
                if (kind == K_STRING) ty = TY_STRING;
                else if (kind == K_STRUCT) ty = struct_type(t, obj);   /* live: checked above */
                else if (kind == K_REF) ty = TY_REF;
                else if (kind == K_INT64) ty = TY_INT64;
                else if (kind == K_BYTES) ty = TY_BYTES;
#if GOGO_FLOAT
                else if (kind == K_FLOAT) ty = float_type(obj);
#endif
            }
            /* ext names a type of this module's image: its own struct
             * types are tagged like its structs. */
            CHECK(!(ext & MOD_TAG_MASK));
            R[ra] = term_from_bool(ty == (ext >= TYPE_OWN_BASE && ext < TY_INT ? ext | tag : ext));
            break;
        }

        case OP_BUILTIN: {
            uint32_t bi = ext < sizeof builtin_info ? builtin_info[ext] : 0u;
            int32_t iarg = 0;
            /* BI(n): rc == n; BI_VAR: any; 0 (bi - 1 wraps): none */
            CHECK((bi & 7u) == BI_VAR || (bi & 7u) - 1u == rc);
            if (bi & BI_TWO) CHECK(ra + 2u <= nregs);
            if (bi & BI_PID) CHECK(term_is_pid(R[rb]));
            if (bi & BI_INT) CHECK(int_val(vm, a, R[rb], &iarg));
            switch (ext) {
            case B_PRINTLN: case B_PRINTLNU: {
                /* Anything printed that names a gone module is badcode.
                 * B_PRINTLNU: rb is the mask of the uint64 values after it. */
                int32_t mask = 0;
                uint32_t first = ext == B_PRINTLNU;
                if (first) CHECK(rc && int_val(vm, a, R[rb], &mask));
                for (uint32_t i = first; i < rc; i++) CHECK(deep_live(vm, a, R[rb + i], 0, WALK_BUDGET) != WALK_STALE);
                do_println(vm, a, R + rb + first, rc - first, (uint32_t)mask, 1);
                R[ra] = T_NIL;
                break;
            }
            case B_SPAWN: case B_SPAWN_LINK: case B_SPAWN_OPT: {
                uint16_t max_words = 0;
                if (ext == B_SPAWN_OPT) CHECK(vm_spawn_opt_words(vm, a, R[rb + 1], &max_words));
                term pid;
                uint32_t err = do_spawn(vm, a, R[rb], max_words, ext == B_SPAWN_LINK, &pid);
                if (err != R_NORMAL) CRASH(err);
                R[ra] = pid;
                break;
            }
            case B_SELF:
                R[ra] = a->id;
                break;
            case B_SEND:
                if (port_is(vm, R[rb])) {
                    /* A command: handed to the driver in place, for any sender. */
                    CHECK(deep_live(vm, a, R[rb + 1], 0, WALK_BUDGET) >= 0);
                    port_command(vm, a, R[rb], R[rb + 1]);
                    R[ra] = T_NIL;
                    break;
                }
                /* Only a failed copy (out of memory, or refused by the
                 * module rules) concerns the sender; a dropped message
                 * does not. */
                if ((r = msg_send(vm, a, R[rb], R[rb + 1])) < 0) CRASH((uint32_t)-r);
                R[ra] = T_NIL;
                break;
            case B_RECEIVE: {
                term t;
                if (!msg_receive(vm, a, &t)) {
                    if (a->mbox_head) CRASH(R_OUT_OF_MEMORY);   /* message does not fit */
                    a->pc = code + pc;    /* retry this BUILTIN when woken */
                    return RUN_WAIT;
                }
                RELOAD();
                R[ra] = t;
                break;
            }
            case B_RECEIVE_TIMEOUT: {
                term t;
                if (msg_receive(vm, a, &t)) {
                    timer_cancel_wakeup(vm, a);
                    RELOAD();
                    R[ra] = t;
                    R[ra + 1] = T_TRUE;
                    break;
                }
                if (a->mbox_head) CRASH(R_OUT_OF_MEMORY);   /* message does not fit */
                switch (timed_wait(vm, a, iarg)) {
                case WAIT_OOM: CRASH(R_OUT_OF_MEMORY);
                case WAIT_BLOCK: a->pc = code + pc; return RUN_WAIT;   /* retry when woken */
                default: break;                                        /* timed out, or ms <= 0 */
                }
                R[ra] = T_NIL;
                R[ra + 1] = T_FALSE;
                break;
            }
            case B_SLEEP: {
                switch (timed_wait(vm, a, iarg)) {
                case WAIT_OOM: CRASH(R_OUT_OF_MEMORY);
                case WAIT_BLOCK: a->pc = code + pc; return RUN_WAIT;   /* also after a message */
                default: break;
                }
                R[ra] = T_NIL;
                break;
            }
            case B_CALL: {
                CHECK(rc == 3);
                REGS(ra, 2);
                uint64_t serial;
                if (!(a->flags & AF_CALL_PENDING)) {
                    int32_t ms;
                    CHECK(int_val(vm, a, R[rb + 2], &ms));
                    term pid = R[rb];
                    /* Request{From: self, Ref: new ref, Body: req}. The
                     * ref is a root across the second allocation. */
                    term roots[1];
                    roots[0] = heap_new_ref(vm, a);
                    if (roots[0] == T_NIL) CRASH(R_OUT_OF_MEMORY);
                    RELOAD();
                    uint32_t off = heap_alloc(vm, a, 5, roots, 1);
                    if (off == UINT32_MAX) CRASH(R_OUT_OF_MEMORY);
                    RELOAD();
                    uint32_t *obj = a->mem + off;
                    obj[0] = hdr_make(5, K_STRUCT);
                    obj[1] = TYPE_REQUEST;
                    obj[2] = a->id;
                    obj[3] = roots[0];
                    obj[4] = R[rb + 1];
                    if ((r = msg_send(vm, a, pid, term_from_ref(off))) < 0) CRASH((uint32_t)-r);
                    if (r == 1) {   /* no live target (a dead local actor, a remote one with no route) */
                        R[ra] = T_NIL;
                        R[ra + 1] = T_FALSE;
                        break;
                    }
                    R[ra] = roots[0];
                    CHECK(ref_serial(vm, a, R[ra], &serial));
                    if (ms > 0) {
                        if (!timer_add_wakeup(vm, a, vm_now(vm) + (uint32_t)ms))
                            CRASH(R_OUT_OF_MEMORY);
                        a->flags |= AF_CALL_PENDING;
                    }
                } else {
                    CHECK(ref_serial(vm, a, R[ra], &serial));
                }
                term t = T_NIL;
                int got = msg_take_reply(vm, a, serial, &t);
                if (got < 0) CRASH(R_OUT_OF_MEMORY);   /* reply does not fit */
                bool pending = (a->flags & AF_CALL_PENDING) != 0;
                if (got == 0 && pending && !(a->flags & (AF_TIMED_OUT | AF_EXIT_PENDING))) {
                    a->pc = code + pc;    /* block; the request is not re-sent */
                    return RUN_WAIT;
                }
                /* Replied, timed out, ms <= 0 with no reply yet, or the
                 * caller is already marked to exit (its own request went
                 * over its quota): do not block; the exit check below runs. */
                timer_cancel_wakeup(vm, a);
                a->flags &= (uint8_t)~AF_CALL_PENDING;
                RELOAD();
                R[ra] = got == 1 ? t : T_NIL;   /* 2: failed (its node is gone) */
                R[ra + 1] = term_from_bool(got == 1);
                break;
            }
            case B_REPLY: {
                const uint32_t *req = obj_of(vm, a, R[rb]);
                CHECK(req && hdr_kind(req[0]) == K_STRUCT && req[1] == TYPE_REQUEST &&
                      hdr_words(req[0]) == 5u);
                /* A dead sender, or a Request not made by Call (no Ref:
                 * serial 0), drops it. */
                uint64_t serial;
                if (!ref_serial(vm, a, req[3], &serial)) serial = 0;
                if ((r = msg_send_reply(vm, a, req[2], serial, R[rb + 1])) < 0) CRASH((uint32_t)-r);
                R[ra] = T_NIL;
                break;
            }
            case B_SEND_AFTER: {
                /* like Send, a non-PID target just drops it; ms is checked
                 * after the remote-PID refusal (not BI_INT), whose reason
                 * comes first */
                if (vm_dist_of(vm, R[rb + 1])) CRASH(R_BAD_MSG);   /* no timers to remote PIDs yet */
                CHECK(int_val(vm, a, R[rb], &iarg));
                uint64_t deadline = vm_now(vm) + (iarg > 0 ? (uint32_t)iarg : 0u);
                uint32_t why;
                term ref = timer_add_send(vm, a, deadline, R[rb + 1], R[rb + 2], &why);
                if (ref == T_NIL) CRASH(why);
                RELOAD();
                R[ra] = ref;
                break;
            }
            case B_CANCEL_TIMER: {
                uint64_t serial;
                CHECK(ref_serial(vm, a, R[rb], &serial));
                R[ra] = term_from_bool(serial != 0 && timer_cancel(vm, serial));   /* zero Ref: false */
                break;
            }
            /* NOW, MEMORY and UNIX_TIME end in put_int: they cannot
             * mark the running actor, so the exit check below has
             * nothing to see. */
            case B_NOW:
                /* An immediate (no allocation) for the first ~12 days. */
                ires = (int64_t)vm_now(vm);
                wide = true;
                goto put_int;
            case B_MEMORY: {
                /* The caller's own state (mem, mem_words) is current in a. */
                const Actor *p = actor_lookup(vm, R[rb]);
                ires = (int32_t)(p ? actor_memory_bytes(p) : 0u);
                wide = false;
                goto put_int;
            }
            case B_MAKE_REF: {
                term ref = heap_new_ref(vm, a);
                if (ref == T_NIL) CRASH(R_OUT_OF_MEMORY);
                RELOAD();
                R[ra] = ref;
                break;
            }
            case B_MONITOR: {
                term ref = sig_monitor(vm, a, R[rb]);
                if (ref == T_NIL) CRASH(R_OUT_OF_MEMORY);
                RELOAD();
                R[ra] = ref;
                break;
            }
            case B_DEMONITOR: {
                uint64_t serial;
                CHECK(ref_serial(vm, a, R[rb], &serial));
                if (serial != 0) sig_demonitor(vm, a, serial);   /* zero Ref: nothing */
                R[ra] = T_NIL;
                break;
            }
            case B_LINK:
                if (!sig_link(vm, a, R[rb])) CRASH(R_OUT_OF_MEMORY);
                R[ra] = T_NIL;
                break;
            case B_UNLINK:
                sig_unlink(vm, a, R[rb]);
                R[ra] = T_NIL;
                break;
            case B_TRAP_EXIT:
                CHECK(R[rb] == T_TRUE || R[rb] == T_FALSE);
                sig_set_trap(a, R[rb] == T_TRUE);
                R[ra] = T_NIL;
                break;
            case B_KILL: {
                int32_t c;
                CHECK(int_val(vm, a, R[rb + 1], &c));
                if (port_is(vm, R[rb])) port_close(vm, R[rb]);   /* any caller, any code */
                else if (!sig_kill(vm, a, R[rb], (uint32_t)c)) CRASH(R_OUT_OF_MEMORY);
                R[ra] = T_NIL;
                break;
            }
            case B_EXIT:
                actor_exit(vm, a, (uint32_t)iarg, T_EMPTYSTR);
                return RUN_EXIT;
            case B_REGISTER: {
                CHECK(term_is_pid(R[rb + 1]));
                const uint8_t *name;
                uint32_t len;
                CHECK(name_bytes(vm, a, R[rb], &name, &len));
                r = names_register(vm, a->id, name, len, R[rb + 1]);
                if (r < 0) CRASH(R_OUT_OF_MEMORY);
                R[ra] = term_from_bool(r > 0);
                break;
            }
            case B_WHEREIS: {
                const uint8_t *name;
                uint32_t len;
                CHECK(name_bytes(vm, a, R[rb], &name, &len));
                term pid;
                bool found = names_whereis(vm, name, len, &pid);
                R[ra] = found ? pid : T_ZEROPID;
                R[ra + 1] = term_from_bool(found);
                break;
            }
            case B_UNREGISTER: {
                const uint8_t *name;
                uint32_t len;
                CHECK(name_bytes(vm, a, R[rb], &name, &len));
                names_unregister(vm, a->id, name, len);
                R[ra] = T_NIL;
                break;
            }
            case B_PORT_OPEN: {
                CHECK(deep_live(vm, a, R[rb], 0, WALK_BUDGET) >= 0 &&
                      deep_live(vm, a, R[rb + 1], 0, WALK_BUDGET) >= 0);
                term pid = port_open(vm, a, R[rb], R[rb + 1]);   /* no GC */
                R[ra] = pid != T_NIL ? pid : T_ZEROPID;   /* failed: the zero PID */
                R[ra + 1] = term_from_bool(pid != T_NIL);
                break;
            }
            case B_PORT_CLOSE:
                port_close(vm, R[rb]);    /* anything but an open port's PID
                                           * (e.g. from a failed Open): nothing */
                R[ra] = T_NIL;
                break;
            case B_PORT_HANDOFF:
                R[ra] = term_from_bool(port_handoff(vm, a, R[rb], R[rb + 1]));   /* no GC */
                break;
            case B_NET_SUBSCRIBE:
                R[ra] = term_from_bool(port_subscribe(vm, a));   /* no GC */
                break;
            case B_UNIX_TIME:
                ires = vm_unix_time_ms(vm);
                wide = true;
                goto put_int;
            case B_SET_UNIX_TIME: {
                int64_t ms;
                CHECK(int64_val(vm, a, R[rb], &ms));
                R[ra] = term_from_bool(vm_set_unix_time_ms(vm, ms) != 0);
                break;
            }
            case B_RANDOM:
                R[ra] = term_from_int((int32_t)(vm_random(vm) & 0x3FFFFFFFu));
                break;
            default: {
                /* The library builtins (strs.c, module.c, the fs hook,
                 * floatlib) and panic: each checks what the table does
                 * not (strs_builtin, module_builtin, fs_builtin and
                 * float_lib their argument counts). May GC. */
                uint32_t err = ext == B_PANIC ? do_panic(vm, a, R + rb)
                             : ext == B_CODE ? module_builtin(vm, a, ra, rb, rc, nregs)
                             : ext == B_FS ? (vm->fs_builtin ? vm->fs_builtin(vm, a, ra, rb, rc, nregs) : R_BAD_CODE)
#if GOGO_FLOAT
                             : ext == B_FLOAT ? float_lib(vm, a, ra, rb, rc, nregs)
#endif
                             : strs_builtin(vm, a, ext, ra, rb, rc);
                RELOAD();
                if (err != R_NORMAL) CRASH(err);
                break;
            }
            }
            /* A builtin may have marked the running actor itself (a send
             * to itself over its quota, Kill of itself, Link of a dead pid):
             * exit now, not when next scheduled. */
            if (actor_exit_pending(a, &reason, &reason_func)) {
                actor_exit(vm, a, reason, reason_func);
                return RUN_EXIT;
            }
            break;
        }

        default:
            CRASH(R_BAD_CODE);
        }

        pc = next;
        if (budget == 0) {
            a->pc = code + pc;
            a->reductions = 0;
            return RUN_YIELD;
        }
    }

crash:
    actor_exit(vm, a, reason, term_from_const(image_func_name_off(img, fn)) | tag);
    return RUN_EXIT;

#undef RELOAD
#undef CRASH
#undef CHECK
#undef REGS
}

/* ---- scheduler ---- */

#define write_err_lit(s) vm_write_err(vm, (s), sizeof(s) - 1u)

/* Scheduler hooks into the port layer.
 *  sched_drain:    delivers the event ring and any due trailing debounce
 *                  values and drains the extra sources (net RX/link slots,
 *                  port_add_source); run at the top of every scheduler
 *                  pass. Reads the clock only when there is something to
 *                  do (port_work_pending).
 *  sched_any_live: true while some open port could still produce events;
 *                  the VM does not finish while it holds.
 * port_next_deadline (the earliest pending debounce or source deadline)
 * is folded into vm_next_deadline; a deadline already due is serviced by
 * the next sched_drain. */
static void sched_drain(Vm *vm) {
    if (port_work_pending(vm)) port_drain(vm, vm_now(vm));
}
static bool sched_any_live(Vm *vm) { return port_any_live(vm); }

int vm_start(Vm *vm) {
    if (vm->run_state != VM_ST_NEW) return 1;   /* a Vm runs once */
    if (!vm_reserve_init(vm)) {
        write_err_lit("gogo: cannot reserve emergency memory\n");
        return 1;
    }
    /* Module 0's main starts like any module's entry (code.Spawn). */
    term pid;
    int err = module_spawn(vm, NULL, module_handle(vm, 0), (const uint8_t *)"main", 4u, T_NIL, 0, &pid);
    if (err != GOGO_E_OK) {
        vm_reserve_release(vm);
        if (err == GOGO_E_NOTFOUND) write_err_lit("gogo: image has no main\n");
        else write_err_lit("gogo: cannot start main actor\n");
        return 1;
    }
    /* time 0 for Now() and timers (the wall clock does not move) */
    vm->t0 += vm_now(vm);
    evq_reset(vm);
    vm->run_state = VM_ST_RUN;
    return 0;
}

bool vm_next_deadline(const Vm *vm, uint64_t *abs_vm_ms) {
    uint64_t next, pn;
    bool have = timer_next(vm, &next);
    if (port_next_deadline(vm, &pn) && (!have || pn < next)) { next = pn; have = true; }
    if (have) *abs_vm_ms = next;
    return have;
}

int vm_step(Vm *vm, uint32_t budget) {
    if (vm->run_state != VM_ST_RUN) return VM_DONE;
    for (bool spent = false;;) {
        /* Once the budget is spent, nothing more runs: the last pass only
         * classifies what is left (runnable, due, idle or done). */
        Actor *a = NULL;
        if (!spent) {
            sched_drain(vm);
            /* Due timers fire between time slices too, so a busy actor
             * cannot hold them back. The clock is read only when a timer
             * is pending (here) or when the scheduler is about to go idle
             * (below). */
            if (vm->ntimers) timer_fire_due(vm, vm_now(vm));
            a = runq_pop(vm);
        } else if (vm->runq_head) {
            return VM_RUNNABLE;
        }
        if (a) {
            switch (interp_run(vm, a)) {
            case RUN_YIELD:
                runq_push(vm, a);
                break;
            case RUN_WAIT:
                /* Not requeued: a message, a timer or an exit mark pushes it
                 * back exactly once (only a waiting actor is pushed). */
                a->state = A_WAITING;
                break;
            default:
                /* RUN_EXIT: actor_exit has already freed it. */
                break;
            }
        } else {
            /* Nothing runnable. Done only when no timer is pending and no
             * input source is live (every actor exited or blocked forever). */
            uint64_t next;
            bool have = vm_next_deadline(vm, &next);
            if (!have && !sched_any_live(vm)) {
                vm->run_state = VM_ST_DONE;
                return VM_DONE;
            }
            /* Still work now, not idle: port_drain stops after
             * GOGO_EVQ_CAP events (more are waiting); a source whose drain
             * left work behind (a sliced TLS handshake step) drains again
             * now, not at the next unrelated wakeup; a timer is due. Not
             * port_work_pending: a held debounce value is due only at its
             * deadline, which is in next (no spinning until then). */
            if (!port_due_now(vm) && (!have || next > vm_now(vm))) return VM_IDLE;
            if (spent) return VM_RUNNABLE;
        }
        spent = budget && --budget == 0;
    }
}

void vm_stop(Vm *vm) {
    port_close_all(vm);              /* output drivers always get close */
    vm_reserve_release(vm);
    vm->run_state = VM_ST_DONE;
}

int vm_run(Vm *vm) {
    if (!vm->host->wait || vm_start(vm)) return 1;   /* no wait: the host steps */
    while (vm_step(vm, 0) != VM_DONE) {
        /* Idle: wait for the next deadline or a port event. An event pushed
         * after vm_step's last check is not lost: producers wake the host
         * after evq_push, and host->wait returns at once for any wake
         * latched since its last return. */
        uint64_t next, now;
        if (!vm_next_deadline(vm, &next)) {
            vm_wait(vm, UINT32_MAX);
        } else {
            now = vm_now(vm);
            if (next > now) vm_wait(vm, next - now > UINT32_MAX - 1u ? UINT32_MAX - 1u : (uint32_t)(next - now));
        }
    }
    vm_stop(vm);
    return 0;
}
