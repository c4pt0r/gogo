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
 * Safety on malformed images: register indices are checked against the
 * current function's n_regs, the pc against the code area, LOADK operands
 * against the term kinds an image may contain, and RET checks that the
 * frame chain is consistent. Violations crash the actor with `badcode`. */
#include "interp.h"
#include "heap.h"
#include "msg.h"
#include "opcodes.h"
#include "platform.h"
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

/* Pointer to the object a heap or const ref names, or NULL if t is neither
 * or points outside the heap / const area. */
static const uint32_t *obj_of(const Vm *vm, const Actor *a, term t) {
    const uint32_t *base;
    uint32_t off, limit;
    if (term_is_ref(t)) {
        base = a->mem;
        off = term_ref_off(t);
        limit = a->hp;
    } else if (term_is_const(t)) {
        base = vm->img->consts;
        off = term_const_off(t);
        limit = vm->img->consts_words;
    } else {
        return NULL;
    }
    /* Every object has at least two words and lies wholly inside its area. */
    if (off >= limit || limit - off < 2u) return NULL;
    uint32_t words = hdr_words(base[off]);
    if (words < 2u || words > limit - off) return NULL;
    return base + off;
}

/* ---- ints ---- */

/* Value of a small int or boxed K_INT; false for anything else. */
static bool int_val(const Vm *vm, const Actor *a, term t, int32_t *out) {
    if (term_is_int(t)) {
        *out = term_to_int(t);
        return true;
    }
    const uint32_t *o = obj_of(vm, a, t);
    if (o && hdr_kind(o[0]) == K_INT) {
        *out = (int32_t)o[1];
        return true;
    }
    return false;
}

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

/* Wrapping int32 arithmetic without signed-overflow UB. The conversion of
 * an out-of-range uint32 back to int32 is implementation-defined in C99;
 * every compiler we target defines it as two's complement. */
static int32_t wrap(uint32_t u) { return (int32_t)u; }

/* ---- println ---- */

typedef struct Out {
    char buf[256];
    size_t n;
    void (*write)(const char *s, size_t n);   /* platform_write or _err */
} Out;

static void out_flush(Out *o) {
    if (o->n) o->write(o->buf, o->n);
    o->n = 0;
}

static void out_bytes(Out *o, const char *s, size_t n) {
    if (n > sizeof o->buf - o->n) {
        out_flush(o);
        if (n > sizeof o->buf) {           /* too big to buffer: write through */
            o->write(s, n);
            return;
        }
    }
    memcpy(o->buf + o->n, s, n);
    o->n += n;
}

static void out_str(Out *o, const char *s) { out_bytes(o, s, strlen(s)); }

static void out_int(Out *o, int32_t v) {
    char tmp[12];
    int i = (int)sizeof tmp;
    uint32_t u = v < 0 ? 0u - (uint32_t)v : (uint32_t)v;
    do {
        tmp[--i] = (char)('0' + u % 10u);
        u /= 10u;
    } while (u);
    if (v < 0) tmp[--i] = '-';
    out_bytes(o, tmp + i, sizeof tmp - (size_t)i);
}

static void out_term(Out *o, const Vm *vm, const Actor *a, term t, int depth) {
    int32_t v;
    if (int_val(vm, a, t, &v)) { out_int(o, v); return; }
    if (t == T_TRUE) { out_str(o, "true"); return; }
    if (t == T_FALSE) { out_str(o, "false"); return; }
    if (t == T_NIL) { out_str(o, "nil"); return; }
    if (term_is_pid(t)) {
        out_str(o, "<pid ");
        out_int(o, term_pid_slot(t));
        out_str(o, ">");
        return;
    }
    const uint32_t *obj = obj_of(vm, a, t);
    uint32_t kind = obj ? hdr_kind(obj[0]) : 0;
    if (kind == K_STRING && obj[1] <= (hdr_words(obj[0]) - 2u) * 4u) {
        out_bytes(o, (const char *)(obj + 2), obj[1]);
    } else if (kind == K_STRUCT) {
        if (depth >= PRINT_MAX_DEPTH) { out_str(o, "{...}"); return; }
        out_str(o, "{");
        for (uint32_t i = 2; i < hdr_words(obj[0]); i++) {
            if (i > 2) out_str(o, " ");
            out_term(o, vm, a, obj[i], depth + 1);
        }
        out_str(o, "}");
    } else if (kind == K_CLOSURE) {
        out_str(o, "<func>");
    } else {
        out_str(o, "?");
    }
}

