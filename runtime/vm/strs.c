/* Sequence builtins: strings and slices.
 *
 * A string is the special T_EMPTYSTR (length 0, never
 * allocated), a const-area K_STRING or a heap K_STRING [hdr][byte_len]
 * [bytes], padded with zeros to a whole word. Strings are immutable, so a
 * result may share its argument (s + "" is s); a substring is a copy
 * (spec decision 1).
 *
 * A slice is T_NILSLICE (nil: length and capacity 0) or an immutable
 * K_SLICE header [hdr][off][len][cap][array] over a mutable backing array:
 * a K_ARRAY of terms or, for []byte, a K_BYTEARR of bytes (byte stores keep
 * v & 0xFF). The array is T_NIL when cap is 0. Several headers may share an
 * array (Go aliasing): s[i] = v writes into it, s[a:b] makes a new header
 * on it, and append writes within cap in place and otherwise copies into a
 * new array (cap x2, at least 4). Elements beyond len, up to cap, are
 * reachable by reslicing, so they always hold the element type's zero
 * value: MAKE_SLICE and APPEND take it as an argument.
 *
 * A bytes.Bytes is T_EMPTYBYTES, an immutable heap K_BYTES [hdr][nbytes]
 * [bytes] of any size, or an off-heap handle (bytes.h); every operation
 * reads it through bytes_view. A bytes.Buffer is a K_BUF [hdr][len][array]
 * whose array is a K_BYTEARR (its capacity; T_NIL while 0): a reference
 * type, mutated in place; the zero Buffer is T_NIL and every Buffer
 * builtin rejects it (badcode). Freeze copies the contents once into a
 * Bytes (heap up to GOGO_BYTES_INLINE bytes, else off-heap) and empties
 * the buffer, which keeps its array. Bytes.Slice shares an off-heap root
 * when the result is larger than GOGO_BYTES_INLINE and copies otherwise.
 *
 * B_LEN, B_INDEX and B_SLICE are generic over the sequence kinds: each
 * dispatches on the kind of its first argument.
 *
 * GC: the arguments stay in registers (stack roots) while a result is
 * allocated; pointers into the heap and views of the arguments are
 * re-derived after each allocation, never held across it. */
#include "strs.h"
#include "floats.h"
#include "heap.h"
#include "opcodes.h"
#include "bytes.h"
#include "config.h"
#include "host.h"
#include "out.h"
#include <string.h>

/* Number of heap words of a string with len bytes. */
static uint32_t str_words(uint32_t len) { return 2u + (len + 3u) / 4u; }

/* A string term's bytes; false if t is not a string. */
static bool str_view(const Vm *vm, const Actor *a, term t, const uint8_t **p, uint32_t *len) {
    return name_bytes(vm, a, t, p, len);
}

/* Reads the int argument t (small or boxed). */
static bool arg_int(const Vm *vm, const Actor *a, term t, int32_t *v) {
    return int_val(vm, a, t, v);
}

/* Allocates a heap K_STRING or K_BYTES (kind) of len bytes (len > 0) and
 * returns its offset in *off, or false if out of memory. The padding of
 * the last word is zeroed (the caller fills the bytes); may GC. */
static bool raw_alloc(Vm *vm, Actor *a, uint32_t kind, uint32_t len, uint32_t *off) {
    if (len > (HEAP_MAX_WORDS - 2u) * 4u) return false;
    uint32_t words = str_words(len);
    uint32_t o = heap_alloc(vm, a, words, NULL, 0);
    if (o == UINT32_MAX) return false;
    a->mem[o] = hdr_make(words, kind);
    a->mem[o + 1] = len;
    a->mem[o + words - 1u] = 0;   /* padding of the last word */
    *off = o;
    return true;
}

static bool str_alloc(Vm *vm, Actor *a, uint32_t len, uint32_t *off) {
    return raw_alloc(vm, a, K_STRING, len, off);
}

static uint32_t str_concat(Vm *vm, Actor *a, term *R, uint32_t ra, uint32_t rb) {
    const uint8_t *p, *q;
    uint32_t lp, lq;
    if (!str_view(vm, a, R[rb], &p, &lp) || !str_view(vm, a, R[rb + 1], &q, &lq)) return R_BAD_CODE;
    if (lq == 0) { R[ra] = R[rb]; return R_NORMAL; }
    if (lp == 0) { R[ra] = R[rb + 1]; return R_NORMAL; }
    uint32_t off;
    if (lp + lq < lp || !str_alloc(vm, a, lp + lq, &off)) return R_OUT_OF_MEMORY;
    R = (term *)a->mem + a->fp;   /* the allocation may have moved everything */
    str_view(vm, a, R[rb], &p, &lp);
    str_view(vm, a, R[rb + 1], &q, &lq);
    uint8_t *d = (uint8_t *)(a->mem + off + 2);
    memcpy(d, p, lp);
    memcpy(d + lp, q, lq);
    R[ra] = term_from_ref(off);
    return R_NORMAL;
}

/* ---- slices ---- */

/* A slice argument: arr is T_NIL when cap is 0 (always for nil). A
 * const array (15.6: a package-level variable's, in flash) is read-only:
 * a write through it is badcode, and its elements get its module's tag
 * when read (field_term). */
typedef struct SliceV {
    term arr;
    uint32_t off, len, cap;
    bool bytes;          /* arr is a K_BYTEARR (false when arr is T_NIL) */
    bool ro;             /* arr is in a const area */
    uint32_t *ao;        /* arr's object */
} SliceV;

/* The elements of the K_ARRAY / K_BYTEARR ref t: *elems is its element
 * capacity. NULL if t is neither. */
