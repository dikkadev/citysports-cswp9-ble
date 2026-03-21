/*
 * CitySports CS-WP9 Treadmill Controller (v1)
 *
 * ESP32-H2 Zigbee End Device + BLE GATT Client
 *
 * Connects to a CitySports CS-WP9 treadmill via BLE (proprietary protocol,
 * not FTMS) and exposes it to Home Assistant as a Zigbee device with:
 *   - On/Off cluster: start / stop
 *   - Analog Output: target speed (1.0 – 12.0 km/h)
 *   - Analog Value: state code (idle/starting/running/stopping)
 *
 * The NanoH2 lives inside the treadmill, powered by it. It boots when the
 * treadmill gets mains power and waits a few seconds before attempting BLE
 * connection (giving the treadmill's BLE MCU time to start advertising).
 *
 * Treadmill BLE MAC must be configured once via the Zigbee Basic cluster's
 * location_description attribute (format: "XX:XX:XX:XX:XX:XX").
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "freertos/event_groups.h"
#include "driver/gpio.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"
#include "esp_zigbee_core.h"
#include "ha/esp_zigbee_ha_standard.h"

// NimBLE
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"
#include "host/ble_hs.h"
#include "host/util/util.h"

// ============================================================================
// Configuration
// ============================================================================

#define TAG "TREADMILL"

// GPIO
#define LED_GPIO            GPIO_NUM_4

// Zigbee
#define TREADMILL_ENDPOINT  1
#define MANUFACTURER_NAME   "\x09""ESPRESSIF"
#define MODEL_ID            "\x09""TREADMILL"

// NVS
#define NVS_NAMESPACE       "treadmill"
#define NVS_KEY_MAC         "ble_mac"
#define NVS_KEY_START_SPEED "start_spd"

// Default start speed — sent before START command so the treadmill
// doesn't always begin at 1.0 km/h (since we lack pause/resume).
#define DEFAULT_START_SPEED 3.0f

// Startup delay — give treadmill BLE time to start advertising after power-on.
// The H2 is powered by the treadmill, so both boot simultaneously.
#define STARTUP_DELAY_SEC   5

// BLE reconnect backoff
#define RECONNECT_MIN_MS    3000
#define RECONNECT_MAX_MS    30000

// Speed limits — SPEED_MAX_KMH can be overridden at build time via
// idf.py build -DSPEED_MAX_KMH=7.5  (or set in Justfile)
#define SPEED_MIN_KMH       1.0f
#ifndef SPEED_MAX_KMH
#define SPEED_MAX_KMH       7.5f
#endif

// Command queue depth
#define CMD_QUEUE_SIZE      8

// Event group bits
#define EVT_BLE_SYNCED       (1 << 0)
#define EVT_BLE_DISCONNECTED (1 << 1)
#define EVT_TREADMILL_UPDATE (1 << 2)
#define EVT_MAC_CHANGED      (1 << 3)

// ============================================================================
// Zigbee Configuration Macros
// ============================================================================

#define ESP_ZB_ZED_CONFIG() \
    { \
        .esp_zb_role = ESP_ZB_DEVICE_TYPE_ED, \
        .install_code_policy = false, \
        .nwk_cfg.zed_cfg = { \
            .ed_timeout = ESP_ZB_ED_AGING_TIMEOUT_64MIN, \
            .keep_alive = 3000, \
        }, \
    }

#define ESP_ZB_DEFAULT_RADIO_CONFIG() \
    { \
        .radio_mode = ZB_RADIO_MODE_NATIVE, \
    }

#define ESP_ZB_DEFAULT_HOST_CONFIG() \
    { \
        .host_connection_mode = ZB_HOST_CONNECTION_MODE_NONE, \
    }

// ============================================================================
// BLE UUIDs (little-endian for NimBLE)
// ============================================================================

// Service: ffeeddcc-bbaa-9988-7766-554433221100
static const ble_uuid128_t svc_uuid = BLE_UUID128_INIT(
    0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
);

// Write characteristic: ffeeddcc-bbaa-9988-7766-554433221101
static const ble_uuid128_t write_chr_uuid = BLE_UUID128_INIT(
    0x01, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
);

// Notify characteristic: ffeeddcc-bbaa-9988-7766-554433221102
static const ble_uuid128_t notify_chr_uuid = BLE_UUID128_INIT(
    0x02, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
    0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff
);

// CCCD descriptor: 0x2902
static const ble_uuid16_t cccd_uuid = BLE_UUID16_INIT(0x2902);

// ============================================================================
// Types
// ============================================================================

typedef enum {
    CMD_START,
    CMD_STOP,
    CMD_SET_SPEED,
    CMD_STATUS_QUERY,
} cmd_type_t;

typedef struct {
    cmd_type_t type;
    float speed_kmh;
} treadmill_cmd_t;

typedef enum {
    STATE_STARTING = 0x01,
    STATE_RUNNING  = 0x02,
    STATE_STOPPING = 0x05,
    STATE_IDLE     = 0x06,
} treadmill_state_t;

// ============================================================================
// State
// ============================================================================

// BLE connection
static uint16_t g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t g_write_handle = 0;
static uint16_t g_notify_handle = 0;
static uint16_t g_cccd_handle = 0;
static uint16_t g_svc_start = 0;
static uint16_t g_svc_end = 0;

// Treadmill state (from BLE notifications)
static volatile treadmill_state_t g_state = STATE_IDLE;
static volatile float g_target_speed = 0.0f;

// Last reported to Zigbee (for change detection / throttling)
static treadmill_state_t g_reported_state = STATE_IDLE;
static float g_reported_speed = 0.0f;

// Configuration
static char g_ble_mac[18] = "";   // "XX:XX:XX:XX:XX:XX"
static bool g_mac_configured = false;
static float g_start_speed = DEFAULT_START_SPEED;

// FreeRTOS primitives
static QueueHandle_t g_cmd_queue = NULL;
static EventGroupHandle_t g_events = NULL;

// ============================================================================
// Forward Declarations
// ============================================================================

static void attempt_ble_connect(void);
static void save_mac_to_nvs(void);

// ============================================================================
// Treadmill BLE Protocol
// ============================================================================

static uint8_t xor_checksum(const uint8_t *data, size_t len)
{
    uint8_t x = 0;
    for (size_t i = 0; i < len; i++) x ^= data[i];
    return x;
}

static size_t build_start(uint8_t *buf)
{
    uint8_t d[] = {0xA1, 0x03, 0x01, 0x01};
    memcpy(buf, d, 4);
    buf[4] = xor_checksum(buf, 4);
    return 5;
}

static size_t build_stop(uint8_t *buf)
{
    uint8_t d[] = {0xA1, 0x03, 0x01, 0x05};
    memcpy(buf, d, 4);
    buf[4] = xor_checksum(buf, 4);
    return 5;
}

static size_t build_set_speed(uint8_t *buf, float kmh)
{
    uint8_t raw = (uint8_t)(kmh * 10.0f + 0.5f);
    if (raw < 10)  raw = 10;
    if (raw > 120) raw = 120;
    buf[0] = 0xA1; buf[1] = 0x01; buf[2] = 0x02; buf[3] = 0x01; buf[4] = raw;
    buf[5] = xor_checksum(buf, 5);
    return 6;
}

static size_t build_status_query(uint8_t *buf)
{
    buf[0] = 0xA1; buf[1] = 0x05; buf[2] = 0x00;
    buf[3] = xor_checksum(buf, 3);
    return 4;
}

// Parse type-01 status notification (13 bytes)
static void parse_notification(const uint8_t *data, size_t len)
{
    if (len < 12 || data[0] != 0x1A || data[1] != 0x01) return;

    float target = data[7] / 10.0f;
    uint8_t state = data[9];

    g_target_speed = target;

    switch (state) {
    case 0x01: g_state = STATE_STARTING; break;
    case 0x02: g_state = STATE_RUNNING;  break;
    case 0x05: g_state = STATE_STOPPING; break;
    case 0x06: g_state = STATE_IDLE;     break;
    default: return;
    }

    xEventGroupSetBits(g_events, EVT_TREADMILL_UPDATE);
}

// ============================================================================
// MAC Address Helpers
// ============================================================================

// Parse MAC in either format: "70:19:88:B2:1C:EA" or "701988B21CEA"
static bool parse_mac(const char *str, ble_addr_t *addr)
{
    unsigned int b[6];

    // Try colon-separated first
    if (sscanf(str, "%02x:%02x:%02x:%02x:%02x:%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        goto ok;
    }

    // Try compact (no colons)
    if (strlen(str) == 12 &&
        sscanf(str, "%02x%02x%02x%02x%02x%02x",
               &b[0], &b[1], &b[2], &b[3], &b[4], &b[5]) == 6) {
        goto ok;
    }

    return false;

ok:
    addr->type = BLE_ADDR_PUBLIC;
    for (int i = 0; i < 6; i++)
        addr->val[5 - i] = (uint8_t)b[i];
    return true;
}

// ============================================================================
// NVS
// ============================================================================

static void load_config_from_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGI(TAG, "No saved config, using defaults");
        return;
    }

    size_t len = sizeof(g_ble_mac);
    if (nvs_get_str(h, NVS_KEY_MAC, g_ble_mac, &len) == ESP_OK) {
        ble_addr_t tmp;
        if (parse_mac(g_ble_mac, &tmp)) {
            g_mac_configured = true;
            ESP_LOGI(TAG, "Loaded MAC: %s", g_ble_mac);
        }
    }

    // Start speed (stored as uint16 = speed * 10)
    uint16_t spd_raw;
    if (nvs_get_u16(h, NVS_KEY_START_SPEED, &spd_raw) == ESP_OK) {
        g_start_speed = spd_raw / 10.0f;
        ESP_LOGI(TAG, "Loaded start speed: %.1f km/h", g_start_speed);
    }

    nvs_close(h);
}

static void save_mac_to_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, NVS_KEY_MAC, g_ble_mac);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "MAC saved to NVS");
}

static void save_start_speed_to_nvs(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h) != ESP_OK) return;
    uint16_t raw = (uint16_t)(g_start_speed * 10.0f + 0.5f);
    nvs_set_u16(h, NVS_KEY_START_SPEED, raw);
    nvs_commit(h);
    nvs_close(h);
    ESP_LOGI(TAG, "Start speed saved: %.1f km/h", g_start_speed);
}

// ============================================================================
// LED
// ============================================================================

static void led_init(void)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << LED_GPIO),
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io);
    gpio_set_level(LED_GPIO, 0);
}

static void led_set(bool on) { gpio_set_level(LED_GPIO, on ? 1 : 0); }

// ============================================================================
// BLE GATT Client — Discovery Chain
// ============================================================================

// Step 4: CCCD written → notifications enabled, BLE ready
static int on_cccd_write(uint16_t conn, const struct ble_gatt_error *err,
                         struct ble_gatt_attr *attr, void *arg)
{
    if (err->status != 0) {
        ESP_LOGE(TAG, "CCCD write failed: %d", err->status);
        return 0;
    }

    ESP_LOGI(TAG, "Notifications enabled — BLE ready");

    // Send initial status query
    uint8_t cmd[4];
    size_t len = build_status_query(cmd);
    ble_gattc_write_flat(conn, g_write_handle, cmd, len, NULL, NULL);
    return 0;
}

// Step 3: Descriptor discovery → find CCCD → enable notifications
static int on_dsc_disc(uint16_t conn, const struct ble_gatt_error *err,
                       uint16_t chr_val, const struct ble_gatt_dsc *dsc, void *arg)
{
    if (err->status == 0 && dsc != NULL) {
        if (ble_uuid_cmp(&dsc->uuid.u, &cccd_uuid.u) == 0) {
            g_cccd_handle = dsc->handle;
            ESP_LOGI(TAG, "  CCCD: 0x%04x", g_cccd_handle);
        }
    } else if (err->status == BLE_HS_EDONE) {
        if (g_cccd_handle > 0) {
            uint8_t val[2] = {0x01, 0x00};  // enable notifications
            ble_gattc_write_flat(conn, g_cccd_handle, val, 2, on_cccd_write, NULL);
        } else {
            ESP_LOGE(TAG, "CCCD not found");
        }
    }
    return 0;
}

// Step 2: Characteristic discovery → find write + notify handles
static int on_chr_disc(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_chr *chr, void *arg)
{
    if (err->status == 0 && chr != NULL) {
        if (ble_uuid_cmp(&chr->uuid.u, &write_chr_uuid.u) == 0) {
            g_write_handle = chr->val_handle;
            ESP_LOGI(TAG, "  Write chr: 0x%04x", g_write_handle);
        }
        if (ble_uuid_cmp(&chr->uuid.u, &notify_chr_uuid.u) == 0) {
            g_notify_handle = chr->val_handle;
            ESP_LOGI(TAG, "  Notify chr: 0x%04x", g_notify_handle);
        }
    } else if (err->status == BLE_HS_EDONE) {
        if (g_notify_handle > 0) {
            ble_gattc_disc_all_dscs(conn, g_notify_handle, g_svc_end,
                                    on_dsc_disc, NULL);
        } else {
            ESP_LOGE(TAG, "Notify characteristic not found");
        }
    }
    return 0;
}

// Step 1: Service discovery → find treadmill service
static int on_svc_disc(uint16_t conn, const struct ble_gatt_error *err,
                       const struct ble_gatt_svc *svc, void *arg)
{
    if (err->status == 0 && svc != NULL) {
        g_svc_start = svc->start_handle;
        g_svc_end = svc->end_handle;
        ESP_LOGI(TAG, "Service found: 0x%04x–0x%04x", g_svc_start, g_svc_end);
    } else if (err->status == BLE_HS_EDONE) {
        if (g_svc_start > 0) {
            ble_gattc_disc_all_chrs(conn, g_svc_start, g_svc_end,
                                    on_chr_disc, NULL);
        } else {
            ESP_LOGE(TAG, "Treadmill service not found — wrong device?");
        }
    }
    return 0;
}

// ============================================================================
// BLE GAP Event Handler
// ============================================================================

static int gap_event_cb(struct ble_gap_event *event, void *arg)
{
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            g_conn_handle = event->connect.conn_handle;
            ESP_LOGI(TAG, "BLE connected (handle=%d)", g_conn_handle);
            // Start GATT discovery
            ble_gattc_disc_svc_by_uuid(g_conn_handle,
                (const ble_uuid_t *)&svc_uuid, on_svc_disc, NULL);
        } else {
            ESP_LOGW(TAG, "BLE connect failed: %d", event->connect.status);
            g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
            xEventGroupSetBits(g_events, EVT_BLE_DISCONNECTED);
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "BLE disconnected (reason=%d)",
                 event->disconnect.reason);
        g_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        g_write_handle = 0;
        g_notify_handle = 0;
        g_cccd_handle = 0;
        g_svc_start = 0;
        g_svc_end = 0;
        xEventGroupSetBits(g_events, EVT_BLE_DISCONNECTED);
        break;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        struct os_mbuf *om = event->notify_rx.om;
        uint16_t len = OS_MBUF_PKTLEN(om);
        if (len <= 20) {
            uint8_t buf[20];
            os_mbuf_copydata(om, 0, len, buf);
            parse_notification(buf, len);
        }
        break;
    }

    default:
        break;
    }
    return 0;
}

// ============================================================================
// BLE Connection
// ============================================================================

static void attempt_ble_connect(void)
{
    if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE || !g_mac_configured) return;

    ble_addr_t addr;
    if (!parse_mac(g_ble_mac, &addr)) {
        ESP_LOGE(TAG, "Invalid MAC: %s", g_ble_mac);
        return;
    }

    // Connection params: 50–100 ms interval, balanced for BLE+Zigbee coexistence
    struct ble_gap_conn_params params = {
        .scan_itvl = 0x0010,
        .scan_window = 0x0010,
        .itvl_min = 40,   // 50 ms
        .itvl_max = 80,   // 100 ms
        .latency = 0,
        .supervision_timeout = 400,  // 4 s
    };

    ESP_LOGI(TAG, "Connecting to %s ...", g_ble_mac);
    int rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, 30000, &params,
                             gap_event_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "ble_gap_connect error: %d", rc);
        xEventGroupSetBits(g_events, EVT_BLE_DISCONNECTED);
    }
}

// ============================================================================
// BLE Command Sending
// ============================================================================

static void send_ble_cmd(const treadmill_cmd_t *cmd)
{
    if (g_conn_handle == BLE_HS_CONN_HANDLE_NONE || g_write_handle == 0) {
        ESP_LOGW(TAG, "BLE not ready, command dropped");
        return;
    }

    uint8_t buf[8];
    size_t len = 0;

    switch (cmd->type) {
    case CMD_START:
        len = build_start(buf);
        ESP_LOGI(TAG, "=> START");
        break;
    case CMD_STOP:
        len = build_stop(buf);
        ESP_LOGI(TAG, "=> STOP");
        break;
    case CMD_SET_SPEED:
        len = build_set_speed(buf, cmd->speed_kmh);
        ESP_LOGI(TAG, "=> SPEED %.1f km/h", cmd->speed_kmh);
        break;
    case CMD_STATUS_QUERY:
        len = build_status_query(buf);
        break;
    }

    if (len > 0) {
        int rc = ble_gattc_write_flat(g_conn_handle, g_write_handle,
                                      buf, len, NULL, NULL);
        if (rc != 0) ESP_LOGE(TAG, "BLE write error: %d", rc);
    }
}

// ============================================================================
// Zigbee — Attribute Updates & Reporting
// ============================================================================

static void report_attr(uint16_t cluster_id, uint16_t attr_id)
{
    esp_zb_zcl_report_attr_cmd_t cmd = {
        .zcl_basic_cmd.src_endpoint = TREADMILL_ENDPOINT,
        .address_mode = ESP_ZB_APS_ADDR_MODE_DST_ADDR_ENDP_NOT_PRESENT,
        .clusterID = cluster_id,
        .attributeID = attr_id,
        .direction = ESP_ZB_ZCL_CMD_DIRECTION_TO_CLI,
    };
    esp_zb_lock_acquire(portMAX_DELAY);
    esp_zb_zcl_report_attr_cmd_req(&cmd);
    esp_zb_lock_release();
}

// Sync all Zigbee attributes from current treadmill state.
// Only reports when something actually changed.
static void sync_zigbee_state(void)
{
    treadmill_state_t st = g_state;
    float spd = g_target_speed;

    bool state_changed = (st != g_reported_state);
    float diff = spd - g_reported_speed;
    if (diff < 0) diff = -diff;
    bool speed_changed = (diff > 0.05f);

    if (!state_changed && !speed_changed) return;

    g_reported_state = st;
    g_reported_speed = spd;

    bool is_on = (st == STATE_STARTING || st == STATE_RUNNING ||
                  st == STATE_STOPPING);
    float state_f = (float)st;

    esp_zb_lock_acquire(portMAX_DELAY);

    // On/Off
    uint8_t on_off_val = is_on ? 1 : 0;
    esp_zb_zcl_set_attribute_val(TREADMILL_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_ON_OFF, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID, &on_off_val, false);

    // Speed (Analog Value)
    esp_zb_zcl_set_attribute_val(TREADMILL_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_ANALOG_VALUE, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ANALOG_VALUE_PRESENT_VALUE_ID, &spd, false);

    // State code (Analog Input)
    esp_zb_zcl_set_attribute_val(TREADMILL_ENDPOINT,
        ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT, ESP_ZB_ZCL_CLUSTER_SERVER_ROLE,
        ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID, &state_f, false);

    esp_zb_lock_release();

    led_set(is_on);

    if (state_changed) {
        report_attr(ESP_ZB_ZCL_CLUSTER_ID_ON_OFF,
                    ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID);
        report_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_INPUT,
                    ESP_ZB_ZCL_ATTR_ANALOG_INPUT_PRESENT_VALUE_ID);
    }
    if (speed_changed) {
        report_attr(ESP_ZB_ZCL_CLUSTER_ID_ANALOG_VALUE,
                    ESP_ZB_ZCL_ATTR_ANALOG_VALUE_PRESENT_VALUE_ID);
    }
}

// ============================================================================
// Zigbee — Signal Handler
// ============================================================================

static void bdb_start_cb(uint8_t mode)
{
    ESP_ERROR_CHECK(esp_zb_bdb_start_top_level_commissioning(mode));
}

void esp_zb_app_signal_handler(esp_zb_app_signal_t *sig)
{
    esp_zb_app_signal_type_t type = *sig->p_app_signal;
    esp_err_t status = sig->esp_err_status;

    switch (type) {
    case ESP_ZB_ZDO_SIGNAL_SKIP_STARTUP:
        esp_zb_bdb_start_top_level_commissioning(ESP_ZB_BDB_MODE_INITIALIZATION);
        break;
    case ESP_ZB_BDB_SIGNAL_DEVICE_FIRST_START:
    case ESP_ZB_BDB_SIGNAL_DEVICE_REBOOT:
        if (status == ESP_OK) {
            if (esp_zb_bdb_is_factory_new()) {
                ESP_LOGI(TAG, "Starting network steering...");
                esp_zb_bdb_start_top_level_commissioning(
                    ESP_ZB_BDB_MODE_NETWORK_STEERING);
            } else {
                ESP_LOGI(TAG, "Rejoined network");
            }
        } else {
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_cb,
                                   ESP_ZB_BDB_MODE_INITIALIZATION, 1000);
        }
        break;
    case ESP_ZB_BDB_SIGNAL_STEERING:
        if (status == ESP_OK) {
            ESP_LOGI(TAG, "Joined! PAN:0x%04hx Ch:%d",
                     esp_zb_get_pan_id(), esp_zb_get_current_channel());
        } else {
            esp_zb_scheduler_alarm((esp_zb_callback_t)bdb_start_cb,
                                   ESP_ZB_BDB_MODE_NETWORK_STEERING, 1000);
        }
        break;
    default:
        break;
    }
}

// ============================================================================
// Zigbee — Action Handler
// ============================================================================

static esp_err_t zb_attr_handler(const esp_zb_zcl_set_attr_value_message_t *msg)
{
    if (!msg || msg->info.status != ESP_ZB_ZCL_STATUS_SUCCESS) return ESP_FAIL;

    ESP_LOGI(TAG, "Zigbee write: cluster=0x%04x attr=0x%04x",
             msg->info.cluster, msg->attribute.id);

    // On/Off → start (with default speed) / stop
    if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ON_OFF &&
        msg->attribute.id == ESP_ZB_ZCL_ATTR_ON_OFF_ON_OFF_ID) {
        bool on = *(uint8_t *)msg->attribute.data.value;
        if (on) {
            // Send speed BEFORE start — treadmill resets to 1.0 on stop,
            // and we don't have pause/resume yet.
            treadmill_cmd_t spd = { .type = CMD_SET_SPEED,
                                    .speed_kmh = g_start_speed };
            xQueueSend(g_cmd_queue, &spd, pdMS_TO_TICKS(100));
            treadmill_cmd_t start = { .type = CMD_START };
            xQueueSend(g_cmd_queue, &start, pdMS_TO_TICKS(100));
        } else {
            treadmill_cmd_t stop = { .type = CMD_STOP };
            xQueueSend(g_cmd_queue, &stop, pdMS_TO_TICKS(100));
        }
    }

    // Analog Value → set speed (also saves as default start speed)
    if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_ANALOG_VALUE &&
        msg->attribute.id == ESP_ZB_ZCL_ATTR_ANALOG_VALUE_PRESENT_VALUE_ID) {
        float speed = *(float *)msg->attribute.data.value;
        if (speed < SPEED_MIN_KMH) speed = SPEED_MIN_KMH;
        if (speed > SPEED_MAX_KMH) speed = SPEED_MAX_KMH;
        treadmill_cmd_t cmd = { .type = CMD_SET_SPEED, .speed_kmh = speed };
        xQueueSend(g_cmd_queue, &cmd, pdMS_TO_TICKS(100));
        // Save as default start speed
        g_start_speed = speed;
        save_start_speed_to_nvs();
    }

    // Basic.location_description → BLE MAC configuration
    if (msg->info.cluster == ESP_ZB_ZCL_CLUSTER_ID_BASIC &&
        msg->attribute.id == ESP_ZB_ZCL_ATTR_BASIC_LOCATION_DESCRIPTION_ID) {
        const uint8_t *data = msg->attribute.data.value;
        uint8_t len = data[0];  // ZCL string: length byte + chars
        if (len >= 12 && len < sizeof(g_ble_mac)) {
            memcpy(g_ble_mac, &data[1], len);
            g_ble_mac[len] = '\0';
            ble_addr_t tmp;
            if (parse_mac(g_ble_mac, &tmp)) {
                g_mac_configured = true;
                save_mac_to_nvs();
                ESP_LOGI(TAG, "MAC configured: %s", g_ble_mac);
                xEventGroupSetBits(g_events, EVT_MAC_CHANGED);
            } else {
                ESP_LOGW(TAG, "Invalid MAC format: %s", g_ble_mac);
            }
        }
    }

    return ESP_OK;
}

static esp_err_t zb_action_handler(esp_zb_core_action_callback_id_t id,
                                   const void *msg)
{
    if (id == ESP_ZB_CORE_SET_ATTR_VALUE_CB_ID)
        return zb_attr_handler((esp_zb_zcl_set_attr_value_message_t *)msg);
    return ESP_OK;
}

// ============================================================================
// Zigbee — Cluster & Endpoint Creation
// ============================================================================

static esp_zb_cluster_list_t *create_clusters(void)
{
    esp_zb_cluster_list_t *list = esp_zb_zcl_cluster_list_create();

    // --- Basic ---
    esp_zb_basic_cluster_cfg_t basic_cfg = {
        .zcl_version = ESP_ZB_ZCL_BASIC_ZCL_VERSION_DEFAULT_VALUE,
        .power_source = ESP_ZB_ZCL_BASIC_POWER_SOURCE_DEFAULT_VALUE,
    };
    esp_zb_attribute_list_t *basic = esp_zb_basic_cluster_create(&basic_cfg);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MANUFACTURER_NAME_ID, (void *)MANUFACTURER_NAME);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_MODEL_IDENTIFIER_ID, (void *)MODEL_ID);

    // Location description = treadmill BLE MAC (writable for config)
    static char loc_desc[20];
    uint8_t mac_len = strlen(g_ble_mac);
    loc_desc[0] = mac_len;
    if (mac_len > 0) memcpy(&loc_desc[1], g_ble_mac, mac_len);
    esp_zb_basic_cluster_add_attr(basic,
        ESP_ZB_ZCL_ATTR_BASIC_LOCATION_DESCRIPTION_ID, loc_desc);

    esp_zb_cluster_list_add_basic_cluster(list, basic,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // --- Identify ---
    esp_zb_identify_cluster_cfg_t id_cfg = {0};
    esp_zb_cluster_list_add_identify_cluster(list,
        esp_zb_identify_cluster_create(&id_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // --- On/Off (start / stop) ---
    esp_zb_on_off_cluster_cfg_t onoff_cfg = { .on_off = false };
    esp_zb_cluster_list_add_on_off_cluster(list,
        esp_zb_on_off_cluster_create(&onoff_cfg), ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // --- Analog Value (speed control, writable) ---
    esp_zb_analog_value_cluster_cfg_t av_cfg = {
        .out_of_service = false,
        .present_value = g_start_speed,
    };
    esp_zb_attribute_list_t *av = esp_zb_analog_value_cluster_create(&av_cfg);
    static char av_desc[] = "\x0C""Speed (km/h)";
    esp_zb_analog_value_cluster_add_attr(av,
        ESP_ZB_ZCL_ATTR_ANALOG_VALUE_DESCRIPTION_ID, av_desc);
    esp_zb_cluster_list_add_analog_value_cluster(list, av,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    // --- Analog Input (state code, read-only reporting) ---
    esp_zb_analog_input_cluster_cfg_t ai_cfg = {
        .out_of_service = false,
        .present_value = (float)STATE_IDLE,
    };
    esp_zb_attribute_list_t *ai = esp_zb_analog_input_cluster_create(&ai_cfg);
    static char ai_desc[] = "\x05""State";
    esp_zb_analog_input_cluster_add_attr(ai,
        ESP_ZB_ZCL_ATTR_ANALOG_INPUT_DESCRIPTION_ID, ai_desc);
    esp_zb_cluster_list_add_analog_input_cluster(list, ai,
        ESP_ZB_ZCL_CLUSTER_SERVER_ROLE);

    return list;
}

static esp_zb_ep_list_t *create_endpoint(void)
{
    esp_zb_ep_list_t *ep = esp_zb_ep_list_create();
    esp_zb_endpoint_config_t cfg = {
        .endpoint = TREADMILL_ENDPOINT,
        .app_profile_id = ESP_ZB_AF_HA_PROFILE_ID,
        .app_device_id = ESP_ZB_HA_ON_OFF_OUTPUT_DEVICE_ID,
    };
    esp_zb_ep_list_add_ep(ep, create_clusters(), cfg);
    return ep;
}

// ============================================================================
// Bridge Task — BLE ↔ Zigbee
// ============================================================================

static void bridge_task(void *p)
{
    ESP_LOGI(TAG, "Bridge task started");

    // Wait for NimBLE host sync
    xEventGroupWaitBits(g_events, EVT_BLE_SYNCED, pdFALSE, pdTRUE,
                        portMAX_DELAY);

    // Startup delay: treadmill BLE needs time to start advertising
    ESP_LOGI(TAG, "Waiting %d s for treadmill BLE to be ready...",
             STARTUP_DELAY_SEC);
    vTaskDelay(pdMS_TO_TICKS(STARTUP_DELAY_SEC * 1000));

    if (g_mac_configured) {
        attempt_ble_connect();
    } else {
        ESP_LOGW(TAG, "No BLE MAC configured — set it via Zigbee "
                      "(Basic > location_description = \"XX:XX:XX:XX:XX:XX\")");
    }

    uint32_t reconnect_ms = RECONNECT_MIN_MS;

    while (1) {
        EventBits_t bits = xEventGroupWaitBits(g_events,
            EVT_BLE_DISCONNECTED | EVT_TREADMILL_UPDATE | EVT_MAC_CHANGED,
            pdTRUE, pdFALSE, pdMS_TO_TICKS(100));

        // MAC changed via Zigbee — disconnect old, connect new
        if (bits & EVT_MAC_CHANGED) {
            if (g_conn_handle != BLE_HS_CONN_HANDLE_NONE) {
                ble_gap_terminate(g_conn_handle, BLE_ERR_REM_USER_CONN_TERM);
                vTaskDelay(pdMS_TO_TICKS(500));
            }
            reconnect_ms = RECONNECT_MIN_MS;
            attempt_ble_connect();
        }

        // BLE disconnected — update Zigbee, then reconnect with backoff
        if (bits & EVT_BLE_DISCONNECTED) {
            g_state = STATE_IDLE;
            g_target_speed = 0;
            sync_zigbee_state();

            if (g_mac_configured) {
                ESP_LOGI(TAG, "Reconnecting in %lu ms...",
                         (unsigned long)reconnect_ms);
                vTaskDelay(pdMS_TO_TICKS(reconnect_ms));
                attempt_ble_connect();
                reconnect_ms *= 2;
                if (reconnect_ms > RECONNECT_MAX_MS)
                    reconnect_ms = RECONNECT_MAX_MS;
            }
        }

        // Treadmill status update from BLE notification
        if (bits & EVT_TREADMILL_UPDATE) {
            reconnect_ms = RECONNECT_MIN_MS;  // activity = reset backoff
            sync_zigbee_state();
        }

        // Drain command queue
        treadmill_cmd_t cmd;
        while (xQueueReceive(g_cmd_queue, &cmd, 0) == pdTRUE) {
            send_ble_cmd(&cmd);
        }
    }
}

// ============================================================================
// NimBLE Host Task
// ============================================================================

static void ble_host_task(void *p)
{
    nimble_port_run();
    nimble_port_freertos_deinit();
}

static void ble_on_sync(void)
{
    ESP_LOGI(TAG, "BLE host synced");
    xEventGroupSetBits(g_events, EVT_BLE_SYNCED);
}

static void ble_on_reset(int reason)
{
    ESP_LOGW(TAG, "BLE host reset: %d", reason);
}

// ============================================================================
// Zigbee Task
// ============================================================================

static void zigbee_task(void *p)
{
    esp_zb_cfg_t cfg = ESP_ZB_ZED_CONFIG();
    esp_zb_init(&cfg);
    esp_zb_device_register(create_endpoint());
    esp_zb_core_action_handler_register(zb_action_handler);
    esp_zb_set_primary_network_channel_set(ESP_ZB_TRANSCEIVER_ALL_CHANNELS_MASK);
    ESP_ERROR_CHECK(esp_zb_start(false));
    esp_zb_stack_main_loop();
}

// ============================================================================
// Main
// ============================================================================

void app_main(void)
{
    ESP_LOGI(TAG, "CitySports CS-WP9 Treadmill Controller v1");

    // NVS
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    load_config_from_nvs();
    led_init();

    // FreeRTOS primitives
    g_cmd_queue = xQueueCreate(CMD_QUEUE_SIZE, sizeof(treadmill_cmd_t));
    g_events = xEventGroupCreate();

    // NimBLE
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "NimBLE init failed: %d", ret);
        return;
    }
    ble_hs_cfg.sync_cb = ble_on_sync;
    ble_hs_cfg.reset_cb = ble_on_reset;
    nimble_port_freertos_init(ble_host_task);

    // Zigbee platform
    esp_zb_platform_config_t zb_cfg = {
        .radio_config = ESP_ZB_DEFAULT_RADIO_CONFIG(),
        .host_config = ESP_ZB_DEFAULT_HOST_CONFIG(),
    };
    ESP_ERROR_CHECK(esp_zb_platform_config(&zb_cfg));

    // Start tasks
    xTaskCreate(bridge_task,  "bridge",  4096, NULL, 5, NULL);
    xTaskCreate(zigbee_task,  "zigbee",  4096, NULL, 5, NULL);
}
