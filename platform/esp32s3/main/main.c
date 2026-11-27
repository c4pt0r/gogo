/* gogo firmware entry: maps the image partition and runs the VM in a
 * FreeRTOS task pinned to CPU1. */
#include "sdkconfig.h"
#include "image.h"
#include "host.h"
#include "actor.h"
#include "interp.h"
#include "module.h"
#include "config.h"
#include "platform_esp.h"
#include "board.h"
#if CONFIG_GOGO_NET
#include "net_int.h"
#endif
#include "vfs_int.h"
#include "input_int.h"
#include "board_main.h"
#include "params.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#ifndef GOGO_FIRMWARE_VERSION
#define GOGO_FIRMWARE_VERSION "dev"
#endif

#define VM_STACK_BYTES 16384u
#define VM_PRIORITY    5

static Vm vm;      /* static: too big for a task stack */
static VfsState vfs_state;     /* its vfs, input and net state (runtime/ keeps none) */
static InputState input_state;
#if CONFIG_GOGO_NET
NetState esp_net_state;        /* shared with wifi.c and config_esp.c (platform_esp.h) */
#endif

void main_esp_usage(uint32_t *bytes_used, uint32_t *bytes_max, uint32_t *ports_used, uint32_t *ports_max) {
    const volatile Vm *v = &vm;
    uint32_t n = 0, have = v->port_n;   /* chunks are only ever added */
    for (uint32_t i = 0; i < have; i++) n += v->port_chunk[i / PORT_CHUNK][i % PORT_CHUNK].drv != NULL;
    *bytes_used = v->bytes_used;
    *bytes_max = v->bytes_cap;
    *ports_used = n;
    *ports_max = v->port_max;
}

void main_esp_params(char *buf, size_t cap) {
    const volatile Vm *v = &vm;
    VmParams p;
    if (v->table_cap) {   /* booted: what the Vm runs with */
        p.actor_memory = (uint32_t)v->actor_words * 4u;
        p.memory = v->va.limit;
        p.max_actors = v->table_cap;
        p.max_ports = v->port_max;
        p.max_links = v->max_links;
        p.code_mem = v->mod_cap;
        p.bytes_handles = v->bytes_cap;
        p.bytes_per_actor = v->bytes_per_actor;
    } else {              /* no image (yet): what a boot would resolve, quietly */
        gogo_host h = esp_host;
        h.write = NULL;
        vm_params_default(&p);
        vm_params_resolve(&h, NULL, NULL, &p);
    }
    board_params_format(&p, buf, cap);
}

#if CONFIG_GOGO_NET
/* Everything attached, before the run: the Wi-Fi driver's heap and the
 * stack's fixed state, on the serial log. */
static void ready(Vm *v, void *ctx) {
    (void)v;
    (void)ctx;
    wifi_esp_report_heap();
    platform_esp_log("gogo: net stack fixed state %lu bytes", (unsigned long)net_fixed_bytes());
}
#endif

static void vm_task(void *arg) {
    (void)arg;
    platform_esp_init();
    console_esp_start(); /* LCD rail (StickS3 M5PM1), ST7789, render task */
    /* `gogo config` works even without (or with a bad) image. */
    (void)config_esp_init();
    config_esp_start_serial();
    const uint32_t *words;
    uint32_t nwords;
    int run_rc = 0;
    /* Wi-Fi (driver only, no esp_netif/lwIP) and gogo's net stack. */
#if CONFIG_GOGO_NET
    wifi_esp_start();
#endif
    /* Storage: /flash (LittleFS on `gogofs`; formats on first boot) and
     * the B_FS builtin; vfs_init below mounts the board's file systems.
     * Without /flash every fs path under it is NotMounted. */
    (void)fs_lfs_mount();
#if HAS_SD && CONFIG_GOGO_SD
    /* /sd (FatFs on the microSD card): mounted by the first call. */
    fs_fat_init();
    platform_esp_log("gogo: fs state %lu bytes", (unsigned long)(fs_lfs_state_bytes() + fs_fat_state_bytes()));
#else
    platform_esp_log("gogo: fs state %lu bytes", (unsigned long)fs_lfs_state_bytes());
#endif
    if (image_part_map(&words, &nwords) != 0) platform_esp_idle();

    /* Internal RAM left for the arena once the console (font tables) and
     * the Wi-Fi driver are up. */
    platform_esp_say("gogo %s (image format %u.%u): image %lu bytes, arena %lu bytes, free RAM %lu (largest %lu)",
                     GOGO_FIRMWARE_VERSION, IMAGE_MAJOR, IMAGE_MINOR, (unsigned long)nwords * 4u, (unsigned long)platform_esp_arena_cap(),
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    /* The VM's parameters (params.h): the config keys over the build's
     * defaults. On this task's stack, which never returns: no static RAM. */
    VmParams params;
    vm_params_default(&params);
    vm_params_resolve(&esp_host, NULL, NULL, &params);
    /* The one boot path (runtime/board): vm_init, vm_boot (the image is
     * module 0, its requirements checked), the net stack, the file
     * systems and the input source, then vm_run. */
    BoardRun run = {
        .vm = &vm, .params = &params, .vfs = &vfs_state, .input = &input_state,
#if CONFIG_GOGO_NET
        .net = &esp_net_state, .net_install = net_install, .ready = ready,
#endif
    };
    int rc = gogo_board_main(&esp_board, &esp_host, &run, words, nwords, &run_rc);
    if (rc == IMG_ERR_VERSION) {
        platform_esp_say("gogo: unsupported image format %u.%u (this firmware runs %u.0 to %u.%u)",
                         (unsigned)(words[1] >> 16), (unsigned)(words[1] & 0xFFFFu), IMAGE_MAJOR, IMAGE_MAJOR,
                         IMAGE_MINOR);
        platform_esp_idle();
    }
    if (rc < 0) {
        platform_esp_say("gogo: bad image (%s)", image_err_str(rc));
        platform_esp_idle();
    }
    if (rc == VM_BOOT_REFUSED) platform_esp_idle();   /* the line is written */
    if (rc != VM_BOOT_OK) {
        platform_esp_say("gogo: out of memory");
        platform_esp_idle();
    }
    rc = run_rc;
    console_esp_report();
    platform_esp_report_refusal();
    platform_esp_say("gogo: vm exited (%d), arena used %lu", rc, (unsigned long)platform_esp_arena_used());
    platform_esp_idle();
}

void app_main(void) {
    xTaskCreatePinnedToCore(vm_task, "gogo_vm", VM_STACK_BYTES, NULL, VM_PRIORITY, NULL, 1);
}