static uint32_t *arr_obj(const Vm *vm, const Actor *a, term t, bool *bytes, uint32_t *elems) {
    uint32_t *o = (uint32_t *)obj_of(vm, a, t);
    if (!o) return NULL;
    uint32_t words = hdr_words(o[0]);
    switch (hdr_kind(o[0])) {
    case K_ARRAY:
        if (o[1] != words - 2u) return NULL;
        *bytes = false;
        break;
    case K_BYTEARR:
        if (o[1] > (words - 2u) * 4u) return NULL;
        *bytes = true;
        break;
    default:
        return NULL;
    }
    *elems = o[1];
    return o;
}

/* Reads the slice t (nil or a well-formed K_SLICE, heap or const);
 * false otherwise. */
static bool slice_view(const Vm *vm, const Actor *a, term t, SliceV *s) {
    s->arr = T_NIL;
    s->off = s->len = s->cap = 0;
    s->bytes = s->ro = false;
    if (t == T_NILSLICE) return true;
    const uint32_t *o = obj_of(vm, a, t);
    if (!o || o[0] != hdr_make(5, K_SLICE)) return false;
    s->off = o[1];
    s->len = o[2];
    s->cap = o[3];
    s->arr = field_term(t, o[4]);
    if (s->len > s->cap) return false;
    if (s->arr == T_NIL) return s->cap == 0;
    uint32_t elems;
    s->ro = term_is_const(s->arr);
    if (!(s->ao = arr_obj(vm, a, s->arr, &s->bytes, &elems))) return false;
    return s->off <= elems && s->cap <= elems - s->off;
}

/* Pointer to element i (< cap) of a term slice, or byte i of a byte slice. */
static term *slice_terms(Actor *a, const SliceV *s) {
    (void)a;
    return (term *)s->ao + 2u + s->off;
}
static uint8_t *slice_bytes(Actor *a, const SliceV *s) {
    (void)a;
    return (uint8_t *)(s->ao + 2u) + s->off;
}

/* Copies n terms of the term slice s, from element i, to dst (they may
 * overlap): a const array's terms get its module's tag. */
static void terms_copy(term *dst, const SliceV *s, uint32_t i, uint32_t n) {
    const term *src = slice_terms(NULL, s) + i;
    if (!s->ro) memmove(dst, src, (size_t)n * 4u);
    else for (uint32_t k = 0; k < n; k++) dst[k] = field_term(s->arr, src[k]);
}

/* The bytes of t, when it is one of the kinds in mask: a string, a Bytes,
 * a []byte. */
#define SQ_STR    1u
#define SQ_BYTES  2u
#define SQ_BSLICE 4u
static bool seq_view(const Vm *vm, const Actor *a, term t, uint32_t mask, const uint8_t **p, uint32_t *len) {
    SliceV s;
    if ((mask & SQ_STR) && str_view(vm, a, t, p, len)) return true;
    if ((mask & SQ_BYTES) && bytes_view(vm, a, t, p, len)) return true;
    if (!(mask & SQ_BSLICE) || !slice_view(vm, a, t, &s) || (s.arr != T_NIL && !s.bytes)) return false;
    *p = s.len ? slice_bytes((Actor *)a, &s) : NULL;
    *len = s.len;
    return true;
}

/* A string or a Bytes: its bytes. */
#define seq_bytes(vm, a, t, p, len) seq_view(vm, a, t, SQ_STR | SQ_BYTES, p, len)

/* R[ra] = a new heap K_STRING / K_BYTES (kind) holding n bytes, from byte
 * lo on, of the register term R[r], which seq_view(mask) reads (again
 * after the allocation: it may GC); the empty one (T_EMPTYSTR,
 * T_EMPTYBYTES) for n == 0. */
static uint32_t seq_copy(Vm *vm, Actor *a, uint32_t ra, uint32_t r, uint32_t kind, uint32_t mask, uint32_t lo, uint32_t n) {
    uint32_t off;
    term out = kind == K_STRING ? T_EMPTYSTR : T_EMPTYBYTES;
    if (n) {
        if (!raw_alloc(vm, a, kind, n, &off)) return R_OUT_OF_MEMORY;
        const uint8_t *p;
        uint32_t len;
        seq_view(vm, a, ((term *)a->mem + a->fp)[r], mask, &p, &len);
        memcpy(a->mem + off + 2, p + lo, n);
        out = term_from_ref(off);
    }
    ((term *)a->mem + a->fp)[ra] = out;
    return R_NORMAL;
}

/* Words of an array of cap elements. */
static uint32_t arr_words(bool bytes, uint32_t cap) {
    return bytes ? 2u + (cap + 3u) / 4u : 2u + cap;
}

/* The largest capacity an array may have (its words fit a heap). */
static uint32_t arr_max(bool bytes) {
    return bytes ? (HEAP_MAX_WORDS - 2u) * 4u : HEAP_MAX_WORDS - 2u;
}

/* Allocates an array of cap (> 0) elements, all T_NIL or 0 (valid for
 * the GC until the caller fills them), into *arr; roots as for
 * heap_alloc. False if out of memory. May GC. */
static bool arr_alloc(Vm *vm, Actor *a, bool bytes, uint32_t cap, term *roots, int n_roots, term *arr) {
    if (cap > arr_max(bytes)) return false;
    uint32_t words = arr_words(bytes, cap);
    uint32_t o = heap_alloc(vm, a, words, roots, n_roots);
    if (o == UINT32_MAX) return false;
    a->mem[o] = hdr_make(words, bytes ? K_BYTEARR : K_ARRAY);
    a->mem[o + 1] = cap;
    if (bytes) memset(a->mem + o + 2, 0, (size_t)(words - 2u) * 4u);
    else for (uint32_t i = 0; i < cap; i++) a->mem[o + 2 + i] = T_NIL;
    *arr = term_from_ref(o);
    return true;
}

