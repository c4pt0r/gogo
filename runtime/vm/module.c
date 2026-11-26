/* The module table: loading, the unified boot, spawning by export name and
 * unloading (module.h). */
#include "module.h"
#include "alloc.h"
#include "bytes.h"
#include "heap.h"
#include "interp.h"
#include "opcodes.h"
#include <string.h>

/* Words of a loaded image, header to the end of the exports. */
static uint32_t image_words(const Image *img) {
    return (uint32_t)(img->exports - (img->funcs - IMAGE_HEADER_WORDS)) + img->n_exports * 2u;
}

uint32_t module_code_words(const ModEntry *e) { return image_words(&e->img); }

/* A new entry for img in slot s (which must be free). */
static ModEntry *install(Vm *vm, uint32_t s, const Image *img, uint8_t flags) {
    ModEntry *e = va_alloc(vm, sizeof *e);
    if (!e) return NULL;
    memset(e, 0, sizeof *e);
    e->img = *img;
    e->bytes = T_NIL;
    for (uint32_t i = 0; i < MOD_HOLDERS; i++) e->holders[i] = MOD_NO_HOLDER;
    e->flags = flags;
    if (flags & MOD_COUNTED) vm->mod_mem += module_code_words(e) * 4u;
    vm->mods[s] = e;
    return e;
}

/* Gives back a code copy of n bytes made under flags (MOD_OWN_*). */
static void free_code(Vm *vm, const uint32_t *w, uint32_t n, uint8_t flags) {
    if (flags & MOD_OWN_PLATFORM) vm_code_free(vm, (void *)(uintptr_t)w, n);
    else if (flags & MOD_OWN_COPY) va_free(vm, (void *)(uintptr_t)w, n);
}

static void unload(Vm *vm, uint32_t s) {
    ModEntry *e = vm->mods[s];
    uint32_t n = module_code_words(e) * 4u;
    if (e->flags & MOD_COUNTED) vm->mod_mem -= n;
    if (e->bytes != T_NIL) bytes_msg_unref(vm, e->bytes);
    free_code(vm, e->img.funcs - IMAGE_HEADER_WORDS, n, e->flags);
    vm->mods[s] = NULL;
    vm->mod_gen[s]++;                  /* every ref to it is stale from now on */
    va_free(vm, e, sizeof *e);
}

/* Ends holder who's hold on slot s's module (MOD_NO_HOLDER: none) and
 * unloads it if it is left with neither actor nor holder. */
static void drop(Vm *vm, uint32_t s, uint16_t who) {
    ModEntry *e = vm->mods[s];
    bool held = (e->flags & MOD_VM_HELD) != 0;
    for (uint32_t i = 0; i < MOD_HOLDERS; i++) {
        if (e->holders[i] == who) e->holders[i] = MOD_NO_HOLDER;
        if (e->holders[i] != MOD_NO_HOLDER) held = true;
    }
    if (!e->live && !held) unload(vm, s);
}

/* The capability check of every load (boot included). */
static bool caps_ok(const Vm *vm, const Image *img, uint32_t *missing) {
    return vm_caps_ok(img->exports[-2], vm->caps, missing) != 0;   /* the word before n_exports */
}

int module_install_static(Vm *vm, const Image *img) {
    if (vm->mods[0]) return CODE_TOO_MANY;
    return install(vm, 0, img, MOD_VM_HELD) ? CODE_OK : CODE_NO_MEMORY;
}

int module_load_static(Vm *vm, const uint32_t *words, uint32_t n_words) {
    Image img;
    int rc = image_load(&img, words, n_words);
    if (rc != IMG_OK) return rc;
    uint32_t missing;
    if (!caps_ok(vm, &img, &missing)) {
        char msg[VM_CAPS_MSG_MAX];
        vm_write_err(vm, msg, vm_caps_explain(msg, img.exports[-2], missing));
        return CODE_UNSUPPORTED;
    }
    return module_install_static(vm, &img);
}

void module_config(Vm *vm) {
    char v[12];
    int32_t n = vm_config_get(vm, (const uint8_t *)"code.mem", 8, (uint8_t *)v, sizeof v);
    if (n < 0) return;
    uint64_t x = 0;
    int32_t i = 0;
    while (i < n && i < (int32_t)sizeof v && v[i] >= '0' && v[i] <= '9') x = x * 10u + (uint64_t)(v[i++] - '0');
    if (i > 0 && i + 1 == n && (v[i] | 0x20) == 'k') { x <<= 10; i++; }
    else if (i > 0 && i + 1 == n && (v[i] | 0x20) == 'm') { x <<= 20; i++; }
    if (i == 0 || i != n || x > 0xFFFFFFFFu) {   /* 12 bytes: no overflow before this check */
        static const char msg[] = "gogo: bad code.mem; using the default\n";
        vm_write_err(vm, msg, sizeof msg - 1u);
        return;
    }
    vm->mod_cap = (uint32_t)x;
}

