/* The "ble" port's radio on the ESP32-S3 (GOGO_BLE): a BleHal over NimBLE
 * (runtime/ble/ble.h). Built with CONFIG_GOGO_BLE only.
 *
 * start (the VM task, at port.Open) acquires the NimBLE host (ble_host.h),
 * builds the GATT server from the Config in fixed tables below, and
 * advertises: flags and the name, the first service's UUID in the scan
 * response. The host task (core 0) runs the GAP and GATT callbacks: they
 * post events to the driver's queue (ble_post, under `mu`), wake the VM
 * task, and answer a phone's reads from the values SetValue stored here,
 * so a read never wakes the VM. One connection: NimBLE stops advertising
 * when a central connects, and this restarts it when it leaves. notify
 * runs on the VM task (ble_gatts_notify_custom takes the host's lock); a
 * mbuf the pool cannot give is BLE_AGAIN, tried again by the driver, and
 * so is a notify that would leave its pool fewer than BLE_MSYS_SPARE
 * blocks (ble.h; the host's replies need them).
 * stop releases the host, which ends the connection and the advertising. */
#include "sdkconfig.h"
#include "ble.h"
#include "ble_host.h"
#include "ble_prov.h"
#include "platform_esp.h"
#include "gogo_errors.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
#include "host/ble_hs.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "os/os_mbuf.h"


static BleBoard board;
static StaticSemaphore_t mu_buf;
static SemaphoreHandle_t mu;     /* board's queue, the values, conn, running */

/* The GATT tables NimBLE keeps pointers into while the server exists. */
static struct ble_gatt_svc_def svcs[BLE_MAX_SERVICES + 1u];
static struct ble_gatt_chr_def chrs[BLE_MAX_CHARS + BLE_MAX_SERVICES];
static ble_uuid_any_t svc_uuid[BLE_MAX_SERVICES], chr_uuid[BLE_MAX_CHARS];
static uint16_t val_handle[BLE_MAX_CHARS];

static BleConfig cfg;            /* start's copy (the host task reads it) */
static uint8_t *val[BLE_MAX_CHARS];
static uint16_t vlen[BLE_MAX_CHARS];
static uint16_t conn = BLE_HS_CONN_HANDLE_NONE;
static bool running;
static uint8_t own_addr;
static uint8_t wbuf[BLE_VALUE_MAX];   /* a write, flattened (host task only) */

/* The driver's first lock (in port.Open, on the VM task, before any
 * start) creates mu: only the VM task calls this before the host task
 * can post, so there is no race to create it. */
static void hal_lock(void *ctx) {
    (void)ctx;
    if (!mu) mu = xSemaphoreCreateMutexStatic(&mu_buf);
    xSemaphoreTake(mu, portMAX_DELAY);
}
static void hal_unlock(void *ctx) { (void)ctx; xSemaphoreGive(mu); }

/* Queues an event (host task) and wakes the VM task; false if it did not fit. */
static bool post(uint32_t ev, uint32_t arg, const uint8_t *d, uint32_t len) {
    xSemaphoreTake(mu, portMAX_DELAY);
    bool ok = ble_post(&board, ev, arg, d, len);
    xSemaphoreGive(mu);
    if (ok) platform_notify();
    return ok;
}

static void to_uuid(const BleUuid *u, ble_uuid_any_t *out) {
    memset(out, 0, sizeof *out);
    if (u->len == 2u) {
        out->u16.u.type = BLE_UUID_TYPE_16;
        out->u16.value = (uint16_t)(u->b[0] | u->b[1] << 8);
    } else {
        out->u128.u.type = BLE_UUID_TYPE_128;
        memcpy(out->u128.value, u->b, 16);
    }
}

/* ---- GATT ---- */

static int access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg) {
    (void)conn_handle;
    (void)attr_handle;
    uint32_t k = (uint32_t)(uintptr_t)arg;
    if (k >= cfg.nchr) return BLE_ATT_ERR_UNLIKELY;
    if (ctxt->op == BLE_GATT_ACCESS_OP_READ_CHR) {
        xSemaphoreTake(mu, portMAX_DELAY);
        int rc = vlen[k] ? os_mbuf_append(ctxt->om, val[k], vlen[k]) : 0;
        xSemaphoreGive(mu);
        return rc ? BLE_ATT_ERR_INSUFFICIENT_RES : 0;
    }
    if (ctxt->op == BLE_GATT_ACCESS_OP_WRITE_CHR) {
        uint16_t n = 0;
        if (OS_MBUF_PKTLEN(ctxt->om) > BLE_VALUE_MAX) return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        if (ble_hs_mbuf_to_flat(ctxt->om, wbuf, sizeof wbuf, &n) != 0) return BLE_ATT_ERR_UNLIKELY;
        return post(BLE_EV_WRITTEN, k, wbuf, n) ? 0 : BLE_ATT_ERR_INSUFFICIENT_RES;
    }
    return BLE_ATT_ERR_UNLIKELY;
}

