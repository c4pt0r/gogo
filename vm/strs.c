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
#include "heap.h"
#include "opcodes.h"
#include "bytes.h"
#include "config.h"
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

static uint32_t str_slice(Vm *vm, Actor *a, term *R, uint32_t ra, uint32_t rb) {
    const uint8_t *p;
    uint32_t len;
    int32_t lo, hi;
    if (!str_view(vm, a, R[rb], &p, &len)) return R_BAD_CODE;
    if (!arg_int(vm, a, R[rb + 1], &lo) || !arg_int(vm, a, R[rb + 2], &hi)) return R_BAD_CODE;
    if (lo < 0 || hi < lo || (uint32_t)hi > len) return R_BAD_CODE;
    uint32_t n = (uint32_t)(hi - lo);
    if (n == 0) { R[ra] = T_EMPTYSTR; return R_NORMAL; }
    if (n == len) { R[ra] = R[rb]; return R_NORMAL; }   /* the whole string */
    uint32_t off;
    if (!str_alloc(vm, a, n, &off)) return R_OUT_OF_MEMORY;
    R = (term *)a->mem + a->fp;
    str_view(vm, a, R[rb], &p, &len);
    memcpy(a->mem + off + 2, p + lo, n);
    R[ra] = term_from_ref(off);
    return R_NORMAL;
}

/* ---- slices ---- */

/* A slice argument: arr is T_NIL when cap is 0 (always for nil). */
typedef struct SliceV {
    term arr;
    uint32_t off, len, cap;
    bool bytes;          /* arr is a K_BYTEARR (false when arr is T_NIL) */
} SliceV;

/* The elements of the K_ARRAY / K_BYTEARR ref t: *elems is its element
 * capacity. NULL if t is neither. */