static void do_println(const Vm *vm, const Actor *a, const term *args, uint32_t n) {
    Out o;
    o.n = 0;
    o.write = platform_write;
    for (uint32_t i = 0; i < n; i++) {
        if (i) out_str(&o, " ");
        out_term(&o, vm, a, args[i], 0);
    }
    out_str(&o, "\n");
    out_flush(&o);
}

/* ---- crash ---- */

static void report_crash(const Vm *vm, const Actor *a, uint32_t fn, const char *kind) {
    Out o;
    o.n = 0;
    o.write = platform_write_err;
    uint32_t len;
    const char *name = image_func_name(vm->img, fn, &len);
    out_str(&o, "gogo: actor <pid ");
    out_int(&o, term_pid_slot(a->id));
    out_str(&o, "> crashed: ");
    out_str(&o, kind);
    out_str(&o, " in ");
    out_bytes(&o, name, len);
    out_str(&o, "\n");
    out_flush(&o);
}

/* ---- actors ---- */

/* Spawns an actor running closure clo: its captures are copied into the new
 * actor like a message and become r0.. of the entry frame. On success sets
 * *pid and returns NULL; otherwise returns the crash kind for the caller.
 * Allocates only in the new actor, so the caller's heap does not move. */
static const char *do_spawn(Vm *vm, const Actor *a, term clo, term *pid) {
    const Image *img = vm->img;
    const uint32_t *obj = obj_of(vm, a, clo);
    if (!obj || !term_is_ref(clo) || hdr_kind(obj[0]) != K_CLOSURE) return "badcode";
    uint32_t f = obj[1];
    uint32_t ncaps = hdr_words(obj[0]) - 2u;
    if (f >= img->n_funcs || image_func_nparams(img, f) != ncaps) return "badcode";
    uint32_t nregs = image_func_nregs(img, f);
    if (ncaps > nregs) return "badcode";

    Msg *m = msg_build(a, clo);
    if (!m) return "oom";
    uint32_t words = nregs + 2u + m->words + 8u;
    Actor *n = words <= HEAP_MAX_WORDS ? actor_new(vm, (uint16_t)words) : NULL;
    /* The block is sized for the frame and the copy, so neither GCs; the
     * frame goes first anyway, so a GC could not drop the copied closure. */
    term copy;
    if (!n || !heap_push_frame(vm, n, (uint16_t)nregs, -1) || !msg_unpack(vm, n, m, &copy)) {
        msg_free(m);
        if (n) actor_free(vm, n);
        return "oom";
    }
    msg_free(m);
    const uint32_t *caps = n->mem + term_ref_off(copy) + 2u;
    for (uint32_t i = 0; i < ncaps; i++) n->mem[n->fp + i] = caps[i];
    n->pc = img->code + image_func_code_off(img, f);
    runq_push(vm, n);
    *pid = n->id;
    return NULL;
}

/* ---- interpreter ---- */