/* Allocates a header for (*arr, off, len, cap) and returns it; *arr is a
 * root (updated if the GC moves it). T_NIL if out of memory. May GC. */
static term hdr_alloc(Vm *vm, Actor *a, term *arr, uint32_t off, uint32_t len, uint32_t cap) {
    uint32_t o = heap_alloc(vm, a, 5, arr, 1);
    if (o == UINT32_MAX) return T_NIL;
    a->mem[o] = hdr_make(5, K_SLICE);
    a->mem[o + 1] = off;
    a->mem[o + 2] = len;
    a->mem[o + 3] = cap;
    a->mem[o + 4] = cap ? *arr : T_NIL;
    return term_from_ref(o);
}

/* Fills elements [from, to) of the array arr with zero (a term), or with
 * 0 for a byte array. */
static void arr_fill(Actor *a, term arr, bool bytes, uint32_t from, uint32_t to, term zero) {
    uint32_t *o = a->mem + term_ref_off(arr);
    if (bytes) memset((uint8_t *)(o + 2) + from, 0, to - from);
    else for (uint32_t i = from; i < to; i++) o[2 + i] = zero;
}

/* MAKE_SLICE kind, len, cap, zero. */
static uint32_t sl_make(Vm *vm, Actor *a, term *R, uint32_t ra, uint32_t rb) {
    int32_t kind, len, cap;
    if (!arg_int(vm, a, R[rb], &kind) || !arg_int(vm, a, R[rb + 1], &len) ||
        !arg_int(vm, a, R[rb + 2], &cap)) return R_BAD_CODE;
    if ((kind != 0 && kind != 1) || len < 0 || cap < len) return R_BAD_CODE;
    bool bytes = kind == 1;
    term arr = T_NIL;
    if (cap > 0 && !arr_alloc(vm, a, bytes, (uint32_t)cap, NULL, 0, &arr)) return R_OUT_OF_MEMORY;
    term h = hdr_alloc(vm, a, &arr, 0, (uint32_t)len, (uint32_t)cap);
    if (h == T_NIL) return R_OUT_OF_MEMORY;
    R = (term *)a->mem + a->fp;
    if (cap > 0) arr_fill(a, arr, bytes, 0, (uint32_t)cap, R[rb + 3]);
    R[ra] = h;
    return R_NORMAL;
}

/* The elements APPEND adds: the values v[0..nv) (modes 0/1) or every
 * element of the spread argument v[0] (modes 2/3). */
typedef struct AppendSrc {
    bool bytes, spread;
    uint32_t n;
    SliceV sl;                  /* v[0] as a slice (spread) */
    const uint8_t *str;         /* v[0] as a string (byte spread), else NULL */
} AppendSrc;

static bool append_src(const Vm *vm, const Actor *a, const term *v, uint32_t nv, int32_t mode, AppendSrc *src) {
    src->bytes = (mode & 1) != 0;
    src->spread = (mode & 2) != 0;
    src->str = NULL;
    src->n = nv;
    if (!src->spread) {
        int32_t b;
        if (nv == 0) return false;
        for (uint32_t k = 0; src->bytes && k < nv; k++) {
            if (!arg_int(vm, a, v[k], &b)) return false;
        }
        return true;
    }
    if (nv != 1) return false;
    if (src->bytes && str_view(vm, a, v[0], &src->str, &src->n)) return true;
    src->str = NULL;
    if (!slice_view(vm, a, v[0], &src->sl)) return false;
    if (src->sl.arr != T_NIL && src->sl.bytes != src->bytes) return false;
    src->n = src->sl.len;
    return true;
}

/* APPEND s, mode, zero, v...: growth is decided once for all the values
 * (Go's semantics: if they do not all fit, none goes into the old array). */
static uint32_t sl_append(Vm *vm, Actor *a, term *R, uint32_t ra, uint32_t rb, uint32_t rc) {
    SliceV s;
    AppendSrc src;
    int32_t mode;
    uint32_t nv = rc - 3u;
    if (!slice_view(vm, a, R[rb], &s) || !arg_int(vm, a, R[rb + 1], &mode) ||
        mode < 0 || mode > 3 || !append_src(vm, a, R + rb + 3, nv, mode, &src)) return R_BAD_CODE;
    if (s.arr != T_NIL && s.bytes != src.bytes) return R_BAD_CODE;
    bool bytes = src.bytes;
    if (s.ro && src.n && s.len + src.n <= s.cap) return R_BAD_CODE;   /* in place into a const array */
    if (src.n == 0) { R[ra] = R[rb]; return R_NORMAL; }     /* append(s, empty...) is s */
    if (src.n > arr_max(bytes) - s.len) return R_OUT_OF_MEMORY;
    uint32_t need = s.len + src.n;

    /* Allocate first (a new array if they do not fit, then the header),
     * then write: a failure leaves every array as it was. */
    term arr = s.arr;
    uint32_t off = s.off, cap = s.cap;
    bool grow = need > s.cap;
    if (grow) {
        cap = s.cap > arr_max(bytes) / 2u ? arr_max(bytes) : s.cap * 2u;
        if (cap < 4u) cap = 4u;
        if (cap < need) cap = need;
        if (cap > arr_max(bytes)) cap = arr_max(bytes);
        if (!arr_alloc(vm, a, bytes, cap, NULL, 0, &arr)) return R_OUT_OF_MEMORY;
        off = 0;
    }
    term h = hdr_alloc(vm, a, &arr, off, need, cap);
    if (h == T_NIL) return R_OUT_OF_MEMORY;
    R = (term *)a->mem + a->fp;
    slice_view(vm, a, R[rb], &s);                            /* re-derive after GC */
    append_src(vm, a, R + rb + 3, nv, mode, &src);
    SliceV d = { arr, off, need, cap, bytes, false, a->mem + term_ref_off(arr) };
    if (grow) {
        arr_fill(a, arr, bytes, need, cap, R[rb + 2]);
        if (bytes && s.len) memcpy(slice_bytes(a, &d), slice_bytes(a, &s), s.len);
        else if (s.len) terms_copy(slice_terms(a, &d), &s, 0, s.len);
    }
    /* The new elements; a spread source may overlap the destination. */
    if (bytes) {
        uint8_t *p = slice_bytes(a, &d) + s.len;
        int32_t b = 0;   /* append_src checked that every value is an int */
        if (!src.spread) {
            for (uint32_t k = 0; k < nv; k++) { arg_int(vm, a, R[rb + 3 + k], &b); p[k] = (uint8_t)b; }
        }
        else if (src.str) memmove(p, src.str, src.n);
        else memmove(p, slice_bytes(a, &src.sl), src.n);
    } else {
        term *p = slice_terms(a, &d) + s.len;
        if (!src.spread) memcpy(p, R + rb + 3, (size_t)nv * 4u);
        else terms_copy(p, &src.sl, 0, src.n);
    }
    R[ra] = h;
    return R_NORMAL;
}

