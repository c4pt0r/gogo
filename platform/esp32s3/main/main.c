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
#if CONFIG_GOGO_BLE_PROV
#include "ble_prov.h"
#endif

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
#include "esp_freertos_hooks.h"
#include "esp_system.h"
/* IDF's watchdog LL headers have unused parameters; this component
 * builds with -Werror=unused-parameter. */
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "hal/wdt_hal.h"
#pragma GCC diagnostic pop
#include "soc/rtc.h"

#ifndef GOGO_FIRMWARE_VERSION
#define GOGO_FIRMWARE_VERSION "dev"
#endif

#define VM_STACK_BYTES 16384u
#define VM_PRIORITY    5
/* The RTC watchdog's timeout (hang_guard_start). */
#define HANG_RESET_S   15u

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
        vm_params_resolve(&h, NULL, &p);
    }
    vm_params_format(&p, buf, cap);
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

/* ---- a hang is a reset ----
 *
 * The interrupt watchdog (800 ms, both CPUs) and the task watchdog (CPU0's
 * idle task, 5 s; CONFIG_ESP_TASK_WDT_PANIC) panic with a backtrace on the
 * console and reboot. Both run on the main clock and need a CPU to run
 * the panic handler. The RTC watchdog is the last resort: it counts on
 * the RTC's own slow clock, so it fires when a CPU, the clocks or the
 * panic itself are stuck. CPU0's idle task feeds it, about once a
 * second; after HANG_RESET_S without that it resets the digital system
 * (CPUs, peripherals, the USB-Serial-JTAG link), and HANG_RESET_S later
 * the RTC as well. The panic handler and esp_restart reprogram it for
 * their own use. */
static bool hang_guard_feed(void) {
    static TickType_t last;
    TickType_t now = xTaskGetTickCount();
    if (now - last >= pdMS_TO_TICKS(1000)) {
        last = now;
        wdt_hal_context_t w = RWDT_HAL_CONTEXT_DEFAULT();
        wdt_hal_write_protect_disable(&w);
        wdt_hal_feed(&w);
        wdt_hal_write_protect_enable(&w);
    }
    return true;
}

static void hang_guard_start(void) {
    wdt_hal_context_t w = RWDT_HAL_CONTEXT_DEFAULT();
    uint32_t ticks = HANG_RESET_S * rtc_clk_slow_freq_get_hz();
    wdt_hal_init(&w, WDT_RWDT, 0, false);
    wdt_hal_write_protect_disable(&w);
    wdt_hal_config_stage(&w, WDT_STAGE0, ticks, WDT_STAGE_ACTION_RESET_SYSTEM);
    wdt_hal_config_stage(&w, WDT_STAGE1, ticks, WDT_STAGE_ACTION_RESET_RTC);
    wdt_hal_enable(&w);
    wdt_hal_write_protect_enable(&w);
    if (esp_register_freertos_idle_hook_for_cpu(hang_guard_feed, 0) != ESP_OK) {
        wdt_hal_write_protect_disable(&w);   /* nobody would feed it */
        wdt_hal_disable(&w);
        wdt_hal_write_protect_enable(&w);
    }
}

/* Says why the board last reset, when it was a crash, a watchdog or a
 * brownout (screen and serial): a hang that the watchdogs turned into a
 * reset stays visible. */
static void report_reset(void) {
    const char *why = NULL;
    switch (esp_reset_reason()) {
    case ESP_RST_PANIC: why = "crash (panic)"; break;
    case ESP_RST_INT_WDT: why = "interrupt watchdog"; break;
    case ESP_RST_TASK_WDT: why = "task watchdog"; break;
    case ESP_RST_WDT: why = "watchdog (a hang)"; break;
    case ESP_RST_BROWNOUT: why = "brownout (low supply voltage)"; break;
    default: break;
    }
    if (why) platform_esp_say("gogo: last reset: %s", why);
}

static void vm_task(void *arg) {
    (void)arg;
    platform_esp_init();
    console_esp_start(); /* LCD rail (StickS3 M5PM1), ST7789, render task */
    report_reset();
    /* `gogo config` works even without (or with a bad) image. */
    (void)config_esp_init();
    config_esp_start_serial();
#if CONFIG_GOGO_BLE_PROV
    /* The Wi-Fi setup key held: provisioning over BLE, then a restart. */
    ble_prov_boot();
#endif
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
    vm_params_resolve(&esp_host, NULL, &params);
    /* The one boot path (runtime/board): vm_init, vm_boot (the image is
     * module 0, its requirements checked), the net stack, the file
     * systems and the input source, then vm_run. */
    BoardRun run = {
        .vm = &vm, .params = &params, .vfs = &vfs_state, .input = &input_state,
#if CONFIG_GOGO_BOARD_PORTS
        .ports_host = &esp_ports_host, .ports_expected = CONFIG_GOGO_BOARD_PORT_NAMES,
#endif
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
    hang_guard_start();
    xTaskCreatePinnedToCore(vm_task, "gogo_vm", VM_STACK_BYTES, NULL, VM_PRIORITY, NULL, 1);
}