static uint32_t *arr_obj(const Vm *vm, const Actor *a, term t, bool *bytes, uint32_t *elems) {
    if (!term_is_ref(t)) return NULL;
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

/* Reads the slice t (nil or a well-formed heap K_SLICE); false otherwise. */
static bool slice_view(const Vm *vm, const Actor *a, term t, SliceV *s) {
    s->arr = T_NIL;
    s->off = s->len = s->cap = 0;
    s->bytes = false;
    if (t == T_NILSLICE) return true;
    if (!term_is_ref(t)) return false;
    const uint32_t *o = obj_of(vm, a, t);
    if (!o || o[0] != hdr_make(5, K_SLICE)) return false;
    s->off = o[1];
    s->len = o[2];
    s->cap = o[3];
    s->arr = o[4];
    if (s->len > s->cap) return false;
    if (s->arr == T_NIL) return s->cap == 0;
    uint32_t elems;
    if (!arr_obj(vm, a, s->arr, &s->bytes, &elems)) return false;
    return s->off <= elems && s->cap <= elems - s->off;
}

/* Pointer to element i (< cap) of a term slice, or byte i of a byte slice. */
static term *slice_terms(Actor *a, const SliceV *s) {
    return (term *)a->mem + term_ref_off(s->arr) + 2u + s->off;
}
static uint8_t *slice_bytes(Actor *a, const SliceV *s) {
    return (uint8_t *)(a->mem + term_ref_off(s->arr) + 2u) + s->off;
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
    SliceV d = { arr, off, need, cap, bytes };
    if (grow) {
        arr_fill(a, arr, bytes, need, cap, R[rb + 2]);
        if (bytes && s.len) memcpy(slice_bytes(a, &d), slice_bytes(a, &s), s.len);
        else if (s.len) memcpy(slice_terms(a, &d), slice_terms(a, &s), (size_t)s.len * 4u);
    }
    /* The new elements; a spread source may overlap the destination. */
    if (bytes) {
        uint8_t *p = slice_bytes(a, &d) + s.len;
        int32_t b;
        if (!src.spread) {
            for (uint32_t k = 0; k < nv; k++) { arg_int(vm, a, R[rb + 3 + k], &b); p[k] = (uint8_t)b; }
        }
        else if (src.str) memmove(p, src.str, src.n);
        else memmove(p, slice_bytes(a, &src.sl), src.n);
    } else {
        term *p = slice_terms(a, &d) + s.len;
        if (!src.spread) memcpy(p, R + rb + 3, (size_t)nv * 4u);
        else memmove(p, slice_terms(a, &src.sl), (size_t)src.n * 4u);
    }
    R[ra] = h;
    return R_NORMAL;
}

/* SLICE s, lo, hi on a slice: 0 <= lo <= hi <= cap. */
static uint32_t sl_slice(Vm *vm, Actor *a, term *R, uint32_t ra, uint32_t rb) {
    SliceV s;
    int32_t lo, hi;
    if (!slice_view(vm, a, R[rb], &s)) return R_BAD_CODE;
    if (!arg_int(vm, a, R[rb + 1], &lo) || !arg_int(vm, a, R[rb + 2], &hi)) return R_BAD_CODE;
    if (lo < 0 || hi < lo || (uint32_t)hi > s.cap) return R_BAD_CODE;
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
    if (n) {
        if (str) memmove(slice_bytes(a, &d), str, n);
        else if (d.bytes) memmove(slice_bytes(a, &d), slice_bytes(a, &s), n);
        else memmove(slice_terms(a, &d), slice_terms(a, &s), (size_t)n * 4u);
    }
    R[ra] = term_from_int((int32_t)n);
    return R_NORMAL;
}

/* STR_FROM b: string(b) of a []byte, or x.String() of a Bytes: a copy. */
static uint32_t sl_str_from(Vm *vm, Actor *a, term *R, uint32_t ra, uint32_t rb) {
    SliceV s;
    const uint8_t *p;
    uint32_t len;
    if (bytes_view(vm, a, R[rb], &p, &len)) {
        if (len == 0) { R[ra] = T_EMPTYSTR; return R_NORMAL; }
        uint32_t off;
        if (!str_alloc(vm, a, len, &off)) return R_OUT_OF_MEMORY;
        R = (term *)a->mem + a->fp;
        bytes_view(vm, a, R[rb], &p, &len);
        memcpy(a->mem + off + 2, p, len);
        R[ra] = term_from_ref(off);
        return R_NORMAL;
    }
    if (!slice_view(vm, a, R[rb], &s) || (s.arr != T_NIL && !s.bytes)) return R_BAD_CODE;
    if (s.len == 0) { R[ra] = T_EMPTYSTR; return R_NORMAL; }
    uint32_t off;
    if (!str_alloc(vm, a, s.len, &off)) return R_OUT_OF_MEMORY;
    R = (term *)a->mem + a->fp;
    slice_view(vm, a, R[rb], &s);
    memcpy(a->mem + off + 2, slice_bytes(a, &s), s.len);
    R[ra] = term_from_ref(off);
    return R_NORMAL;
}

/* A string or a Bytes: its bytes. */
static bool seq_bytes(const Vm *vm, const Actor *a, term t, const uint8_t **p, uint32_t *len) {
    return str_view(vm, a, t, p, len) || bytes_view(vm, a, t, p, len);
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

/* A new heap K_BYTES holding the n (> 0) bytes that view() finds in the
 * register term R[r] after the allocation (it may GC), from byte lo on. */
typedef bool (*ViewFn)(const Vm *, const Actor *, term, const uint8_t **, uint32_t *);

static uint32_t bytes_copy(Vm *vm, Actor *a, uint32_t ra, uint32_t r, ViewFn view, uint32_t lo, uint32_t n) {
    uint32_t off;
    if (!raw_alloc(vm, a, K_BYTES, n, &off)) return R_OUT_OF_MEMORY;
    term *R = (term *)a->mem + a->fp;
    const uint8_t *p;
    uint32_t len;
    view(vm, a, R[r], &p, &len);
    memcpy(a->mem + off + 2, p + lo, n);
    R[ra] = term_from_ref(off);
    return R_NORMAL;
}

/* SLICE x, lo, hi of a Bytes: 0 <= lo <= hi <= len. Empty is T_EMPTYBYTES,
 * the whole is x; a part larger than GOGO_BYTES_INLINE of an off-heap x
 * shares its root, anything else is a heap copy. */
static uint32_t bytes_slice(Vm *vm, Actor *a, term *R, uint32_t ra, uint32_t rb) {
    const uint8_t *p;
    uint32_t len;
    int32_t lo, hi;
    if (!bytes_view(vm, a, R[rb], &p, &len)) return R_BAD_CODE;
    if (!arg_int(vm, a, R[rb + 1], &lo) || !arg_int(vm, a, R[rb + 2], &hi)) return R_BAD_CODE;
    if (lo < 0 || hi < lo || (uint32_t)hi > len) return R_BAD_CODE;
    uint32_t n = (uint32_t)(hi - lo);
    if (n == 0) { R[ra] = T_EMPTYBYTES; return R_NORMAL; }
    if (n == len) { R[ra] = R[rb]; return R_NORMAL; }
    if (term_is_handle(R[rb]) && n > GOGO_BYTES_INLINE) {
        term h = bytes_sub(vm, a, R[rb], (uint32_t)lo, n);   /* may GC */
        if (h == T_NIL) return R_OUT_OF_MEMORY;
        R = (term *)a->mem + a->fp;
        R[ra] = h;
        return R_NORMAL;
    }
    return bytes_copy(vm, a, ra, rb, bytes_view, (uint32_t)lo, n);
}

/* BYTES_FROM x: a heap Bytes copy of a string or []byte (any size). */
static bool str_or_byteslice(const Vm *vm, const Actor *a, term t, const uint8_t **p, uint32_t *len) {
    SliceV s;
    if (str_view(vm, a, t, p, len)) return true;
    if (!slice_view(vm, a, t, &s) || (s.arr != T_NIL && !s.bytes)) return false;
    *p = s.len ? slice_bytes((Actor *)a, &s) : NULL;
    *len = s.len;
    return true;
}

static uint32_t bytes_from(Vm *vm, Actor *a, term *R, uint32_t ra, uint32_t rb) {
    const uint8_t *p;
    uint32_t len;
    if (!str_or_byteslice(vm, a, R[rb], &p, &len)) return R_BAD_CODE;
    if (len == 0) { R[ra] = T_EMPTYBYTES; return R_NORMAL; }
    return bytes_copy(vm, a, ra, rb, str_or_byteslice, 0, len);
}

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
    return str_or_byteslice(vm, a, t, p, n) || bytes_view(vm, a, t, p, n);
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
    if (n) memmove(slice_bytes(a, &d), p, n);
    R[ra] = term_from_int((int32_t)n);
    return R_NORMAL;
}

uint32_t strs_builtin(Vm *vm, Actor *a, uint32_t id, uint32_t ra, uint32_t rb, uint32_t rc) {
    term *R = (term *)a->mem + a->fp;
    const uint8_t *p, *q;
    uint32_t lp, lq;
    int32_t i;
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
            R[ra] = s.bytes ? term_from_int(slice_bytes(a, &s)[i]) : slice_terms(a, &s)[i];
        } else {
            return R_BAD_CODE;
        }
        return R_NORMAL;
    case B_SETINDEX: {
        if (rc != 3u || !slice_view(vm, a, R[rb], &s) || !arg_int(vm, a, R[rb + 1], &i) ||
            i < 0 || (uint32_t)i >= s.len) return R_BAD_CODE;
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
        if (rc != 3u) return R_BAD_CODE;
        if (str_view(vm, a, R[rb], &p, &lp)) return str_slice(vm, a, R, ra, rb);
        if (bytes_view(vm, a, R[rb], &p, &lp)) return bytes_slice(vm, a, R, ra, rb);
        return sl_slice(vm, a, R, ra, rb);
    case B_MAKE_SLICE:
        if (rc != 4u) return R_BAD_CODE;
        return sl_make(vm, a, R, ra, rb);
    case B_APPEND:
        if (rc < 4u) return R_BAD_CODE;
        return sl_append(vm, a, R, ra, rb, rc);
    case B_COPY:
        if (rc != 2u) return R_BAD_CODE;
        return sl_copy(vm, a, R, ra, rb);
    case B_STR_FROM:
        if (rc != 1u) return R_BAD_CODE;
        return sl_str_from(vm, a, R, ra, rb);
    case B_BYTESLICE_FROM:
        if (rc != 1u) return R_BAD_CODE;
        return sl_bytes_from(vm, a, R, ra, rb);
    case B_STR_CMP: {
        if (rc != 2u) return R_BAD_CODE;
        if (!str_view(vm, a, R[rb], &p, &lp) || !str_view(vm, a, R[rb + 1], &q, &lq)) return R_BAD_CODE;
        uint32_t m = lp < lq ? lp : lq;
        int c = m ? memcmp(p, q, m) : 0;
        if (c == 0) c = lp < lq ? -1 : (lp > lq ? 1 : 0);
        R[ra] = term_from_int(c < 0 ? -1 : (c > 0 ? 1 : 0));
        return R_NORMAL;
    }
    case B_BUF_NEW:
        if (rc != 1u) return R_BAD_CODE;
        return buf_new(vm, a, R, ra, rb);
    case B_BUF_WRITE:
        if (rc != 2u) return R_BAD_CODE;
        return buf_write(vm, a, R, ra, rb);
    case B_BUF_FREEZE:
        if (rc != 1u) return R_BAD_CODE;
        return buf_freeze(vm, a, R, ra, rb);
    case B_BYTES_FROM:
        if (rc != 1u) return R_BAD_CODE;
        return bytes_from(vm, a, R, ra, rb);
    case B_BYTES_EQUAL:
        if (rc != 2u || !bytes_view(vm, a, R[rb], &p, &lp) || !bytes_view(vm, a, R[rb + 1], &q, &lq))
            return R_BAD_CODE;
        R[ra] = term_from_bool(lp == lq && (lp == 0 || memcmp(p, q, lp) == 0));
        return R_NORMAL;
    case B_BYTES_COPYTO:
        if (rc != 2u) return R_BAD_CODE;
        return bytes_copyto(vm, a, R, ra, rb);
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