/* SLICE s, lo, hi on a slice: 0 <= lo <= hi (the caller checked those)
 * <= cap. */
static uint32_t sl_slice(Vm *vm, Actor *a, term *R, uint32_t ra, uint32_t rb, int32_t lo, int32_t hi) {
    SliceV s;
    if (!slice_view(vm, a, R[rb], &s) || (uint32_t)hi > s.cap) return R_BAD_CODE;
    if (R[rb] == T_NILSLICE || (lo == 0 && (uint32_t)hi == s.len)) {
        R[ra] = R[rb];                                     /* nil[0:0], s[:len(s)] */
        return R_NORMAL;
    }
    term arr = s.arr;
    term h = hdr_alloc(vm, a, &arr, s.off + (uint32_t)lo, (uint32_t)(hi - lo), s.cap - (uint32_t)lo);
    if (h == T_NIL) return R_OUT_OF_MEMORY;
    R = (term *)a->mem + a->fp;
    R[ra] = h;
    return R_NORMAL;
}

/* COPY dst, src: n = min(len(dst), len(src)) elements, overlap allowed. */
static uint32_t sl_copy(Vm *vm, Actor *a, term *R, uint32_t ra, uint32_t rb) {
    SliceV d, s;
    const uint8_t *str = NULL;
    uint32_t slen;
    if (!slice_view(vm, a, R[rb], &d)) return R_BAD_CODE;
    if (str_view(vm, a, R[rb + 1], &str, &slen) || bytes_view(vm, a, R[rb + 1], &str, &slen)) {
        if (d.arr != T_NIL && !d.bytes) return R_BAD_CODE;
    } else {
        str = NULL;
        if (!slice_view(vm, a, R[rb + 1], &s)) return R_BAD_CODE;
        if (d.arr != T_NIL && s.arr != T_NIL && d.bytes != s.bytes) return R_BAD_CODE;
        slen = s.len;
    }
    uint32_t n = d.len < slen ? d.len : slen;
    if (n && d.ro) return R_BAD_CODE;   /* into a const array */
    if (n) {
        if (str) memmove(slice_bytes(a, &d), str, n);
        else if (d.bytes) memmove(slice_bytes(a, &d), slice_bytes(a, &s), n);
        else terms_copy(slice_terms(a, &d), &s, 0, n);
    }
    R[ra] = term_from_int((int32_t)n);
    return R_NORMAL;
}

/* BYTESLICE_FROM s: []byte(s) of a string or a Bytes, a copy; never nil. */
static uint32_t sl_bytes_from(Vm *vm, Actor *a, term *R, uint32_t ra, uint32_t rb) {
    const uint8_t *p;
    uint32_t len;
    if (!seq_bytes(vm, a, R[rb], &p, &len)) return R_BAD_CODE;
    term arr = T_NIL;
    if (len > 0 && !arr_alloc(vm, a, true, len, NULL, 0, &arr)) return R_OUT_OF_MEMORY;
    term h = hdr_alloc(vm, a, &arr, 0, len, len);
    if (h == T_NIL) return R_OUT_OF_MEMORY;
    R = (term *)a->mem + a->fp;
    if (len > 0) {
        seq_bytes(vm, a, R[rb], &p, &len);
        memcpy(a->mem + term_ref_off(arr) + 2, p, len);
    }
    R[ra] = h;
    return R_NORMAL;
}

/* ---- bytes.Bytes and bytes.Buffer ---- */

/* The Buffer t: its K_BUF object, length and capacity; NULL if t is not a
 * well-formed Buffer (the zero Buffer is T_NIL). */
static uint32_t *buf_obj(const Vm *vm, const Actor *a, term t, uint32_t *len, uint32_t *cap) {
    if (!term_is_ref(t)) return NULL;
    uint32_t *o = (uint32_t *)obj_of(vm, a, t);
    if (!o || o[0] != hdr_make(3, K_BUF)) return NULL;
    *cap = 0;
    if (o[2] != T_NIL) {
        bool b;
        if (!arr_obj(vm, a, o[2], &b, cap) || !b) return NULL;
    }
    if (o[1] > *cap) return NULL;
    *len = o[1];
    return o;
}

static uint8_t *buf_bytes(Actor *a, const uint32_t *o) {
    return (uint8_t *)(a->mem + term_ref_off(o[2]) + 2u);
}

