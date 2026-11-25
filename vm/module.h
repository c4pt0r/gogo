/* The module table (hot loading, design §5.2-§5.5).
 *
 * Every image is a module. The VM holds at most GOGO_MAX_MODULES of them in
 * Vm.mods; slot 0 is the boot image, loaded by vm_boot (module_load_static)
 * and started like any other module: module_spawn of its "main" export.
 * It differs only in where its code lives (flash, or the caller's buffer:
 * not counted against the code memory limit) and in being held by the VM
 * (MOD_VM_HELD), so it is never unloaded and its generation stays 0.
 *
 * Entry lifetime: a module stays loaded while an actor runs its code (live)
 * or an actor holds it (holders: module_load makes the loader one;
 * code.Hold adds more, code.Release ends one). module_actor_exit, run for
 * every freed actor, drops both; when neither is left the entry is freed, the slot's generation is
 * bumped and the code memory released. Terms that still name the module
 * (module-tagged const refs, closures, structs: term.h) then fail
 * module_of_tag and their use is `badcode`.
 *
 * Handles: slot << 16 | generation (16 bits); a code.Module{H, Missing}
 * carries handle + 1 in H (module_builtin). A term tag
 * carries only the low 8 bits of the generation (term.h); handles compare
 * all 16 (module_resolve). module_load picks free slots round-robin
 * (Vm.mod_next), spreading reloads over every slot: a stale term aliases a
 * newer module only after 256 loads into its own slot (term.h). */
#ifndef GOGO_MODULE_H
#define GOGO_MODULE_H

#include <stdint.h>
#include <stdbool.h>
#include "term.h"
#include "image.h"
#include "actor.h"
#include "config.h"

/* code.Err values (gogo/code, Task 4). */
enum {
    CODE_OK = 0,
    CODE_BAD_IMAGE = 1,     /* not a valid image (image_load), or unusable bytes */
    CODE_UNSUPPORTED = 2,   /* needs a capability the firmware lacks */
    CODE_NO_MEMORY = 3,
    CODE_NOT_FOUND = 4,     /* no such module (stale handle) or entry */
    CODE_BAD_ARG = 5,
    CODE_TOO_MANY = 6       /* the module table is full */
};

#define MOD_HOLDERS 4u

/* ModEntry.flags. Where a counted module's code lives: in its Bytes (bytes
 * != T_NIL), or in a copy it owns (MOD_OWN_COPY: VM memory, va_alloc;
 * MOD_OWN_PLATFORM: platform_code_alloc), freed when it unloads. */
enum {
    MOD_VM_HELD = 1,        /* the boot image: held by the VM, never unloaded */
    MOD_COUNTED = 2,        /* its code bytes count in Vm.mod_mem */
    MOD_OWN_COPY = 4,
    MOD_OWN_PLATFORM = 8
};

/* B_CODE op codes (the first argument; compiler codeOps, cross-checked by
 * TestCodeMatchC). Two results (value, code.Err) for LOAD..SPAWN_OPT. */
enum {
    CODE_OP_LOAD = 0,       /* Bytes -> Module, Err */
    CODE_OP_SPAWN = 1,      /* Module, entry string, arg -> PID, Err */
    CODE_OP_SPAWN_LINK = 2, /* likewise, linked to the caller */
    CODE_OP_SPAWN_OPT = 3,  /* Module, entry, arg, actor.Options -> PID, Err */
    CODE_OP_HOLD = 4,       /* Module -> Err */
    CODE_OP_RELEASE = 5,    /* Module -> nil */
    CODE_OP_MISSING = 6,    /* Module -> int */
    CODE_OP_MEM_USED = 7    /* -> int */
};

#define MOD_NO_HOLDER 0xFFFFu   /* above every actor slot (PORT_SLOT_BASE) */

/* One loaded module, allocated (va_alloc) when it loads, freed when it
 * unloads: 64 B on 32-bit (check32.c), 48 of them the Image, kept whole
 * because the interpreter and the image_* helpers read it in place. Its
 * code memory is the image itself (img.funcs - IMAGE_HEADER_WORDS up to the
 * end of the exports: module_code_words); its generation is
 * Vm.mod_gen[slot]. */
