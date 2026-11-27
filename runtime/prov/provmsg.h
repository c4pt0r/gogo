/* Wi-Fi provisioning messages: the "prov-config" endpoint of ESP-IDF's
 * provisioning protocol (network_provisioning's network_config.proto, wire
 * compatible with the older wifi_provisioning's wifi_config.proto), which
 * Espressif's "ESP BLE Provisioning" phone apps and esp_prov.py speak, and
 * the "proto-ver" document. Only the Wi-Fi messages: get status, set
 * config, apply config. Pure C99, no malloc; the firmware's BLE transport
 * and session security (protocomm, security 2) are platform/esp32s3's
 * ble_prov.c.
 *
 * Wire format (protobuf, proto3):
 *
 *     NetworkConfigPayload { msg = 1 (enum); oneof payload {
 *         CmdGetWifiStatus 10 {}       RespGetWifiStatus 11
 *         CmdSetWifiConfig 12          RespSetWifiConfig 13 { status = 1 }
 *         CmdApplyWifiConfig 14 {}     RespApplyWifiConfig 15 { status = 1 } } }
 *     CmdSetWifiConfig { bytes ssid = 1; bytes passphrase = 2; bytes bssid = 3; int32 channel = 4 }
 *     RespGetWifiStatus { status = 1; wifi_sta_state = 2; oneof state {
 *         wifi_fail_reason = 10 (enum); WifiConnectedState wifi_connected = 11 } }
 *     WifiConnectedState { string ip4_addr = 1; auth_mode = 2; bytes ssid = 3; ... }
 *
 * The decoder skips fields it does not know (any wire type) and rejects
 * truncated or malformed input; a member of a oneof is encoded even when
 * its value is 0 (AuthError), as protobuf does. */
#ifndef GOGO_PROVMSG_H
#define GOGO_PROVMSG_H

#include <stddef.h>
#include <stdint.h>

/* NetworkConfigMsgType (requests even, responses odd). */
#define PROV_MSG_GET_STATUS   0
#define PROV_MSG_SET_CONFIG   2
#define PROV_MSG_APPLY_CONFIG 4

/* constants.proto Status. */
#define PROV_STATUS_OK          0
#define PROV_STATUS_INVALID_ARG 4
#define PROV_STATUS_INTERNAL    5

/* WifiStationState. */
#define PROV_STA_CONNECTED    0
#define PROV_STA_CONNECTING   1
#define PROV_STA_DISCONNECTED 2
#define PROV_STA_FAILED       3

/* WifiConnectFailedReason. */
#define PROV_FAIL_AUTH      0
#define PROV_FAIL_NOT_FOUND 1

#define PROV_SSID_MAX 32u   /* bytes (802.11) */
#define PROV_PASS_MAX 63u   /* bytes: a WPA2 passphrase; what wifi.password holds (wifi.c) */

/* A decoded request: msg is PROV_MSG_*; for PROV_MSG_SET_CONFIG the
 * credentials (not NUL-terminated; ssid_len 1..32, pass_len 0..63 when
 * prov_decode returned PROV_DEC_OK) and bad is set if a length was out of
 * range (the request is answered with PROV_STATUS_INVALID_ARG). */
typedef struct {
    int msg;
    uint8_t ssid[PROV_SSID_MAX];
    uint8_t pass[PROV_PASS_MAX];
    uint8_t ssid_len, pass_len;
    uint8_t bad;
} ProvRequest;

#define PROV_DEC_OK        0
#define PROV_DEC_MALFORMED (-1)   /* truncated, a bad varint or wire type */
#define PROV_DEC_UNKNOWN   (-2)   /* a message type this side does not serve (Thread, a response) */

int prov_decode(const uint8_t *in, size_t n, ProvRequest *r);

/* Encoders: the response into out (cap bytes); its length, or 0 if it
 * does not fit. */
size_t prov_encode_status(uint8_t *out, size_t cap, int sta_state, int fail_reason, const uint8_t *ssid,
                          size_t ssid_len);
/* RespSetWifiConfig (req PROV_MSG_SET_CONFIG) or RespApplyWifiConfig
 * (PROV_MSG_APPLY_CONFIG). */
size_t prov_encode_resp(uint8_t *out, size_t cap, int req, int status);

/* The "proto-ver" document: {"prov":{"ver":"v1.1","sec_ver":S,
 * "sec_patch_ver":P,"cap":["wifi_prov"]}}. No "wifi_scan": the phone asks
 * for the network name. Its length, or 0 if it does not fit (NUL
 * included). */
size_t prov_version_json(char *out, size_t cap, int sec_ver, int sec_patch_ver);

/* The six-digit PIN for a 32-bit random source: rejection sampling, so
 * every PIN is equally likely; rnd is called until it gives an unbiased
 * value. out holds 7 bytes (NUL-terminated). */
void prov_pin(char out[7], uint32_t (*rnd)(void));

#endif