int interp_run(Vm *vm, Actor *a) {
    const Image *img = vm->img;
    const uint32_t *code = img->code;
    uint32_t pc = (uint32_t)(a->pc - code);
    uint32_t fn = func_of_pc(img, pc);
    uint32_t nregs = image_func_nregs(img, fn);
    uint32_t budget = REDUCTIONS;
    const char *kind = "badcode";
    term *R;

#define RELOAD() (R = (term *)a->mem + a->fp)
#define CRASH(k) do { kind = (k); goto crash; } while (0)
#define CHECK(cond) do { if (!(cond)) CRASH("badcode"); } while (0)
#define REG(r) do { if ((uint32_t)(r) >= nregs) CRASH("badcode"); } while (0)
/* Registers r..r+n-1 all exist (n may be 0). */
#define REGS(r, n) do { if ((uint32_t)(r) + (uint32_t)(n) > nregs) CRASH("badcode"); } while (0)

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
            /* An image may hold small ints, specials and in-range const
             * refs; never heap refs (they would point into a random heap). */
            CHECK(term_is_int(ext) || ext == T_NIL || ext == T_TRUE || ext == T_FALSE ||
                  (term_is_const(ext) && obj_of(vm, a, ext) != NULL));
            R[ra] = ext;
            break;

        case OP_LOADI32: {
            REG(ra);
            term t;
            if (!make_int(vm, a, (int32_t)ext, &t)) CRASH("oom");
            RELOAD();
            R[ra] = t;
            break;
        }

        case OP_ADD: case OP_SUB: case OP_MUL: case OP_DIV: case OP_REM: {
            REG(ra); REG(rb); REG(rc);
            int32_t x, y, r;
            CHECK(int_val(vm, a, R[rb], &x) && int_val(vm, a, R[rc], &y));
            switch (op) {
            case OP_ADD: r = wrap((uint32_t)x + (uint32_t)y); break;
            case OP_SUB: r = wrap((uint32_t)x - (uint32_t)y); break;
            case OP_MUL: r = wrap((uint32_t)x * (uint32_t)y); break;
            case OP_DIV:
                if (y == 0) CRASH("divzero");
                r = (x == INT32_MIN && y == -1) ? INT32_MIN : x / y;  /* Go: wraps */
                break;
            default: /* OP_REM */
                if (y == 0) CRASH("divzero");
                r = (y == -1) ? 0 : x % y;     /* avoids INT32_MIN % -1 UB */
                break;
            }
            term t;
            if (!make_int(vm, a, r, &t)) CRASH("oom");
            RELOAD();
            R[ra] = t;
            break;
        }

        case OP_NEG: {
            REG(ra); REG(rb);
            int32_t x;
            CHECK(int_val(vm, a, R[rb], &x));
            term t;
            if (!make_int(vm, a, wrap(0u - (uint32_t)x), &t)) CRASH("oom");
            RELOAD();
            R[ra] = t;
            break;
        }

        case OP_EQ: case OP_NE: {
            REG(ra); REG(rb); REG(rc);
            int32_t x, y;
            bool eq;
            if (int_val(vm, a, R[rb], &x) && int_val(vm, a, R[rc], &y)) eq = x == y;
            else eq = R[rb] == R[rc];
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
            if (!heap_push_frame(vm, a, (uint16_t)callee_regs, (int32_t)pc)) CRASH("oom");
            const term *args = a->mem + (a->mem_words - caller_top) + rb;
            for (uint32_t i = 0; i < rc; i++) a->mem[a->fp + i] = args[i];
            fn = ext;
            nregs = callee_regs;
            next = image_func_code_off(img, fn);
            RELOAD();
            budget--;
            break;
        }

        case OP_RET: {
            REG(ra);
            term v = R[ra];
            uint32_t fp = a->fp;
            CHECK(term_is_int(R[nregs]) && term_is_int(R[nregs + 1]));
            int32_t saved = term_to_int(R[nregs]);
            int32_t ret = term_to_int(R[nregs + 1]);
            a->sp = (uint16_t)(fp + nregs + 2u);      /* pop the frame */
            if (ret == -1) {                           /* entry frame */
                a->pc = code + pc;
                return RUN_EXIT;
            }
            /* The caller's frame sits directly above ours. */
            CHECK(saved > 0 && (uint32_t)saved <= a->mem_words &&
                  a->mem_words - (uint32_t)saved == a->sp);
            CHECK(ret >= 0 && (uint32_t)ret + 1u < img->code_words &&
                  OP_OP(code[ret]) == OP_CALL);
            uint32_t caller = func_of_pc(img, (uint32_t)ret);
            uint32_t caller_regs = image_func_nregs(img, caller);
            uint32_t caller_fp = a->sp;
            CHECK(caller_fp + caller_regs + 2u <= a->mem_words);
            uint32_t dst = OP_A(code[ret]);
            CHECK(dst < caller_regs);
            a->fp = (uint16_t)caller_fp;
            fn = caller;
            nregs = caller_regs;
            RELOAD();
            R[dst] = v;
            next = (uint32_t)ret + 2u;
            break;
        }

        case OP_NEWSTRUCT: case OP_MKCLOSURE: {
            REG(ra); REGS(rb, rc);
            if (op == OP_NEWSTRUCT) CHECK(ext < img->n_types && image_type_nfields(img, ext) == rc);
            else CHECK(ext < img->n_funcs);
            uint32_t words = 2u + rc;
            uint32_t off = heap_alloc(vm, a, words, NULL, 0);
            if (off == UINT32_MAX) CRASH("oom");
            RELOAD();
            uint32_t *obj = a->mem + off;
            obj[0] = hdr_make(words, op == OP_NEWSTRUCT ? K_STRUCT : K_CLOSURE);
            obj[1] = ext;
            for (uint32_t i = 0; i < rc; i++) obj[2 + i] = R[rb + i];
            R[ra] = term_from_ref(off);
            break;
        }

        case OP_GETFIELD: {
            REG(ra); REG(rb);
            const uint32_t *obj = obj_of(vm, a, R[rb]);
            CHECK(obj && hdr_kind(obj[0]) == K_STRUCT && 2u + rc < hdr_words(obj[0]));
            R[ra] = obj[2 + rc];
            break;
        }

        case OP_TYPEIS: {
            REG(ra); REG(rb);
            term t = R[rb];
            uint32_t ty = UINT32_MAX;
            int32_t dummy;
            if (int_val(vm, a, t, &dummy)) ty = TY_INT;
            else if (t == T_TRUE || t == T_FALSE) ty = TY_BOOL;
            else if (term_is_pid(t)) ty = TY_PID;
            else {
                const uint32_t *obj = obj_of(vm, a, t);
                if (obj && hdr_kind(obj[0]) == K_STRING) ty = TY_STRING;
                else if (obj && hdr_kind(obj[0]) == K_STRUCT) ty = obj[1];
            }
            R[ra] = term_from_bool(ty == ext);
            break;
        }

        case OP_BUILTIN:
            REG(ra); REGS(rb, rc);
            switch (ext) {
            case B_PRINTLN:
                do_println(vm, a, R + rb, rc);
                R[ra] = T_NIL;
                break;
            case B_SPAWN: {
                CHECK(rc == 1);
                term pid;
                const char *err = do_spawn(vm, a, R[rb], &pid);
                if (err) CRASH(err);
                R[ra] = pid;
                break;
            }
            case B_SELF:
                CHECK(rc == 0);
                R[ra] = a->id;
                break;
            case B_SEND:
                CHECK(rc == 2);
                /* 1 (receiver dead or stale) is not an error. */
                {
                    int sr = msg_send(vm, a, R[rb], R[rb + 1]);
                    if (sr == -2) CRASH("mailbox_full");
                    if (sr < 0) CRASH("oom");
                }
                R[ra] = T_NIL;
                break;
            case B_RECEIVE: {
                CHECK(rc == 0);
                term t;
                if (!msg_receive(vm, a, &t)) {
                    if (a->mbox_head) CRASH("oom");   /* message does not fit */
                    a->pc = code + pc;    /* retry this BUILTIN when woken */
                    return RUN_WAIT;
                }
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
                if (!make_int(vm, a, (int32_t)bytes, &t)) CRASH("oom");
                RELOAD();
                R[ra] = t;
                break;
            }
            default:
                CRASH("badbuiltin");
            }
            break;

        default:
            CRASH("badcode");
        }

        pc = next;
        if (budget == 0) {
            a->pc = code + pc;
            a->reductions = 0;
            return RUN_YIELD;
        }
    }