struct ModEntry {
    Image img;              /* validated header; pointers into the code memory */
    term bytes;             /* the off-heap Bytes it lives in (one reference), or T_NIL */
    /* Actor slots (term_pid_slot) of its holders, MOD_NO_HOLDER = free.
     * A slot is enough: module_actor_exit drops an actor's holds before
     * its slot can be reused. */
    uint16_t holders[MOD_HOLDERS];
    uint16_t live;          /* actors running its code */
    uint8_t flags;
};

/* Spawn flags: the low 16 bits are the child's memory limit in words (0:
 * the default, as SpawnOpt's Options); MOD_SPAWN_LINK links it to the
 * caller (SpawnLink). */
#define MOD_SPAWN_LINK 0x10000u

/* vm_boot results; a negative value is an IMG_ERR_* (image_err_str). */
enum {
    VM_BOOT_OK = 0,
    VM_BOOT_NO_MEMORY = 1,  /* nothing written: the platform reports it */
    VM_BOOT_REFUSED = 2     /* the refusal line is already on stderr */
};

/* The boot path every platform calls: vm_init, then the boot image into
 * slot 0 (module_load_static). Then vm_run starts module 0's "main"
 * through module_spawn and runs the scheduler. On a refusal the actor
 * table is freed again. */
int vm_boot(Vm *vm, const uint32_t *words, uint32_t n_words, uint16_t table_cap);

/* The boot image into slot 0: image_load, then the capability check
 * (Task 2: on a missing capability the refusal line goes to stderr in one
 * write), then module_install_static. Returns IMG_OK, a negative IMG_ERR_*,
 * CODE_UNSUPPORTED, CODE_NO_MEMORY or CODE_TOO_MANY (slot 0 in use). The
 * words are used in place, never copied, and must outlive the VM. */
int module_load_static(Vm *vm, const uint32_t *words, uint32_t n_words);

/* Installs an already loaded image as slot 0, VM-held and not counted
 * (module_load_static's last step; vm_init's test convenience). No caps
 * check: that is module_load_static's. CODE_OK, CODE_TOO_MANY or
 * CODE_NO_MEMORY. */
int module_install_static(Vm *vm, const Image *img);

/* Reads the code memory limit, config key code.mem (bytes, optional K or M
 * suffix), into vm->mod_cap; unset: GOGO_CODE_MEM_DEFAULT, which vm_init
 * sets; a bad value says so on stderr and keeps the default. vm_boot runs
 * it (VM start). */
void module_config(Vm *vm);

/* code.Load: loads the image in Bytes `bytes` (as caller sees it: off-heap,
 * heap or const) into a free slot and makes caller a holder. Checks, in
 * order: the bytes are exactly one valid image (image_load, and no
 * trailing words) -> CODE_BAD_IMAGE; its requires word against
 * platform_caps -> CODE_UNSUPPORTED, with *handle = the missing CAP_* bits;
 * the code memory limit (Vm.mod_cap, counting the image's bytes) ->
 * CODE_NO_MEMORY; a free slot -> CODE_TOO_MANY. Unaligned bytes are
 * validated as an aligned copy (whose allocation may fail: CODE_NO_MEMORY).
 * Bytes longer than Vm.mod_cap are CODE_NO_MEMORY at once, before any
 * copy. Where the code then lives: platform_code_alloc memory if the
 * platform gives some, aligned (a copy; unaligned bytes are validated
 * there, not copied twice); else the Bytes in place if it is a whole
 * off-heap root at an aligned address (the module keeps one reference on
 * it until it unloads: what it pins is what is counted); else a copy in VM
 * memory (heap or const Bytes, any slice). A copy is made once, counted like in-place code, and
 * the Bytes is not kept. CODE_BAD_ARG if bytes is no Bytes. On CODE_OK
 * *handle is the module handle; nothing is kept on any error. */
int module_load(Vm *vm, Actor *caller, term bytes, uint32_t *handle);

/* code.Hold: makes a a holder of module handle (once per actor: holding
 * again is CODE_OK). CODE_NOT_FOUND if the module is gone, CODE_TOO_MANY
 * if it has MOD_HOLDERS holders already. */
int module_hold(Vm *vm, Actor *a, uint32_t handle);

/* code.Release: ends a's hold on module handle (nothing if a holds none or
 * the module is gone); unloads it if that leaves neither holder nor actor. */
