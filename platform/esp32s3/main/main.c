/* gogo firmware entry: maps the image partition and runs the VM in a
 * FreeRTOS task pinned to CPU1. */
#include "image.h"
#include "actor.h"
#include "interp.h"
#include "config.h"
#include "platform_esp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"

#ifndef GOGO_VERSION
#define GOGO_VERSION "dev"
#endif

#define VM_STACK_BYTES 16384u
#define VM_PRIORITY    5

static Vm vm;      /* static: too big for a task stack */

static void vm_task(void *arg) {
    (void)arg;
    platform_esp_init();
    console_esp_start(); /* M5PM1 LCD rail, ST7789, render task */
    const uint32_t *words;
    uint32_t nwords;
    if (image_part_map(&words, &nwords) != 0) platform_esp_idle();

    Image img;
    int rc = image_load(&img, words, nwords);
    if (rc != IMG_OK) {
        static const char *const msgs[] = {"ok", "bad magic", "unsupported version", "truncated image", "bad offsets"};
        platform_esp_say("gogo: bad image (%s)", (rc <= 0 && rc >= -4) ? msgs[-rc] : "invalid image");
        platform_esp_idle();
    }
    /* Internal RAM left for the arena once the console (font tables) is up. */
    platform_esp_say("gogo %s: image %lu bytes, arena %lu bytes, free RAM %lu (largest %lu)", GOGO_VERSION,
                     (unsigned long)nwords * 4u, (unsigned long)platform_esp_arena_cap(),
                     (unsigned long)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                     (unsigned long)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (vm_init(&vm, &img, (uint16_t)GOGO_TABLE_CAP) != 0) {
        platform_esp_say("gogo: out of memory");
        platform_esp_idle();
    }
    rc = vm_run(&vm);
    console_esp_report();
    platform_esp_report_refusal();
    platform_esp_say("gogo: vm exited (%d), arena used %lu", rc, (unsigned long)platform_esp_arena_used());
    platform_esp_idle();
}

void app_main(void) {
    xTaskCreatePinnedToCore(vm_task, "gogo_vm", VM_STACK_BYTES, NULL, VM_PRIORITY, NULL, 1);
}