int vm_boot(Vm *vm, const uint32_t *words, uint32_t n_words) {
    module_config(vm);
    int rc = module_load_static(vm, words, n_words);
    if (rc == CODE_OK) return VM_BOOT_OK;
    /* Refused: give back the actor table, so a caller may boot again. */
    va_free(vm, vm->table, (size_t)vm->table_cap * sizeof(uintptr_t));
    vm->table = NULL;
    vm->table_cap = 0;
    if (rc < 0) return rc;
    return rc == CODE_UNSUPPORTED ? VM_BOOT_REFUSED : VM_BOOT_NO_MEMORY;
}

/* A copy of len bytes of src as module code: in platform memory if the
 * platform gives some (aligned: an unaligned block goes back), else, if
 * vm_too, in VM memory. Sets the MOD_OWN_* flag; NULL if neither. */
static uint32_t *copy_code(Vm *vm, const uint8_t *src, uint32_t len, bool vm_too, uint8_t *flags) {
    uint32_t *d = vm_code_alloc(vm, len);
    if (d && ((uintptr_t)d & 3u)) {
        vm_code_free(vm, d, len);
        d = NULL;
    }
    if (d) *flags |= MOD_OWN_PLATFORM;
    else if (vm_too && (d = va_alloc(vm, len)) != NULL) *flags |= MOD_OWN_COPY;
    if (d) memcpy(d, src, len);
    return d;
}

int module_load(Vm *vm, Actor *caller, term bytes, uint32_t *handle) {
    const uint8_t *data;
    uint32_t len;
    if (!caller || !bytes_view(vm, caller, bytes, &data, &len)) return CODE_BAD_ARG;
    if (len > vm->mod_cap) return CODE_NO_MEMORY;           /* can never fit: before any copy */
    /* In place only from a whole off-heap root: what it keeps alive is
     * what is counted (a slice would pin its whole root). */
    bool in_place = !((uintptr_t)data & 3u) && bytes_pinned_len(vm, bytes) == len;
    const uint32_t *words = (const uint32_t *)(const void *)data;
    uint8_t flags = MOD_COUNTED;
    /* Unaligned: validate an aligned copy, made where the code will live. */
    if (((uintptr_t)data & 3u) && !(words = copy_code(vm, data, len, true, &flags))) return CODE_NO_MEMORY;
    Image img;
    uint32_t missing, s = vm->mod_next, tries = 0;
    int rc = CODE_BAD_IMAGE;
    if (len & 3u) goto fail;
    int ir = image_load(&img, words, len / 4u);
    if (ir == IMG_ERR_VERSION) rc = CODE_BAD_VERSION;
    if (ir != IMG_OK || image_words(&img) != len / 4u) goto fail;   /* exactly one image */
    if (!caps_ok(vm, &img, &missing)) {
        *handle = missing;
        rc = CODE_UNSUPPORTED;
        goto fail;
    }
    rc = CODE_NO_MEMORY;
    if (vm->mod_mem > vm->mod_cap - len) goto fail;
    /* Round-robin from the slot after the last one used: reloads spread
     * over every slot, so a stale tag needs 256 loads into its own slot
     * to alias (term.h). */
    while (tries < GOGO_MAX_MODULES && vm->mods[s % GOGO_MAX_MODULES]) { s++; tries++; }
    rc = CODE_TOO_MANY;
    if (tries == GOGO_MAX_MODULES) goto fail;
    s %= GOGO_MAX_MODULES;
    /* Where the code lives: platform memory (PSRAM) if given, else the
     * Bytes in place, else a VM copy. */
    rc = CODE_NO_MEMORY;
    if (!(flags & (MOD_OWN_COPY | MOD_OWN_PLATFORM))) {
        uint32_t *d = copy_code(vm, data, len, !in_place, &flags);
        if (d) {
            words = d;
            image_load(&img, words, len / 4u);   /* the same image: re-points img */
        } else if (!in_place) {
            goto fail;
        }
    }
    ModEntry *e = install(vm, s, &img, flags);
    if (!e) goto fail;
    vm->mod_next = (uint8_t)((s + 1u) % GOGO_MAX_MODULES);
    if (!(flags & (MOD_OWN_COPY | MOD_OWN_PLATFORM))) {
        bytes_msg_ref(vm, bytes);          /* the code stays put while loaded */
        e->bytes = bytes;
    }
    e->holders[0] = term_pid_slot(caller->id);
    *handle = module_handle(vm, s);
    return CODE_OK;
fail:
    free_code(vm, words, len, flags);
    return rc;
}