/* BUF_NEW cap: an empty Buffer with room for cap bytes. */
static uint32_t buf_new(Vm *vm, Actor *a, term *R, uint32_t ra, uint32_t rb) {
    int32_t cap;
    if (!arg_int(vm, a, R[rb], &cap) || cap < 0) return R_BAD_CODE;
    term arr = T_NIL;
    if (cap > 0 && !arr_alloc(vm, a, true, (uint32_t)cap, NULL, 0, &arr)) return R_OUT_OF_MEMORY;
    uint32_t o = heap_alloc(vm, a, 3, &arr, 1);
    if (o == UINT32_MAX) return R_OUT_OF_MEMORY;
    a->mem[o] = hdr_make(3, K_BUF);
    a->mem[o + 1] = 0;
    a->mem[o + 2] = arr;
    R = (term *)a->mem + a->fp;
    R[ra] = term_from_ref(o);
    return R_NORMAL;
}

/* What BUF_WRITE appends: a byte (an int, low 8 bits), or the bytes of a
 * string, a []byte or a Bytes. */
static bool write_src(const Vm *vm, const Actor *a, term t, uint8_t *byte, const uint8_t **p, uint32_t *n) {
    int32_t v;
    if (int_val(vm, a, t, &v)) {
        *byte = (uint8_t)v;
        *p = byte;
        *n = 1;
        return true;
    }
    return seq_view(vm, a, t, SQ_STR | SQ_BYTES | SQ_BSLICE, p, n);
}

/* BUF_WRITE buf, x: appends, growing the array (x2, at least 16) when full. */
static uint32_t buf_write(Vm *vm, Actor *a, term *R, uint32_t ra, uint32_t rb) {
    uint32_t len, cap, n;
    uint8_t byte;
    const uint8_t *p;
    uint32_t *o = buf_obj(vm, a, R[rb], &len, &cap);
    if (!o || !write_src(vm, a, R[rb + 1], &byte, &p, &n)) return R_BAD_CODE;
    if (n > arr_max(true) - len) return R_OUT_OF_MEMORY;
    uint32_t need = len + n;
    if (need > cap) {
        uint32_t ncap = cap > arr_max(true) / 2u ? arr_max(true) : cap * 2u;
        if (ncap < 16u) ncap = 16u;
        if (ncap < need) ncap = need;
        if (ncap > arr_max(true)) ncap = arr_max(true);
        term arr;
        if (!arr_alloc(vm, a, true, ncap, NULL, 0, &arr)) return R_OUT_OF_MEMORY;
        R = (term *)a->mem + a->fp;                       /* re-derive after GC */
        o = buf_obj(vm, a, R[rb], &len, &cap);
        if (len) memcpy(a->mem + term_ref_off(arr) + 2u, buf_bytes(a, o), len);
        o[2] = arr;
        write_src(vm, a, R[rb + 1], &byte, &p, &n);
    }
    if (n) memmove(buf_bytes(a, o) + len, p, n);
    o[1] = need;
    R[ra] = T_NIL;
    return R_NORMAL;
}

/* BUF_FREEZE buf: its contents as a Bytes (one copy); buf is emptied. */
static uint32_t buf_freeze(Vm *vm, Actor *a, term *R, uint32_t ra, uint32_t rb) {
    uint32_t len, cap;
    uint32_t *o = buf_obj(vm, a, R[rb], &len, &cap);
    if (!o) return R_BAD_CODE;
    if (len == 0) { R[ra] = T_EMPTYBYTES; return R_NORMAL; }
    term out;
    uint8_t *d;
    if (len <= GOGO_BYTES_INLINE) {
        uint32_t off;
        if (!raw_alloc(vm, a, K_BYTES, len, &off)) return R_OUT_OF_MEMORY;
        out = term_from_ref(off);
        d = (uint8_t *)(a->mem + off + 2);
    } else {
        out = bytes_make(vm, a, len, &d);                  /* may GC */
        if (out == T_NIL) return R_OUT_OF_MEMORY;
    }
    R = (term *)a->mem + a->fp;
    o = buf_obj(vm, a, R[rb], &len, &cap);
    memcpy(d, buf_bytes(a, o), len);
    o[1] = 0;
    R[ra] = out;
    return R_NORMAL;
}

/* BYTES_COPYTO x, p: copy(p, x) of a Bytes into a []byte. */
static uint32_t bytes_copyto(Vm *vm, Actor *a, term *R, uint32_t ra, uint32_t rb) {
    const uint8_t *p;
    uint32_t len;
    SliceV d;
    if (!bytes_view(vm, a, R[rb], &p, &len) || !slice_view(vm, a, R[rb + 1], &d) ||
        (d.arr != T_NIL && !d.bytes)) return R_BAD_CODE;
    uint32_t n = d.len < len ? d.len : len;
    if (n && d.ro) return R_BAD_CODE;   /* into a const array */
    if (n) memmove(slice_bytes(a, &d), p, n);
    R[ra] = term_from_int((int32_t)n);
    return R_NORMAL;
}

/* CONFIG_GET key: sys.Config. "wifi." keys are never looked up. The value
 * is copied straight into a new heap string: one lookup for its length,
 * one into the string; a value whose length changed in between (the store
 * written meanwhile) reads as unset. */