/* The tables from cfg; 0 or a NimBLE error. */
static int build_gatt(void) {
    memset(svcs, 0, sizeof svcs);
    memset(chrs, 0, sizeof chrs);
    uint32_t c = 0;
    for (uint32_t s = 0; s < cfg.nsvc; s++) {
        to_uuid(&cfg.svc[s], &svc_uuid[s]);
        svcs[s].type = BLE_GATT_SVC_TYPE_PRIMARY;
        svcs[s].uuid = &svc_uuid[s].u;
        svcs[s].characteristics = &chrs[c];
        for (uint32_t k = cfg.first[s]; k < cfg.first[s + 1u]; k++, c++) {
            uint8_t f = cfg.flags[k];
            to_uuid(&cfg.chr[k], &chr_uuid[k]);
            chrs[c].uuid = &chr_uuid[k].u;
            chrs[c].access_cb = access;
            chrs[c].arg = (void *)(uintptr_t)k;
            chrs[c].val_handle = &val_handle[k];
            chrs[c].flags = (ble_gatt_chr_flags)((f & BLE_F_READ ? BLE_GATT_CHR_F_READ : 0) |
                                                 (f & BLE_F_WRITE ? BLE_GATT_CHR_F_WRITE : 0) |
                                                 (f & BLE_F_WRITE_NR ? BLE_GATT_CHR_F_WRITE_NO_RSP : 0) |
                                                 (f & BLE_F_NOTIFY ? BLE_GATT_CHR_F_NOTIFY : 0));
        }
        c++;                             /* the zeroed terminator */
    }
    int rc = ble_gatts_reset();
    if (rc) return rc;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    if ((rc = ble_gatts_count_cfg(svcs)) != 0 || (rc = ble_gatts_add_svcs(svcs)) != 0) return rc;
    if ((rc = ble_svc_gap_device_name_set(cfg.name)) != 0) return rc;
    return ble_gatts_start();
}

/* ---- GAP ---- */

static int gap_event(struct ble_gap_event *e, void *arg);

static int advertise(void) {
    struct ble_hs_adv_fields f;
    memset(&f, 0, sizeof f);
    f.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    f.name = (const uint8_t *)cfg.name;
    f.name_len = (uint8_t)strlen(cfg.name);
    f.name_is_complete = 1;
    int rc = ble_gap_adv_set_fields(&f);
    if (rc) return rc;
    struct ble_hs_adv_fields r;
    memset(&r, 0, sizeof r);
    if (cfg.svc[0].len == 2u) {
        r.uuids16 = &svc_uuid[0].u16;
        r.num_uuids16 = 1;
        r.uuids16_is_complete = cfg.nsvc == 1;
    } else {
        r.uuids128 = &svc_uuid[0].u128;
        r.num_uuids128 = 1;
        r.uuids128_is_complete = cfg.nsvc == 1;
    }
    if ((rc = ble_gap_adv_rsp_set_fields(&r)) != 0) return rc;
    struct ble_gap_adv_params p;
    memset(&p, 0, sizeof p);
    p.conn_mode = BLE_GAP_CONN_MODE_UND;
    p.disc_mode = BLE_GAP_DISC_MODE_GEN;
    return ble_gap_adv_start(own_addr, NULL, BLE_HS_FOREVER, &p, gap_event, NULL);
}

/* Advertises again, if the port is still open (host task). */
static void readvertise(void) {
    xSemaphoreTake(mu, portMAX_DELAY);
    bool on = running;
    xSemaphoreGive(mu);
    if (on && advertise() != 0) platform_esp_log("gogo: ble: advertising did not restart");
}

static int chr_of(uint16_t attr) {
    for (uint32_t k = 0; k < cfg.nchr; k++)
        if (val_handle[k] == attr) return (int)k;
    return -1;
}