const Image *module_resolve(const Vm *vm, uint32_t slot, uint32_t gen) {
    const ModEntry *e = slot < GOGO_MAX_MODULES ? vm->mods[slot] : NULL;
    return (e && vm->mod_gen[slot] == gen) ? &e->img : NULL;
}

uint32_t module_handle(const Vm *vm, uint32_t slot) {
    return (slot << 16) | vm->mod_gen[slot];
}

int module_spawn(Vm *vm, Actor *caller, uint32_t handle, const uint8_t *entry, uint32_t len,
                 term arg, uint32_t flags, term *pid) {
    uint32_t slot = handle >> 16, f;
    const Image *img = module_resolve(vm, slot, handle & 0xFFFFu);
    if (!img || !image_export_find(img, entry, len, &f) || image_func_nparams(img, f) > 1u)
        return CODE_NOT_FOUND;
    if (!caller && (term_is_ref(arg) || term_is_handle(arg))) return CODE_BAD_ARG;
    if (image_func_nparams(img, f) > image_func_nregs(img, f)) return CODE_BAD_IMAGE;
    uint32_t r = vm_spawn_fn(vm, caller, (uint8_t)slot, f, arg, false, (uint16_t)(flags & 0xFFFFu),
                             caller && (flags & MOD_SPAWN_LINK), pid);
    if (r == R_NORMAL) return CODE_OK;
    if (r == R_BAD_MSG) return CODE_BAD_ARG;
    return r == R_OUT_OF_MEMORY ? CODE_NO_MEMORY : -(int)r;   /* R_BAD_CODE: a stale arg */
}

void module_actor_exit(Vm *vm, Actor *a) {
    for (uint32_t s = 0; s < GOGO_MAX_MODULES; s++) {
        ModEntry *e = vm->mods[s];
        if (!e) continue;
        if (s == a->mod && e->live) e->live--;
        drop(vm, s, term_pid_slot(a->id));
    }
}

int module_hold(Vm *vm, Actor *a, uint32_t handle) {
    uint32_t s = handle >> 16, k = MOD_HOLDERS;
    if (!module_resolve(vm, s, handle & 0xFFFFu)) return CODE_NOT_FOUND;
    ModEntry *e = vm->mods[s];
    uint16_t me = term_pid_slot(a->id);
    for (uint32_t i = 0; i < MOD_HOLDERS; i++) {
        if (e->holders[i] == me) return CODE_OK;
        if (e->holders[i] == MOD_NO_HOLDER) k = i;
    }
    if (k == MOD_HOLDERS) return CODE_TOO_MANY;
    e->holders[k] = me;
    return CODE_OK;
}

void module_release(Vm *vm, Actor *a, uint32_t handle) {
    uint32_t s = handle >> 16;
    if (module_resolve(vm, s, handle & 0xFFFFu)) drop(vm, s, term_pid_slot(a->id));
}

/* ---- B_CODE ---- */

/* Arguments after the op, per CODE_OP_*. */
static const uint8_t code_nargs[CODE_OP_MEM_USED + 1] = { 1, 3, 3, 4, 1, 1, 1, 0 };

/* The struct of a code.Module{H, Missing}, or NULL if t is none. */
static const uint32_t *module_obj(const Vm *vm, const Actor *a, term t) {
    const uint32_t *o = obj_of(vm, a, t);
    return o && o[0] == hdr_make(4, K_STRUCT) && o[1] == TYPE_CODE_MODULE ? o : NULL;
}

/* The handle a Module's H names: H - 1 for H in 1 .. 2^19 that is not slot
 * 0 (the boot image is never named through gogo/code); UINT32_MAX (no
 * module: NotFound) for every other int, boxed ones included. */
static uint32_t handle_of(const Vm *vm, const Actor *a, term t) {
    int32_t h;
    if (!int_val(vm, a, t, &h) || h <= 0x10000 || h > (int32_t)(GOGO_MAX_MODULES << 16)) return UINT32_MAX;
    return (uint32_t)h - 1u;
}