static uint32_t config_get(Vm *vm, Actor *a, term *R, uint32_t ra, uint32_t rb) {
    const uint8_t *k;
    uint32_t kl;
    if (!str_view(vm, a, R[rb], &k, &kl)) return R_BAD_CODE;
    int32_t n = -1;
    if (kl < 5u || memcmp(k, "wifi.", 5) != 0) n = vm_config_get(vm, k, kl, NULL, 0);
    term v = T_EMPTYSTR;
    if (n > 0) {
        uint32_t off;
        if (!str_alloc(vm, a, (uint32_t)n, &off)) return R_OUT_OF_MEMORY;
        R = (term *)a->mem + a->fp;
        str_view(vm, a, R[rb], &k, &kl);
        if (vm_config_get(vm, k, kl, (uint8_t *)(a->mem + off + 2), (uint32_t)n) == n) v = term_from_ref(off);
        else n = -1;
    }
    R[ra] = v;
    R[ra + 1] = term_from_bool(n >= 0);
    return R_NORMAL;
}

/* ---- B_SPRINT (15.6) ---- */

/* SPRINT mode, v (gogo/fmt): the text println prints for v, without the
 * newline: mode 0 a new string (the text counted, then written into it),
 * mode 1 written to the console (nil). Not static, so that it is not
 * inlined: its Out buffer would make strs_builtin's frame large. */
uint32_t strs_sprint(Vm *vm, Actor *a, term *R, uint32_t ra, uint32_t rb) {
    int32_t mode;
    uint32_t off = UINT32_MAX;   /* the string, once allocated */
    size_t len = 0;
    Out o;
    if (!arg_int(vm, a, R[rb], &mode)) return R_BAD_CODE;
    for (;;) {
        out_init(&o, vm, mode ? 1 : 0);
        if (off != UINT32_MAX) {
            o.dst = (char *)(a->mem + off + 2);
            o.cap = len;
        }
        if (!term_text(&o, vm, a, R[rb + 1])) return R_BAD_CODE;
        out_flush(&o);
        if (mode || off != UINT32_MAX || !(len = o.n)) break;
        if (!str_alloc(vm, a, (uint32_t)len, &off)) return R_OUT_OF_MEMORY;
        R = (term *)a->mem + a->fp;   /* the allocation may have moved everything */
    }
    /* The second pass must print what the first counted; were the text
     * ever to change between them, it stops at the string's end. */
    if (off != UINT32_MAX && o.n != len) return R_BAD_CODE;
    R[ra] = mode ? T_NIL : off != UINT32_MAX ? term_from_ref(off) : T_EMPTYSTR;
    return R_NORMAL;
}

/* ---- order (B_CMP) ---- */

/* The order of two byte strings: by their first differing byte, then by
 * length; -1, 0 or 1 (B_STR_CMP, B_CMP). */
static int strs_order(const uint8_t *p, uint32_t lp, const uint8_t *q, uint32_t lq) {
    uint32_t m = lp < lq ? lp : lq;
    int c = m ? memcmp(p, q, m) : 0;
    if (c == 0) c = lp < lq ? -1 : (lp > lq ? 1 : 0);
    return c < 0 ? -1 : (c > 0 ? 1 : 0);
}


/* One side of B_CMP. ty is its rank: K_INT for an int (immediate or
 * boxed), the heap kind of an int64 box, Ref, string, Bytes or other
 * object (a closure, a Buffer: by identity), K_ARRAY or K_BYTEARR for a
 * slice (an array's value: by its elements), K_FLOAT (+ 32 for a
 * float32) for a float, 64 + the type word for a struct, 0 for the other
 * immediates (bools, PIDs, nil, the zero Ref: by term). Then what orders
 * it within its rank: hi:lo (an int with its sign bit flipped, an int64,
 * a Ref serial, a term), bytes p, n (strings, Bytes, byte arrays), the
 * object p (a float: float_order), or the terms e, n (a struct's fields,
 * an array's elements). Values of different gogo types never share a rank
 * and a value: an int64 is boxed in an interface and in its plain
 * representation outside one. */
typedef struct CmpKey {
    uint32_t ty, n;
    int32_t hi;
    uint32_t lo;
    const uint8_t *p;
    const term *e;
} CmpKey;

static void cmp_key(const Vm *vm, const Actor *a, term t, CmpKey *k) {
    SliceV s;
    const uint32_t *o = obj_of(vm, a, t);
    uint32_t kind = o ? hdr_kind(o[0]) : 0u;
    k->e = NULL;
    k->hi = 0;
    k->lo = t;
    k->ty = kind;
    if (term_is_int(t) || kind == K_INT) {
        k->ty = K_INT;
        k->lo = (term_is_int(t) ? (uint32_t)term_to_int(t) : o[1]) ^ 0x80000000u;
    } else if (kind == K_INT64 || kind == K_REF) {
        k->hi = (int32_t)o[2];
        k->lo = o[1];
    } else if (seq_bytes(vm, a, t, &k->p, &k->n)) k->ty = t == T_EMPTYSTR || kind == K_STRING ? K_STRING : K_BYTES;
    else if (kind == K_STRUCT && !(o[1] & STRUCT_MUT)) {   /* a mutable one: by identity (15.6) */
        k->ty = struct_type(t, o) + 64u;
        k->e = o + 2;
        k->n = hdr_words(o[0]) - 2u;
    } else if (slice_view(vm, a, t, &s)) {   /* an array's value: by its elements */
        k->n = s.len;
        k->ty = s.bytes ? K_BYTEARR : K_ARRAY;
        if (s.len) {
            k->p = slice_bytes((Actor *)a, &s);
            k->e = s.bytes ? NULL : slice_terms((Actor *)a, &s);
        }
    }
#if GOGO_FLOAT
    else if (kind == K_FLOAT) {   /* by value (float_order); a float32 is its own rank */
        k->ty = hdr_words(o[0]) == 3u ? K_FLOAT : K_FLOAT + 32u;
        k->p = (const uint8_t *)o;
    }
#endif
}

#define CMP_DEPTH 64   /* nesting (other than through the last field or element) */

