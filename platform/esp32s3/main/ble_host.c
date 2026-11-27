/* The NimBLE host's lifetime (ble_host.h), and the weak default of
 * ble_prov_active (ble_prov.h). */
#include "ble_host.h"
#include "ble_prov.h"
#include "platform_esp.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"

static StaticSemaphore_t mu_buf, sync_buf;
static SemaphoreHandle_t mu, synced;
static unsigned holders;
static int ready;                  /* 0, 1 (being created), 2 (mu and synced exist) */

/* No provisioning in this firmware: never on air. */
__attribute__((weak)) bool ble_prov_active(void) { return false; }

static volatile bool up;            /* the first sync of this start has happened */
static void (*volatile resync)(void);

/* The first sync wakes ble_host_acquire; a later one follows a host reset
 * (a controller fault): GAP state such as advertising is gone, and the
 * holder's resync callback restores it (host task). */
static void on_sync(void) {
    if (!up) {
        up = true;
        xSemaphoreGive(synced);
        return;
    }
    platform_esp_log("gogo: ble: host synced again after a reset");
    void (*cb)(void) = resync;
    if (cb) cb();
}

void ble_host_on_resync(void (*cb)(void)) { resync = cb; }

static void on_reset(int reason) { platform_esp_log("gogo: ble: host reset (%d)", reason); }

static void host_task(void *arg) {
    (void)arg;
    nimble_port_run();               /* until nimble_port_stop */
    nimble_port_freertos_deinit();   /* deletes this task */
}

/* Creates mu and synced once, whichever task comes first; the others
 * wait for it. */
static void init_once(void) {
    int z = 0;
    if (__atomic_compare_exchange_n(&ready, &z, 1, false, __ATOMIC_ACQ_REL, __ATOMIC_ACQUIRE)) {
        mu = xSemaphoreCreateMutexStatic(&mu_buf);
        synced = xSemaphoreCreateBinaryStatic(&sync_buf);
        __atomic_store_n(&ready, 2, __ATOMIC_RELEASE);
    }
    while (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != 2) vTaskDelay(1);
}

int ble_host_acquire(void) {
    init_once();
    xSemaphoreTake(mu, portMAX_DELAY);
    int r = 0;
    if (holders == 0) {
        (void)xSemaphoreTake(synced, 0);
        up = false;
        if (nimble_port_init() != ESP_OK) {
            r = -1;
        } else {
            ble_hs_cfg.sync_cb = on_sync;
            ble_hs_cfg.reset_cb = on_reset;
            nimble_port_freertos_init(host_task);
            if (xSemaphoreTake(synced, pdMS_TO_TICKS(BLE_HOST_SYNC_MS)) != pdTRUE) {
                platform_esp_log("gogo: ble: the host did not sync with the controller");
                if (nimble_port_stop() != 0) platform_esp_log("gogo: ble: the host did not stop");
                nimble_port_deinit();
                r = -1;
            }
        }
    }
    if (r == 0) holders++;
    xSemaphoreGive(mu);
    return r;
}

void ble_host_release(void) {
    if (__atomic_load_n(&ready, __ATOMIC_ACQUIRE) != 2) return;   /* never acquired */
    xSemaphoreTake(mu, portMAX_DELAY);
    if (holders && --holders == 0) {
        resync = NULL;
        /* waits for the phone to acknowledge the disconnect, at most
         * BT_NIMBLE_HS_STOP_TIMEOUT_MS (2 s) */
        int rc = nimble_port_stop();
        if (rc != 0) platform_esp_log("gogo: ble: the host did not stop cleanly (%d)", rc);
        nimble_port_deinit();
    }
    xSemaphoreGive(mu);
}