uint32_t module_builtin(Vm *vm, Actor *a, uint32_t ra, uint32_t rb, uint32_t rc, uint32_t nregs) {
    term *R = (term *)a->mem + a->fp;
    int32_t op, x;
    if (rc == 0 || !int_val(vm, a, R[rb], &op) || op < 0 || op > CODE_OP_MEM_USED ||
        rc != 1u + code_nargs[op] || (op <= CODE_OP_SPAWN_OPT && ra + 1u >= nregs))
        return R_BAD_CODE;
    const uint32_t *m = NULL;
    if (op != CODE_OP_LOAD && op != CODE_OP_MEM_USED &&
        (!(m = module_obj(vm, a, R[rb + 1])) || !int_val(vm, a, m[3], &x))) return R_BAD_CODE;
    uint32_t handle = m ? handle_of(vm, a, m[2]) : 0u;
    term res = T_NIL;
    int err = CODE_OK;
    switch (op) {
    case CODE_OP_LOAD: {
        /* Module{H, Missing} and a box for Missing, before the Bytes is
         * read (may GC); the box is garbage unless Missing needs it. */
        uint32_t o = heap_alloc(vm, a, 6, NULL, 0);
        if (o == UINT32_MAX) return R_OUT_OF_MEMORY;
        R = (term *)a->mem + a->fp;
        err = module_load(vm, a, R[rb + 1], &handle);
        uint32_t miss = err == CODE_UNSUPPORTED ? handle : 0u;   /* < 2^31: CAP_ANYPORT is never missing */
        a->mem[o] = hdr_make(4, K_STRUCT);
        a->mem[o + 1] = TYPE_CODE_MODULE;
        a->mem[o + 2] = term_from_int(err == CODE_OK ? (int32_t)handle + 1 : 0);
        a->mem[o + 4] = hdr_make(2, K_INT);
        a->mem[o + 5] = miss;
        a->mem[o + 3] = miss <= (uint32_t)TERM_INT_MAX ? term_from_int((int32_t)miss) : term_from_ref(o + 4);
        res = term_from_ref(o);
        break;
    }
    case CODE_OP_SPAWN: case CODE_OP_SPAWN_LINK: case CODE_OP_SPAWN_OPT: {
        const uint8_t *name;
        uint32_t len, flags = op == CODE_OP_SPAWN_LINK ? MOD_SPAWN_LINK : 0u;
        uint16_t words = 0;
        if (!name_bytes(vm, a, R[rb + 2], &name, &len) ||
            (op == CODE_OP_SPAWN_OPT && !vm_spawn_opt_words(vm, a, R[rb + 4], &words)))
            return R_BAD_CODE;
        err = module_spawn(vm, a, handle, name, len, R[rb + 3], flags | words, &res);
        if (err < 0) return (uint32_t)-err;
        if (err) res = T_ZEROPID;
        break;
    }
    case CODE_OP_HOLD:
        res = term_from_int(module_hold(vm, a, handle));
        break;
    case CODE_OP_RELEASE:
        module_release(vm, a, handle);
        break;
    case CODE_OP_MISSING:              /* the field's value (boxed again if it must be) */
        if (term_int_fits(x)) {
            res = term_from_int(x);
        } else {
            uint32_t o = heap_alloc(vm, a, 2, NULL, 0);   /* may GC: m is not used after */
            if (o == UINT32_MAX) return R_OUT_OF_MEMORY;
            R = (term *)a->mem + a->fp;
            a->mem[o] = hdr_make(2, K_INT);
            a->mem[o + 1] = (uint32_t)x;
            res = term_from_ref(o);
        }
        break;
    default: /* CODE_OP_MEM_USED (a small int: clamped, never boxed) */
        res = term_from_int(vm->mod_mem > (uint32_t)TERM_INT_MAX ? TERM_INT_MAX : (int32_t)vm->mod_mem);
        break;
    }
    if (op <= CODE_OP_SPAWN_OPT) R[ra + 1] = term_from_int(err);
    R[ra] = res;
    return R_NORMAL;
}

uint32_t module_mem_used(const Vm *vm) { return vm->mod_mem; }

const uint32_t *const_obj_of(const Vm *vm, term t) {
    const ModEntry *e = module_of_tag(vm, t);
    uint32_t off = term_const_off(t);
    if (!e || !image_const_start(&e->img, off)) return NULL;
    /* Every const object has at least two words and lies wholly inside the
     * area (image_load). */
    return e->img.consts + off;
}

void module_actor_enter(Vm *vm, Actor *a) {
    ModEntry *e = a->mod < GOGO_MAX_MODULES ? vm->mods[a->mod] : NULL;
    if (e) e->live++;
}
