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
 * register indices are checked against the current function's n_regs, the
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

/* A term word read out of the object t names: a const ref inside a const
 * object is untagged in its image and gets the container's tag. */
static inline term field_term(term t, term f) {
    return (term_is_const(t) && term_is_const(f)) ? f | (t & MOD_TAG_MASK) : f;
}

/* The type word of the struct obj that t names: a const struct's word is
 * untagged in its image; a module type gets the module's tag. */
static inline uint32_t struct_type(term t, const uint32_t *obj) {
    uint32_t ty = obj[1];
    return (term_is_const(t) && ty >= TYPE_OWN_BASE) ? ty | (t & MOD_TAG_MASK) : ty;
}

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
 * can grow as width^depth for a value of a few hundred words. A value
 * without such sharing cannot reach the limit: every visit but the first
 * reads a distinct word of the actor's heap (at most HEAP_MAX_WORDS). The
 * println walk (out_term) visits the same terms in the same order under
 * the same limit, so it never prints what was not checked. */
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
static int32_t deep_live(const Vm *vm, const Actor *a, term t, int depth, int32_t left) {
    if (left <= 0) return WALK_CUT;
    left--;
    if (!term_live(vm, a, t)) return WALK_STALE;
    if (depth >= PRINT_MAX_DEPTH) return left;
    const uint32_t *obj = obj_of(vm, a, t);
    uint32_t kind = obj ? hdr_kind(obj[0]) : 0;
    if (kind == K_STRUCT) {
        for (uint32_t i = 2; left >= 0 && i < hdr_words(obj[0]); i++)
            left = deep_live(vm, a, field_term(t, obj[i]), depth + 1, left);
    } else if (kind == K_SLICE && hdr_words(obj[0]) == 5u) {
        const uint32_t *arr = obj_of(vm, a, obj[4]);
        uint32_t off = obj[1], len = obj[2];
        if (arr && hdr_kind(arr[0]) == K_ARRAY && len && off <= arr[1] && len <= arr[1] - off) {
            for (uint32_t i = 0; left >= 0 && i < len; i++)
                left = deep_live(vm, a, arr[2 + off + i], depth + 1, left);
        }
    }
    return left;
}

/* ---- ints ---- */

/* Term for v, boxing it into a K_INT on the heap when it does not fit in 31
 * bits. May GC (the caller reloads register pointers). False on OOM. */