void module_release(Vm *vm, Actor *a, uint32_t handle);

/* B_CODE (the interpreter's BUILTIN, same contract as vm_fs_builtin): ra,
 * rb, rc and the frame's register count; R[rb] is the CODE_OP_*, the
 * operation's arguments follow. Two-result ops check ra+1 themselves.
 * Returns R_NORMAL or the caller's crash reason (R_BAD_CODE for a bad op,
 * arity or operand, R_OUT_OF_MEMORY when the Module struct does not fit);
 * may GC.
 *
 * code.Module{H, Missing} (reserved type 34): Load gives H = handle + 1
 * for a loaded module and H = 0 on every error, and Missing = the missing
 * CAP_* bits after Unsupported (boxed if above a small int), else 0. Only
 * H in 0x10001 .. GOGO_MAX_MODULES << 16 can name a module, never slot 0
 * (the boot image is not reachable through gogo/code); every other int,
 * forged or stale, is NotFound for Spawn and Hold and nothing for Release.
 * code.Missing returns the Missing field as it is. */
uint32_t module_builtin(Vm *vm, Actor *a, uint32_t ra, uint32_t rb, uint32_t rc, uint32_t nregs);

/* Starts the export `entry` (len bytes) of the module `handle` in a new
 * actor of that module. CODE_BAD_IMAGE if that function's header has more
 * parameters than registers (image_load does not check it; the compiler
 * never emits it). The entry takes no parameter or one (arg, copied
 * out of caller's heap like a message; with no caller it must be an
 * immediate). flags: MOD_SPAWN_LINK and a memory limit (above). Returns a
 * code.Err: CODE_NOT_FOUND (stale handle, no such export, or an export of
 * another arity), CODE_BAD_ARG (also an arg the cross-module rules refuse,
 * msg_build's R_BAD_MSG: no actor, no crash), CODE_NO_MEMORY; on CODE_OK
 * sets *pid. -R_BAD_CODE if arg names a module that is gone: the caller's
 * badcode, as everywhere a stale value is used. */
int module_spawn(Vm *vm, Actor *caller, uint32_t handle, const uint8_t *entry, uint32_t len,
                 term arg, uint32_t flags, term *pid);

/* The image of module (slot, generation), or NULL if that slot is free or
 * holds another generation (the module was unloaded). */
const Image *module_resolve(const Vm *vm, uint32_t slot, uint32_t gen);

/* Words of the image a module runs from (header to the end of the
 * exports): what a counted module is charged. */
uint32_t module_code_words(const ModEntry *e);

/* The handle of the module now in slot (slot << 16 | generation). */
uint32_t module_handle(const Vm *vm, uint32_t slot);

/* Run for every new actor (actor_new_in): counts it in its module's live
 * actors. */
void module_actor_enter(Vm *vm, Actor *a);

/* Run for every actor that is freed (actor_free): leaves its module's live
 * count, ends every hold it had, and unloads each module that is left
 * with neither. */
void module_actor_exit(Vm *vm, Actor *a);

/* Code bytes of the loaded modules that count against the code memory
 * limit (the boot image does not). */
uint32_t module_mem_used(const Vm *vm);

/* The tag (term.h) of the module now in slot. */
static inline uint32_t module_tag(const Vm *vm, uint32_t slot) {
    return mod_tag_make(slot, vm->mod_gen[slot]);
}

/* The module a tagged word (const ref, closure or struct type word) names,
 * or NULL if it is gone: the hot path of every module-tagged access. */
static inline const ModEntry *module_of_tag(const Vm *vm, uint32_t w) {
    uint32_t slot = mod_tag_slot(w);
    if (slot >= GOGO_MAX_MODULES || (vm->mod_gen[slot] & MOD_GEN_MASK) != mod_tag_gen(w)) return NULL;
    return vm->mods[slot];
}

/* The object const ref t names: through the module table (its tag): NULL
 * once its module is gone, and NULL unless it names an object start in
 * that module's const map (a ref kept across 256 reloads of its slot names
 * the newest module there: it may only reach a validated object, never
 * forge one). obj_of's const half (heap.h). */
const uint32_t *const_obj_of(const Vm *vm, term t);

#endif