crash:
    report_crash(vm, a, fn, kind);
    a->pc = code + pc;
    return RUN_EXIT;

#undef RELOAD
#undef CRASH
#undef CHECK
#undef REG
#undef REGS
}

/* ---- scheduler ---- */

#define write_err_lit(s) platform_write_err((s), sizeof(s) - 1u)

int vm_run(Vm *vm) {
    const Image *img = vm->img;
    uint32_t entry = img->entry;
    uint16_t nregs = image_func_nregs(img, entry);
    if (nregs > 65535u - 10u) {      /* initial block of nregs + 2 + 8 words */
        write_err_lit("gogo: entry function has too many registers\n");
        return 2;
    }
    Actor *a = actor_new(vm, (uint16_t)(nregs + 2u + 8u));
    if (!a) {
        write_err_lit("gogo: cannot start main actor\n");
        return 1;
    }
    if (!heap_push_frame(vm, a, nregs, -1)) {
        write_err_lit("gogo: cannot start main actor\n");
        actor_free(vm, a);
        return 1;
    }
    a->pc = img->code + image_func_code_off(img, entry);
    runq_push(vm, a);

    while ((a = runq_pop(vm)) != NULL) {
        switch (interp_run(vm, a)) {
        case RUN_YIELD:
            runq_push(vm, a);
            break;
        case RUN_WAIT:
            /* Not requeued: msg_send pushes it back exactly once. */
            a->state = A_WAITING;
            break;
        default:
            a->state = A_EXITED;
            actor_free(vm, a);
            break;
        }
    }
    return 0;
}