static bool make_int(Vm *vm, Actor *a, int32_t v, term *out) {
    if (term_int_fits(v)) {
        *out = term_from_int(v);
        return true;
    }
    uint32_t off = heap_alloc(vm, a, 2, NULL, 0);
    if (off == UINT32_MAX) return false;
    a->mem[off] = hdr_make(2, K_INT);
    a->mem[off + 1] = (uint32_t)v;
    *out = term_from_ref(off);
    return true;
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
    uint32_t off = heap_alloc(vm, a, 3, NULL, 0);
    if (off == UINT32_MAX) return false;
    a->mem[off] = hdr_make(3, K_INT64);
    a->mem[off + 1] = (uint32_t)(uint64_t)v;
    a->mem[off + 2] = (uint32_t)((uint64_t)v >> 32);
    *out = term_from_ref(off);
    return true;
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

/* x op y for the binary arithmetic ops with Go semantics (wrapping; MinInt
 * / -1 == MinInt; x % -1 == 0; shifts take a count from count_val, >> is
 * arithmetic). Returns 0 or the crash reason (division by zero, negative
 * shift count). */
static uint32_t arith32(uint32_t op, int32_t x, int32_t y, int32_t *r) {
    switch (op) {
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

static uint32_t arith64(uint32_t op, int64_t x, int64_t y, int64_t *r) {
    switch (op) {
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

static int32_t out_term(Out *o, const Vm *vm, const Actor *a, term t, int depth, int32_t left);

/* A slice, bare or inside a printed struct: [e0 e1 ...] (Go's println
 * prints [len/cap]address instead). out_slice and out_term spend left as
 * deep_live does and return what is left; once it is spent the rest of
 * the value is " ..." and no more (then -1). */
static int32_t out_slice(Out *o, const Vm *vm, const Actor *a, const uint32_t *h, int depth,
                         int32_t left) {
    uint32_t off = h[1], len = h[2];
    const uint32_t *arr = obj_of(vm, a, h[4]);
    if (depth >= PRINT_MAX_DEPTH) { out_str(o, "[...]"); return left; }
    out_str(o, "[");
    if (arr && len && off <= arr[1] && len <= arr[1] - off) {
        bool bytes = hdr_kind(arr[0]) == K_BYTEARR;
        for (uint32_t i = 0; i < len && left >= 0; i++) {
            if (i && left > 0) out_str(o, " ");
            if (bytes) out_int(o, ((const uint8_t *)(arr + 2))[off + i]);
            else left = out_term(o, vm, a, arr[2 + off + i], depth + 1, left);
        }
    }
    if (left >= 0) out_str(o, "]");
    return left;
}

static int32_t out_term(Out *o, const Vm *vm, const Actor *a, term t, int depth, int32_t left) {
    if (left <= 0) {
        if (!left) out_str(o, " ...");
        return -1;
    }
    left--;
    int32_t v;
    int64_t v64;
    const uint8_t *bp;
    uint32_t blen;
    const uint32_t *obj = obj_of(vm, a, t);
    uint32_t kind = obj ? hdr_kind(obj[0]) : 0;
    if (int_val(vm, a, t, &v)) out_int(o, v);
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
        out_int(o, term_pid_slot(t));
        out_str(o, ">");
    } else if (kind == K_STRING && obj[1] <= (hdr_words(obj[0]) - 2u) * 4u) {
        out_bytes(o, (const char *)(obj + 2), obj[1]);
    } else if (kind == K_STRUCT) {
        if (depth >= PRINT_MAX_DEPTH) { out_str(o, "{...}"); return left; }
        out_str(o, "{");
        for (uint32_t i = 2; i < hdr_words(obj[0]) && left >= 0; i++) {
            if (i > 2 && left > 0) out_str(o, " ");
            left = out_term(o, vm, a, field_term(t, obj[i]), depth + 1, left);
        }
        if (left >= 0) out_str(o, "}");
    } else if (kind == K_SLICE && hdr_words(obj[0]) == 5u) {
        left = out_slice(o, vm, a, obj, depth, left);
    } else if (kind == K_CLOSURE) {
        out_str(o, "<func>");
    } else if (kind == K_BUF) {
        out_str(o, "<buffer>");
    } else if (kind == K_REF && hdr_words(obj[0]) == 3u) {
        out_str(o, "<ref ");
        out_dec(o, obj[1], false);
        out_str(o, ">");
    } else {
        out_str(o, "?");
    }
    return left;
}

static void do_println(const Vm *vm, const Actor *a, const term *args, uint32_t n) {
    Out o;
    out_init(&o, vm, 1);
    for (uint32_t i = 0; i < n; i++) {
        if (i) out_str(&o, " ");
        out_term(&o, vm, a, args[i], 0, WALK_BUDGET);
    }
    out_str(&o, "\n");
    out_flush(&o);
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
#define REG(r) do { if ((uint32_t)(r) >= nregs) CRASH(R_BAD_CODE); } while (0)
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
        uint32_t ext = 0;
        if (op_has_ext(op)) {
            CHECK(pc + 1u < img->code_words);
            ext = code[pc + 1u];
        }
        uint32_t next = pc + (op_has_ext(op) ? 2u : 1u);

        switch (op) {
        case OP_NOP:
            break;

        case OP_MOVE:
            REG(ra); REG(rb);
            R[ra] = R[rb];
            break;

        case OP_LOADK:
            REG(ra);
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

        case OP_LOADI32: {
            REG(ra);
            term t;
            if (!make_int(vm, a, (int32_t)ext, &t)) CRASH(R_OUT_OF_MEMORY);
            RELOAD();
            R[ra] = t;
            break;
        }

        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_REM:
        case OP_AND: case OP_OR: case OP_XOR: case OP_ANDNOT: case OP_SHL: case OP_SHR: {
            /* ints only (an int64 box is badcode; see the *64 ops), except
             * a shift count, which may be any integer. */
            REG(ra); REG(rb); REG(rc);
            int32_t x, y, r;
            uint32_t why;
            term t;
            CHECK(int_val(vm, a, R[rb], &x));
            CHECK((op == OP_SHL || op == OP_SHR) ? count_val(vm, a, R[rc], &y)
                                                 : int_val(vm, a, R[rc], &y));
            if ((why = arith32(op, x, y, &r)) != 0) CRASH(why);
            if (!make_int(vm, a, r, &t)) CRASH(R_OUT_OF_MEMORY);
            RELOAD();
            R[ra] = t;
            break;
        }

        case OP_NEG: {
            REG(ra); REG(rb);
            int32_t x;
            term t;
            CHECK(int_val(vm, a, R[rb], &x));
            if (!make_int(vm, a, wrap(0u - (uint32_t)x), &t)) CRASH(R_OUT_OF_MEMORY);
            RELOAD();
            R[ra] = t;
            break;
        }

        case OP_ADD64: case OP_SUB64: case OP_MUL64: case OP_DIV64: case OP_REM64:
        case OP_AND64: case OP_OR64: case OP_XOR64: case OP_ANDNOT64:
        case OP_SHL64: case OP_SHR64: {
            REG(ra); REG(rb); REG(rc);
            int64_t x, y, r;
            int32_t n;
            uint32_t why;
            term t;
            CHECK(int64_val(vm, a, R[rb], &x));
            if (op == OP_SHL64 || op == OP_SHR64) {     /* the count may be an int */
                CHECK(count_val(vm, a, R[rc], &n));
                y = n;
            } else {
                CHECK(int64_val(vm, a, R[rc], &y));
            }
            if ((why = arith64(op, x, y, &r)) != 0) CRASH(why);
            if (!make_int64(vm, a, r, &t)) CRASH(R_OUT_OF_MEMORY);
            RELOAD();
            R[ra] = t;
            break;
        }

        case OP_NEG64: {
            REG(ra); REG(rb);
            int64_t x;
            term t;
            CHECK(int64_val(vm, a, R[rb], &x));
            if (!make_int64(vm, a, wrap64(0u - (uint64_t)x), &t)) CRASH(R_OUT_OF_MEMORY);
            RELOAD();
            R[ra] = t;
            break;
        }

        case OP_EQ64: case OP_NE64: case OP_LT64: case OP_LE64: {
            /* By value, whatever the representations. */
            REG(ra); REG(rb); REG(rc);
            int64_t x, y;
            bool r;
            CHECK(int64_val(vm, a, R[rb], &x) && int64_val(vm, a, R[rc], &y));
            switch (op) {
            case OP_EQ64: r = x == y; break;
            case OP_NE64: r = x != y; break;
            case OP_LT64: r = x < y; break;
            default:      r = x <= y; break;
            }
            R[ra] = term_from_bool(r);
            break;
        }

        case OP_BOX64: {
            REG(ra); REG(rb);
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
             * c=1: int64(rb) of an int, sign-extending. An immediate is
             * the same term in both types (its value fits either way).
             * c=2: byte(rb) of an int or int64 (either representation),
             * the low 8 bits: always an immediate. */
            REG(ra); REG(rb);
            CHECK(rc <= 2u);
            int32_t x;
            int64_t x64;
            term t;
            bool ok;
            if (rc == 2u) {
                if (int_val(vm, a, R[rb], &x)) x64 = x;
                else CHECK(int64_val(vm, a, R[rb], &x64));
                R[ra] = term_from_int((int32_t)((uint64_t)x64 & 0xFFu));
                break;
            }
            if (rc == 0u) {
                CHECK(int64_val(vm, a, R[rb], &x64));
                if (term_is_int(R[rb])) { R[ra] = R[rb]; break; }
                ok = make_int(vm, a, wrap((uint32_t)(uint64_t)x64), &t);
            } else {
                CHECK(int_val(vm, a, R[rb], &x));
                if (term_is_int(R[rb])) { R[ra] = R[rb]; break; }
                ok = make_int64(vm, a, x, &t);
            }
            if (!ok) CRASH(R_OUT_OF_MEMORY);
            RELOAD();
            R[ra] = t;
            break;
        }

        case OP_EQ: case OP_NE: {
            REG(ra); REG(rb); REG(rc);
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
            REG(ra); REG(rb); REG(rc);
            int32_t x, y;
            CHECK(int_val(vm, a, R[rb], &x) && int_val(vm, a, R[rc], &y));
            R[ra] = term_from_bool(op == OP_LT ? x < y : x <= y);
            break;
        }

        case OP_CLEAR:
            REGS(ra, rb);
            for (uint32_t i = 0; i < rb; i++) R[ra + i] = T_NIL;
            break;

        case OP_NOT:
            REG(ra); REG(rb);
            CHECK(R[rb] == T_TRUE || R[rb] == T_FALSE);
            R[ra] = term_from_bool(R[rb] == T_FALSE);
            break;

        case OP_JMP: case OP_JMPF: case OP_JMPT: {
            bool take = true;
            if (op != OP_JMP) {
                REG(ra);
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
            REG(ra); REGS(rb, rc);
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
            REG(rb); REGS(rb + 1u, rc);
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
            REG(ra);
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
            REGS(ra, n);
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
            next = (uint32_t)ret + (op_has_ext(OP_OP(code[ret])) ? 2u : 1u);
            break;
        }

        case OP_NEWSTRUCT: case OP_MKCLOSURE: {
            REG(ra); REGS(rb, rc);
            if (op == OP_NEWSTRUCT) {
                /* image_type_row, unrolled: a reserved row, or an own one */
                uint32_t own = ext - TYPE_OWN_BASE, row = ext < img->n_reserved ? ext : own + img->n_reserved;
                CHECK((ext < img->n_reserved || own < img->n_types - img->n_reserved) && img->types[row * 2u] == rc);
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
            obj[1] = (op == OP_MKCLOSURE || ext >= TYPE_OWN_BASE) ? ext | tag : ext;
            for (uint32_t i = 0; i < rc; i++) obj[2 + i] = R[rb + i];
            R[ra] = term_from_ref(off);
            break;
        }

        case OP_GETFIELD: {
            REG(ra); REG(rb);
            const uint32_t *obj = obj_of(vm, a, R[rb]);
            /* Past the end only of a reserved struct from an older image:
             * the fields appended since are ints, 0 the default (opcodes.h). */
            CHECK(obj && hdr_kind(obj[0]) == K_STRUCT && (2u + rc < hdr_words(obj[0]) || obj[1] < TYPE_OWN_BASE) &&
                  type_live(vm, tag, struct_type(R[rb], obj)));
            R[ra] = 2u + rc < hdr_words(obj[0]) ? field_term(R[rb], obj[2 + rc]) : term_from_int(0);
            break;
        }

        case OP_TYPEIS: {
            REG(ra); REG(rb);
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
            }
            /* ext names a type of this module's image: its own struct
             * types are tagged like its structs. */
            CHECK(!(ext & MOD_TAG_MASK));
            R[ra] = term_from_bool(ty == (ext >= TYPE_OWN_BASE && ext < TY_INT ? ext | tag : ext));
            break;
        }

        case OP_BUILTIN:
            REG(ra); REGS(rb, rc);
            switch (ext) {
            case B_PRINTLN:
                /* Anything printed that names a gone module is badcode. */
                for (uint32_t i = 0; i < rc; i++) CHECK(deep_live(vm, a, R[rb + i], 0, WALK_BUDGET) != WALK_STALE);
                do_println(vm, a, R + rb, rc);
                R[ra] = T_NIL;
                break;
            case B_SPAWN: case B_SPAWN_LINK: case B_SPAWN_OPT: {
                CHECK(rc == (ext == B_SPAWN_OPT ? 2u : 1u));
                uint16_t max_words = 0;
                if (ext == B_SPAWN_OPT) CHECK(vm_spawn_opt_words(vm, a, R[rb + 1], &max_words));
                term pid;
                uint32_t err = do_spawn(vm, a, R[rb], max_words, ext == B_SPAWN_LINK, &pid);
                if (err != R_NORMAL) CRASH(err);
                R[ra] = pid;
                break;
            }
            case B_SELF:
                CHECK(rc == 0);
                R[ra] = a->id;
                break;
            case B_SEND:
                CHECK(rc == 2);
                if (port_is(R[rb])) {
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
                CHECK(rc == 0);
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
                CHECK(rc == 1);
                REGS(ra, 2);
                int32_t ms;
                CHECK(int_val(vm, a, R[rb], &ms));
                term t;
                if (msg_receive(vm, a, &t)) {
                    timer_cancel_wakeup(vm, a);
                    RELOAD();
                    R[ra] = t;
                    R[ra + 1] = T_TRUE;
                    break;
                }
                if (a->mbox_head) CRASH(R_OUT_OF_MEMORY);   /* message does not fit */
                switch (timed_wait(vm, a, ms)) {
                case WAIT_OOM: CRASH(R_OUT_OF_MEMORY);
                case WAIT_BLOCK: a->pc = code + pc; return RUN_WAIT;   /* retry when woken */
                default: break;                                        /* timed out, or ms <= 0 */
                }
                R[ra] = T_NIL;
                R[ra + 1] = T_FALSE;
                break;
            }
            case B_SLEEP: {
                CHECK(rc == 1);
                int32_t ms;
                CHECK(int_val(vm, a, R[rb], &ms));
                switch (timed_wait(vm, a, ms)) {
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
                    const Actor *to = actor_lookup(vm, pid);
                    if (!to || to->state == A_EXITED || (to->flags & AF_EXIT_PENDING)) {
                        R[ra] = T_NIL;
                        R[ra + 1] = T_FALSE;
                        break;
                    }
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
                R[ra] = got ? t : T_NIL;
                R[ra + 1] = term_from_bool(got != 0);
                break;
            }
            case B_REPLY: {
                CHECK(rc == 2);
                const uint32_t *req = obj_of(vm, a, R[rb]);
                CHECK(req && hdr_kind(req[0]) == K_STRUCT && req[1] == TYPE_REQUEST &&
                      hdr_words(req[0]) == 5u);
                /* A dead sender, or a Request not made by Call, drops it. */
                if ((r = msg_send_reply(vm, a, req[2], req[3], R[rb + 1])) < 0) CRASH((uint32_t)-r);
                R[ra] = T_NIL;
                break;
            }
            case B_SEND_AFTER: {
                CHECK(rc == 3);   /* like Send, a non-PID target just drops it */
                int32_t ms;
                CHECK(int_val(vm, a, R[rb], &ms));
                uint64_t deadline = vm_now(vm) + (ms > 0 ? (uint32_t)ms : 0u);
                uint32_t why;
                term ref = timer_add_send(vm, a, deadline, R[rb + 1], R[rb + 2], &why);
                if (ref == T_NIL) CRASH(why);
                RELOAD();
                R[ra] = ref;
                break;
            }
            case B_CANCEL_TIMER: {
                uint64_t serial;
                CHECK(rc == 1 && ref_serial(vm, a, R[rb], &serial));
                R[ra] = term_from_bool(serial != 0 && timer_cancel(vm, serial));   /* zero Ref: false */
                break;
            }
            case B_NOW: {
                CHECK(rc == 0);
                /* An immediate (no allocation) for the first ~12 days. */
                term t;
                if (!make_int64(vm, a, (int64_t)vm_now(vm), &t)) CRASH(R_OUT_OF_MEMORY);
                RELOAD();
                R[ra] = t;
                break;
            }
            case B_MEMORY: {
                CHECK(rc == 1);
                /* The caller's own state (mem, mem_words) is current in a. */
                const Actor *p = actor_lookup(vm, R[rb]);
                size_t bytes = p ? actor_memory_bytes(p) : 0u;
                term t;
                if (!make_int(vm, a, (int32_t)bytes, &t)) CRASH(R_OUT_OF_MEMORY);
                RELOAD();
                R[ra] = t;
                break;
            }
            case B_MAKE_REF: {
                CHECK(rc == 0);
                term ref = heap_new_ref(vm, a);
                if (ref == T_NIL) CRASH(R_OUT_OF_MEMORY);
                RELOAD();
                R[ra] = ref;
                break;
            }
            case B_MONITOR: {
                CHECK(rc == 1 && term_is_pid(R[rb]));
                term ref = sig_monitor(vm, a, R[rb]);
                if (ref == T_NIL) CRASH(R_OUT_OF_MEMORY);
                RELOAD();
                R[ra] = ref;
                break;
            }
            case B_DEMONITOR: {
                uint64_t serial;
                CHECK(rc == 1 && ref_serial(vm, a, R[rb], &serial));
                if (serial != 0) sig_demonitor(vm, a, serial);   /* zero Ref: nothing */
                R[ra] = T_NIL;
                break;
            }
            case B_LINK:
                CHECK(rc == 1 && term_is_pid(R[rb]));
                if (!sig_link(vm, a, R[rb])) CRASH(R_OUT_OF_MEMORY);
                R[ra] = T_NIL;
                break;
            case B_UNLINK:
                CHECK(rc == 1 && term_is_pid(R[rb]));
                sig_unlink(vm, a, R[rb]);
                R[ra] = T_NIL;
                break;
            case B_TRAP_EXIT:
                CHECK(rc == 1 && (R[rb] == T_TRUE || R[rb] == T_FALSE));
                sig_set_trap(a, R[rb] == T_TRUE);
                R[ra] = T_NIL;
                break;
            case B_KILL: {
                CHECK(rc == 2 && term_is_pid(R[rb]));
                int32_t c;
                CHECK(int_val(vm, a, R[rb + 1], &c));
                if (port_is(R[rb])) port_close(vm, R[rb]);   /* any caller, any code */
                else if (!sig_kill(vm, a, R[rb], (uint32_t)c)) CRASH(R_OUT_OF_MEMORY);
                R[ra] = T_NIL;
                break;
            }
            case B_EXIT: {
                CHECK(rc == 1);
                int32_t c;
                CHECK(int_val(vm, a, R[rb], &c));
                actor_exit(vm, a, (uint32_t)c, T_EMPTYSTR);
                return RUN_EXIT;
            }
            case B_REGISTER: {
                CHECK(rc == 2 && term_is_pid(R[rb + 1]));
                const uint8_t *name;
                uint32_t len;
                CHECK(name_bytes(vm, a, R[rb], &name, &len));
                r = names_register(vm, a->id, name, len, R[rb + 1]);
                if (r < 0) CRASH(R_OUT_OF_MEMORY);
                R[ra] = term_from_bool(r > 0);
                break;
            }
            case B_WHEREIS: {
                CHECK(rc == 1);
                REGS(ra, 2);
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
                CHECK(rc == 1);
                const uint8_t *name;
                uint32_t len;
                CHECK(name_bytes(vm, a, R[rb], &name, &len));
                names_unregister(vm, a->id, name, len);
                R[ra] = T_NIL;
                break;
            }
            case B_PORT_OPEN: {
                CHECK(rc == 2);
                REGS(ra, 2);
                CHECK(deep_live(vm, a, R[rb], 0, WALK_BUDGET) >= 0 &&
                      deep_live(vm, a, R[rb + 1], 0, WALK_BUDGET) >= 0);
                term pid = port_open(vm, a, R[rb], R[rb + 1]);   /* no GC */
                R[ra] = pid != T_NIL ? pid : T_ZEROPID;   /* failed: the zero PID */
                R[ra + 1] = term_from_bool(pid != T_NIL);
                break;
            }
            case B_PORT_CLOSE:
                CHECK(rc == 1);
                port_close(vm, R[rb]);    /* anything but an open port's PID
                                           * (e.g. from a failed Open): nothing */
                R[ra] = T_NIL;
                break;
            case B_PORT_HANDOFF:
                CHECK(rc == 2);
                R[ra] = term_from_bool(port_handoff(vm, a, R[rb], R[rb + 1]));   /* no GC */
                break;
            case B_NET_SUBSCRIBE:
                CHECK(rc == 0);
                R[ra] = term_from_bool(port_subscribe(vm, a));   /* no GC */
                break;
            case B_STR_CONCAT: case B_LEN: case B_INDEX: case B_SETINDEX: case B_SLICE:
            case B_MAKE_SLICE: case B_APPEND: case B_COPY: case B_CAP: case B_STR_FROM:
            case B_BYTESLICE_FROM: case B_STR_CMP: case B_BUF_NEW: case B_BUF_WRITE:
            case B_BUF_FREEZE: case B_BYTES_FROM: case B_BYTES_EQUAL: case B_BYTES_COPYTO: {
                uint32_t err = strs_builtin(vm, a, ext, ra, rb, rc);   /* may GC */
                RELOAD();
                if (err != R_NORMAL) CRASH(err);
                break;
            }
            case B_CONFIG_GET: {
                REGS(ra, 2);   /* two results: value, ok */
                uint32_t err = strs_builtin(vm, a, ext, ra, rb, rc);   /* may GC */
                RELOAD();
                if (err != R_NORMAL) CRASH(err);
                break;
            }
            case B_UNIX_TIME: {
                CHECK(rc == 0);
                term t;
                if (!make_int64(vm, a, vm_unix_time_ms(vm), &t)) CRASH(R_OUT_OF_MEMORY);
                RELOAD();
                R[ra] = t;
                break;
            }
            case B_SET_UNIX_TIME: {
                int64_t ms;
                CHECK(rc == 1 && int64_val(vm, a, R[rb], &ms));
                R[ra] = term_from_bool(vm_set_unix_time_ms(vm, ms) != 0);
                break;
            }
            case B_RANDOM:
                CHECK(rc == 0);
                R[ra] = term_from_int((int32_t)(vm_random(vm) & 0x3FFFFFFFu));
                break;
            case B_FS: case B_CODE: {
                /* Both check ra+1 themselves (two-result ops only). */
                uint32_t err = ext == B_CODE ? module_builtin(vm, a, ra, rb, rc, nregs)
                             : vm->fs_builtin ? vm->fs_builtin(vm, a, ra, rb, rc, nregs) : R_BAD_CODE;
                RELOAD();      /* may GC */
                if (err != R_NORMAL) CRASH(err);
                break;
            }
            default:
                /* Unknown builtin id. */
                CRASH(R_BAD_CODE);
            }
            /* A builtin may have marked the running actor itself (a send
             * to itself over its quota, Kill of itself, Link of a dead pid):
             * exit now, not when next scheduled. */
            if (actor_exit_pending(a, &reason, &reason_func)) {
                actor_exit(vm, a, reason, reason_func);
                return RUN_EXIT;
            }
            break;

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
#undef REG
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
    if (err != CODE_OK) {
        vm_reserve_release(vm);
        if (err == CODE_NOT_FOUND) write_err_lit("gogo: image has no main\n");
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