static int gap_event(struct ble_gap_event *e, void *arg) {
    (void)arg;
    switch (e->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (e->connect.status != 0) {
            readvertise();
            break;
        }
        {
            struct ble_gap_conn_desc d;
            char peer[BLE_PEER_MAX + 1u] = "";
            if (ble_gap_conn_find(e->connect.conn_handle, &d) == 0) {
                const uint8_t *a = d.peer_id_addr.val;
                snprintf(peer, sizeof peer, "%02x:%02x:%02x:%02x:%02x:%02x", a[5], a[4], a[3], a[2], a[1], a[0]);
            }
            xSemaphoreTake(mu, portMAX_DELAY);
            conn = e->connect.conn_handle;
            xSemaphoreGive(mu);
            post(BLE_EV_CONNECTED, ble_att_mtu(e->connect.conn_handle), (const uint8_t *)peer, (uint32_t)strlen(peer));
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT: {
        int r = e->disconnect.reason;
        xSemaphoreTake(mu, portMAX_DELAY);
        conn = BLE_HS_CONN_HANDLE_NONE;
        xSemaphoreGive(mu);
        /* NimBLE's HCI reasons are BLE_HS_ERR_HCI_BASE + the code */
        post(BLE_EV_DISCONNECTED, (uint32_t)(r >= BLE_HS_ERR_HCI_BASE ? r - BLE_HS_ERR_HCI_BASE : r) & 0xFFFFu, NULL, 0);
        readvertise();
        break;
    }
    case BLE_GAP_EVENT_ADV_COMPLETE:
        readvertise();
        break;
    case BLE_GAP_EVENT_SUBSCRIBE: {
        int k = chr_of(e->subscribe.attr_handle);
        if (k >= 0 && e->subscribe.reason != BLE_GAP_SUBSCRIBE_REASON_TERM)
            post(BLE_EV_SUBSCRIBED, (uint32_t)k | (e->subscribe.cur_notify ? 1u << 8 : 0u), NULL, 0);
        break;
    }
    case BLE_GAP_EVENT_MTU:
        post(BLE_EV_MTU, e->mtu.value, NULL, 0);
        break;
    default:
        break;
    }
    return 0;
}

/* ---- the HAL ---- */

static void drop_values(void) {
    for (uint32_t k = 0; k < BLE_MAX_CHARS; k++) {
        heap_caps_free(val[k]);
        val[k] = NULL;
        vlen[k] = 0;
    }
}

static int hal_start(void *ctx, const BleConfig *c) {
    (void)ctx;
    if (ble_prov_active()) return GOGO_E_BUSY;
    if (ble_host_acquire() != 0) return GOGO_E_IO;
    cfg = *c;
    int rc = build_gatt();
    if (rc == 0) rc = ble_hs_id_infer_auto(0, &own_addr);
    xSemaphoreTake(mu, portMAX_DELAY);
    drop_values();
    conn = BLE_HS_CONN_HANDLE_NONE;
    running = rc == 0;
    xSemaphoreGive(mu);
    if (rc == 0) rc = advertise();
    if (rc != 0) {
        platform_esp_log("gogo: ble: GATT server or advertising failed (%d)", rc);
        xSemaphoreTake(mu, portMAX_DELAY);
        running = false;
        xSemaphoreGive(mu);
        ble_host_release();
        return rc == BLE_HS_ENOMEM ? GOGO_E_NOMEMORY : GOGO_E_IO;
    }
    return 0;
}

static void hal_stop(void *ctx) {
    (void)ctx;
    xSemaphoreTake(mu, portMAX_DELAY);
    running = false;
    xSemaphoreGive(mu);
    (void)ble_gap_adv_stop();
    ble_host_release();              /* ends the connection */
    xSemaphoreTake(mu, portMAX_DELAY);
    conn = BLE_HS_CONN_HANDLE_NONE;
    drop_values();
    xSemaphoreGive(mu);
}

static int hal_set_value(void *ctx, uint32_t k, const uint8_t *d, uint32_t len) {
    (void)ctx;
    uint8_t *v = NULL;
    if (len) {
#if CONFIG_SPIRAM
        v = heap_caps_malloc_prefer(len, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#else
        v = heap_caps_malloc(len, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
#endif
        if (!v) return GOGO_E_NOMEMORY;
        memcpy(v, d, len);
    }
    xSemaphoreTake(mu, portMAX_DELAY);
    heap_caps_free(val[k]);
    val[k] = v;
    vlen[k] = (uint16_t)len;
    xSemaphoreGive(mu);
    return 0;
}

static int hal_notify(void *ctx, uint32_t k, const uint8_t *d, uint32_t len) {
    (void)ctx;
    xSemaphoreTake(mu, portMAX_DELAY);
    uint16_t c = conn;
    xSemaphoreGive(mu);
    if (c == BLE_HS_CONN_HANDLE_NONE) return GOGO_E_CLOSED;
    struct os_mbuf *om = ble_hs_mbuf_from_flat(d, (uint16_t)len);
    if (!om) return BLE_AGAIN;
    /* NimBLE queues what the controller cannot take yet, so a burst could
     * take the whole pool the value came from (msys_1, the one the ATT
     * header and the host's replies use too): leave BLE_MSYS_SPARE of it
     * (ble.h ble_msys_spare_ok), counted in that pool only. */
    if (!ble_msys_spare_ok(om->om_omp->omp_pool->mp_num_free)) {
        os_mbuf_free_chain(om);
        return BLE_AGAIN;
    }
    int rc = ble_gatts_notify_custom(c, val_handle[k], om);   /* consumes om */
    switch (rc) {
    case 0:
        return 0;
    case BLE_HS_ENOMEM:       /* no mbuf for the ATT or L2CAP header */
    case BLE_HS_EAGAIN:
    case BLE_HS_EBUSY:
    case BLE_HS_ETIMEOUT_HCI: /* the controller did not take the packet in time */
        return BLE_AGAIN;
    case BLE_HS_ENOTCONN:
        return GOGO_E_CLOSED;
    default:
        return GOGO_E_IO;
    }
}

static const BleHal esp_ble_hal = {
    .ctx = NULL, .board = &board,
    .start = hal_start, .stop = hal_stop, .set_value = hal_set_value, .notify = hal_notify,
    .lock = hal_lock, .unlock = hal_unlock,
};

const struct PortDriver esp_ble_driver = BLE_DRIVER(&esp_ble_hal);