/* The terms one B_CMP may visit, deep_live's WALK_BUDGET: a compare walks
 * without marking what it has seen, so values sharing subvalues (a DAG,
 * x = N{x, x} in a loop) can take width^depth visits; without sharing
 * every visit but the first reads a distinct heap word, so the limit is
 * never reached. B_CMP does not preempt: past the limit it is badcode,
 * not a VM frozen in one instruction. */
#define CMP_BUDGET 65536

/* B_CMP (opcodes.h): the order of x and y, -1, 0 or 1, or R_BAD_CODE past
 * CMP_DEPTH or after *left visits. Ranks first, then values; structs field
 * by field and arrays element by element (then by length), the last one in
 * this loop, so a list linked through it does not nest. Allocates nothing.
 * Unlike println's walk it does not check for a gone module's values: a
 * module's own values never reach another module, and the errors that do
 * are of reserved types. */
static int cmp_terms(const Vm *vm, const Actor *a, term x, term y, int depth, int32_t *left, bool total) {
    for (;;) {
        if (--*left < 0) return R_BAD_CODE;
        CmpKey kx, ky;
        cmp_key(vm, a, x, &kx);
        cmp_key(vm, a, y, &ky);
        uint32_t u = kx.ty, v = ky.ty;
        /* total (maps): a func, a Buffer or a mutable struct (a pointer, a
         * map), at any depth, is unordered (-2): its identity moves */
        if (total && (u == K_CLOSURE || u == K_BUF || u == K_STRUCT || v == K_CLOSURE || v == K_BUF || v == K_STRUCT))
            return -2;
        if (u == v) {
            if (u == K_STRING || u == K_BYTES || u == K_BYTEARR) return strs_order(kx.p, kx.n, ky.p, ky.n);
#if GOGO_FLOAT
            if (u == K_FLOAT || u == K_FLOAT + 32u) {   /* a NaN (2) equals nothing */
                const uint32_t *fx = (const uint32_t *)(const void *)kx.p, *fy = (const uint32_t *)(const void *)ky.p;
                int c = float_order(fx, fy);
                /* total (maps, 15.6): NaNs equal, before every other float */
                if (c > 1 && total) return (float_order(fy, fy) > 1) - (float_order(fx, fx) > 1);
                return c > 1 ? 1 : c;
            }
#endif
            if (kx.hi != ky.hi) return kx.hi < ky.hi ? -1 : 1;
            u = kx.lo;
            v = ky.lo;
            if (kx.e || kx.ty == K_ARRAY) {   /* fields or elements */
                u = kx.n;
                v = ky.n;
                if (u == v && u) {
                    if (depth > CMP_DEPTH) return R_BAD_CODE;
                    for (uint32_t i = 0; i + 1u < u; i++) {
                        int c = cmp_terms(vm, a, field_term(x, kx.e[i]), field_term(y, ky.e[i]), depth + 1, left, total);
                        if (c) return c;
                    }
                    x = field_term(x, kx.e[u - 1u]);
                    y = field_term(y, ky.e[u - 1u]);
                    continue;
                }
            }
        }
        return (u > v) - (u < v);
    }
}

