/* The shared NimBLE host (ble_host.h): a minimal reference-counted
 * start / stop around nimble_port_init and the host task. */
#include "ble_host.h"
#include "platform_esp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "host/ble_hs.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

#define SYNC_TIMEOUT_MS 3000

static SemaphoreHandle_t lock;
static StaticSemaphore_t lock_buf;
static portMUX_TYPE lock_init = portMUX_INITIALIZER_UNLOCKED;
static unsigned refs;
static volatile bool synced;

static void on_sync(void) { synced = true; }

static void on_reset(int reason) {
    synced = false;
    platform_esp_log("gogo: ble host reset (%d)", reason);
}

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();   /* returns after nimble_port_stop */
    nimble_port_freertos_deinit();
}

static void take(void) {
    taskENTER_CRITICAL(&lock_init);
    if (!lock) lock = xSemaphoreCreateMutexStatic(&lock_buf);
    taskEXIT_CRITICAL(&lock_init);
    xSemaphoreTake(lock, portMAX_DELAY);
}

static void stop(void) {
    if (nimble_port_stop() == 0) (void)nimble_port_deinit();
    synced = false;
}

int ble_host_acquire(void) {
    take();
    int rc = 0;
    if (refs == 0) {
        synced = false;
        if (nimble_port_init() != ESP_OK) {
            rc = -1;
        } else {
            ble_hs_cfg.sync_cb = on_sync;
            ble_hs_cfg.reset_cb = on_reset;
            nimble_port_freertos_init(host_task);
            for (int ms = 0; !synced && ms < SYNC_TIMEOUT_MS; ms += 10) vTaskDelay(pdMS_TO_TICKS(10));
            if (!synced) {
                platform_esp_log("gogo: ble host did not sync");
                stop();
                rc = -1;
            }
        }
    }
    if (rc == 0) refs++;
    xSemaphoreGive(lock);
    return rc;
}

void ble_host_release(void) {
    take();
    if (refs > 0 && --refs == 0) stop();
    xSemaphoreGive(lock);
}