uint32_t strs_builtin(Vm *vm, Actor *a, uint32_t id, uint32_t ra, uint32_t rb, uint32_t rc) {
    if (id == B_CMP) {
        term *T = (term *)a->mem + a->fp;
        int32_t left = CMP_BUDGET;
        SliceV s;
        /* Two arguments: interface values or structs, so a slice at the
         * top is one in an interface, which equals nothing (Go: false for
         * slices of two types, a panic for one type). Three: arrays. Four
         * (15.6, gogo/maps): arrays, in a total order in which a NaN
         * equals a NaN; a func, a Buffer, a pointer or a map at any depth
         * is unordered, 2, and so with mode 2 is a slice at the top. */
        bool total = rc == 4u;
        int c = rc - 2u > 2u ? R_BAD_CODE
              : (rc == 2u || (total && T[rb + 3] == term_from_int(2))) &&
                (slice_view(vm, a, T[rb], &s) || slice_view(vm, a, T[rb + 1], &s)) ? 1 - 3 * total
              : cmp_terms(vm, a, T[rb], T[rb + 1], 0, &left, total);
        if (c > 1) return (uint32_t)c;
        T[ra] = term_from_int(c == -2 ? 2 : c);
        return R_NORMAL;
    }
    term *R = (term *)a->mem + a->fp;
    const uint8_t *p, *q;
    uint32_t lp, lq;
    int32_t i, hi;
    SliceV s;
    switch (id) {
    case B_STR_CONCAT:
        if (rc != 2u) return R_BAD_CODE;
        return str_concat(vm, a, R, ra, rb);
    case B_LEN:
        if (rc != 1u) return R_BAD_CODE;
        if (str_view(vm, a, R[rb], &p, &lp) || bytes_view(vm, a, R[rb], &p, &lp)) {}
        else if (slice_view(vm, a, R[rb], &s)) lp = s.len;
        else if (!buf_obj(vm, a, R[rb], &lp, &lq)) return R_BAD_CODE;
        R[ra] = term_from_int((int32_t)lp);
        return R_NORMAL;
    case B_CAP:
        if (rc != 1u || !slice_view(vm, a, R[rb], &s)) return R_BAD_CODE;
        R[ra] = term_from_int((int32_t)s.cap);
        return R_NORMAL;
    case B_INDEX:
        if (rc != 2u || !arg_int(vm, a, R[rb + 1], &i) || i < 0) return R_BAD_CODE;
        if (seq_bytes(vm, a, R[rb], &p, &lp)) {
            if ((uint32_t)i >= lp) return R_BAD_CODE;
            R[ra] = term_from_int(p[i]);
        } else if (slice_view(vm, a, R[rb], &s)) {
            if ((uint32_t)i >= s.len) return R_BAD_CODE;
            R[ra] = s.bytes ? term_from_int(slice_bytes(a, &s)[i]) : field_term(s.arr, slice_terms(a, &s)[i]);
        } else {
            return R_BAD_CODE;
        }
        return R_NORMAL;
    case B_SETINDEX: {
        if (rc != 3u || !slice_view(vm, a, R[rb], &s) || !arg_int(vm, a, R[rb + 1], &i) ||
            i < 0 || (uint32_t)i >= s.len || s.ro) return R_BAD_CODE;   /* a const array is read-only */
        if (s.bytes) {
            int32_t v;
            if (!arg_int(vm, a, R[rb + 2], &v)) return R_BAD_CODE;
            slice_bytes(a, &s)[i] = (uint8_t)v;
        } else {
            slice_terms(a, &s)[i] = R[rb + 2];
        }
        R[ra] = T_NIL;
        return R_NORMAL;
    }
    case B_SLICE:
        /* lo, hi: 0 <= lo <= hi <= len (a slice's hi: cap) */
        if (rc != 3u || !arg_int(vm, a, R[rb + 1], &i) || !arg_int(vm, a, R[rb + 2], &hi) || i < 0 || hi < i)
            return R_BAD_CODE;
        lq = str_view(vm, a, R[rb], &p, &lp) ? SQ_STR : bytes_view(vm, a, R[rb], &p, &lp) ? SQ_BYTES : 0u;
        if (lq) {   /* a string or a Bytes: empty, the whole, a shared root or a copy */
            uint32_t n = (uint32_t)(hi - i);
            if ((uint32_t)hi > lp) return R_BAD_CODE;
            if (n && n == lp) { R[ra] = R[rb]; return R_NORMAL; }
            /* a part larger than GOGO_BYTES_INLINE of an off-heap Bytes shares its root */
            if (term_is_handle(R[rb]) && n > GOGO_BYTES_INLINE) {
                term h = bytes_sub(vm, a, R[rb], (uint32_t)i, n);   /* may GC */
                if (h == T_NIL) return R_OUT_OF_MEMORY;
                ((term *)a->mem + a->fp)[ra] = h;
                return R_NORMAL;
            }
            return seq_copy(vm, a, ra, rb, lq == SQ_STR ? K_STRING : K_BYTES, lq, (uint32_t)i, n);
        }
        return sl_slice(vm, a, R, ra, rb, i, hi);
    case B_MAKE_SLICE:
        if (rc != 4u) return R_BAD_CODE;
        return sl_make(vm, a, R, ra, rb);
    case B_APPEND:
        if (rc < 4u) return R_BAD_CODE;
        return sl_append(vm, a, R, ra, rb, rc);
    case B_COPY:
        if (rc != 2u) return R_BAD_CODE;
        return sl_copy(vm, a, R, ra, rb);
    case B_STR_FROM:   /* string(b) of a []byte, x.String() of a Bytes: a copy */
        lq = SQ_BYTES | SQ_BSLICE;
        goto copy;
    case B_BYTESLICE_FROM:
        if (rc != 1u) return R_BAD_CODE;
        return sl_bytes_from(vm, a, R, ra, rb);
    case B_STR_CMP:
        if (rc != 2u) return R_BAD_CODE;
        if (!str_view(vm, a, R[rb], &p, &lp) || !str_view(vm, a, R[rb + 1], &q, &lq)) return R_BAD_CODE;
        R[ra] = term_from_int(strs_order(p, lp, q, lq));
        return R_NORMAL;
    case B_BUF_NEW:
        if (rc != 1u) return R_BAD_CODE;
        return buf_new(vm, a, R, ra, rb);
    case B_BUF_WRITE:
        if (rc != 2u) return R_BAD_CODE;
        return buf_write(vm, a, R, ra, rb);
    case B_BUF_FREEZE:
        if (rc != 1u) return R_BAD_CODE;
        return buf_freeze(vm, a, R, ra, rb);
    case B_BYTES_FROM:   /* a heap Bytes copy of a string or []byte (any size) */
        lq = SQ_STR | SQ_BSLICE;
    copy:
        if (rc != 1u || !seq_view(vm, a, R[rb], lq, &p, &lp)) return R_BAD_CODE;
        return seq_copy(vm, a, ra, rb, id == B_STR_FROM ? K_STRING : K_BYTES, lq, 0, lp);
    case B_BYTES_EQUAL:
        if (rc != 2u || !bytes_view(vm, a, R[rb], &p, &lp) || !bytes_view(vm, a, R[rb + 1], &q, &lq))
            return R_BAD_CODE;
        R[ra] = term_from_bool(lp == lq && (lp == 0 || memcmp(p, q, lp) == 0));
        return R_NORMAL;
    case B_BYTES_COPYTO:
        if (rc != 2u) return R_BAD_CODE;
        return bytes_copyto(vm, a, R, ra, rb);
    case B_CONFIG_GET:
        if (rc != 1u) return R_BAD_CODE;
        return config_get(vm, a, R, ra, rb);
    case B_SPRINT:
        if (rc != 2u) return R_BAD_CODE;
        return strs_sprint(vm, a, R, ra, rb);
    default:
        return R_BAD_CODE;
    }
}

bool strs_equal(const Vm *vm, const Actor *a, term x, term y, bool *eq) {
    const uint8_t *p, *q;
    uint32_t lp, lq;
    if (!str_view(vm, a, x, &p, &lp) || !str_view(vm, a, y, &q, &lq)) return false;
    *eq = lp == lq && (lp == 0 || memcmp(p, q, lp) == 0);
    return true;
}
