# BLE on ESP32-H2 (NimBLE) — Reference

> Adding BLE GATT Client alongside an existing Zigbee stack on the NanoH2.
> Treadmill protocol is already known. This covers the BLE plumbing only.

---

## NimBLE over Bluedroid — Why

The H2 has no Bluetooth Classic, only BLE. NimBLE is BLE-only and uses ~170 KB less flash and significantly less RAM than Bluedroid. Espressif recommends NimBLE for BLE-only chips. On an H2 already running Zigbee, the smaller footprint matters.

- BLE stack overview: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/ble/overview.html
- NimBLE API (H2): https://docs.espressif.com/projects/esp-idf/en/stable/esp32h2/api-reference/bluetooth/nimble/index.html

---

## GATT Client Basics

Your H2 is the **Central / GATT Client**. The treadmill is the **Peripheral / GATT Server**.

**Connection without scanning** — if you have the peer's MAC, you can call `ble_gap_connect()` directly and skip scanning entirely. You still need to do service discovery once per connection to resolve characteristic handles (they can change between firmware versions on the peripheral).

**Writes:** `ble_gattc_write_flat()` (with response) or `ble_gattc_write_no_rsp_flat()` (fire-and-forget). Both take a connection handle + characteristic handle + data buffer.

**Reads:** `ble_gattc_read()` triggers a callback with the value.

**Notifications:** Write `{0x01, 0x00}` to the characteristic's CCCD descriptor handle to enable. Incoming data arrives via `BLE_GAP_EVENT_NOTIFY_RX` in the GAP event callback. For indications, write `{0x02, 0x00}` instead.

**MTU:** Default is 23 bytes. Call `ble_gattc_exchange_mtu()` after connecting if you need larger payloads. The negotiated MTU fires `BLE_GAP_EVENT_MTU`.

---

## NimBLE Init Sequence

```c
nimble_port_init();

// Host config
ble_hs_cfg.sync_cb  = on_sync;   // called when host<->controller synced — start connecting here
ble_hs_cfg.reset_cb = on_reset;

ble_svc_gap_init();
ble_svc_gatt_init();
nimble_port_freertos_init(nimble_host_task_fn);  // starts the host task
```

NimBLE runs its own FreeRTOS task. All BLE events come in on that task via callbacks. Use queues/event groups to communicate with your Zigbee/sensor tasks.

---

## Connecting by Address

```c
struct ble_gap_conn_params params = {
    .scan_itvl = 0x0010,
    .scan_window = 0x0010,
    .itvl_min = 40,    // 50 ms  (units of 1.25 ms)
    .itvl_max = 80,    // 100 ms
    .latency = 0,
    .supervision_timeout = 400,  // 4 s (units of 10 ms)
};

ble_addr_t addr = {
    .type = BLE_ADDR_PUBLIC,  // or BLE_ADDR_RANDOM
    .val = { /* MAC in reverse byte order */ }
};

ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &addr, 30000, &params, gap_event_cb, NULL);
```

Connection result arrives as `BLE_GAP_EVENT_CONNECT`. Disconnects as `BLE_GAP_EVENT_DISCONNECT` with a reason code.

The connection interval params above (50–100 ms) are a reasonable middle ground — responsive enough for treadmill control, not so aggressive that it starves Zigbee.

---

## Service Discovery

After connecting, discover services to get characteristic handles:

```c
// All services
ble_gattc_disc_all_svcs(conn_handle, svc_cb, NULL);

// Or by known UUID
ble_gattc_disc_svc_by_uuid(conn_handle, &your_svc_uuid, svc_cb, NULL);

// Then characteristics within a service
ble_gattc_disc_all_chrs(conn_handle, start_handle, end_handle, chr_cb, NULL);

// Then descriptors (to find CCCD handles for notify/indicate)
ble_gattc_disc_all_dscs(conn_handle, chr_handle, end_handle, dsc_cb, NULL);
```

Cache the discovered handles — you don't need to rediscover unless the connection drops and the peripheral's GATT database could have changed.

---

## Zigbee + BLE Coexistence

Single 2.4 GHz radio, shared via time-division multiplexing. A coexistence arbiter assigns time slices by priority.

**Priority order (simplified):**
- 802.15.4 TX/ACK → higher priority
- BLE connection events → get dedicated time slices
- 802.15.4 normal RX → lowest priority, gets remaining time

For periodic command writes and notification reads alongside Zigbee, this is well within what the coexistence module handles automatically. No coexistence API calls needed from your code unless you're doing BLE Mesh (you're not).

### Required sdkconfig

```
CONFIG_BT_ENABLED=y
CONFIG_BT_NIMBLE_ENABLED=y
CONFIG_IEEE802154_ENABLED=y
CONFIG_ESP_COEX_SW_COEXIST_ENABLE=y

# Only enable the roles you need — saves memory
CONFIG_BT_NIMBLE_ROLE_CENTRAL=y
CONFIG_BT_NIMBLE_ROLE_OBSERVER=y
CONFIG_BT_NIMBLE_ROLE_PERIPHERAL=n
CONFIG_BT_NIMBLE_ROLE_BROADCASTER=n

# Optional: dynamic BLE memory allocation
CONFIG_BT_BLE_DYNAMIC_ENV_MEMORY=y
```

- Coexistence docs (H2 specific): https://docs.espressif.com/projects/esp-idf/en/stable/esp32h2/api-guides/coexist.html

---

## Key Examples

| Example | What it shows | Link |
|---|---|---|
| **blecent** | NimBLE central — scan, connect, discover, subscribe to notifications | [github](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/nimble/blecent) |
| **ble_spp/spp_client** | Bidirectional read/write over BLE | [github](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/nimble/ble_spp/spp_client) |
| **NimBLE_GATT_Server** | Get-started example, explicitly lists H2 as supported | [github](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/ble_get_started/nimble/NimBLE_GATT_Server) |
| **bleprph** | NimBLE peripheral with security/bonding config | [github](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/nimble/bleprph) |
| All NimBLE examples | Full list | [github](https://github.com/espressif/esp-idf/tree/master/examples/bluetooth/nimble) |

Community:
- Well-commented beginner NimBLE examples: https://github.com/Zeni241/ESP32-NimbleBLE-For-Dummies
- esp-nimble-cpp (C++ wrapper): https://h2zero.github.io/esp-nimble-cpp/

---

## API References

- **NimBLE GAP** (connection, params): https://mynewt.apache.org/latest/network/ble_hs/ble_gap.html
- **NimBLE GATTC** (service discovery, read, write, subscribe): https://mynewt.apache.org/latest/network/ble_hs/ble_gattc.html
- **NimBLE ATT**: https://mynewt.apache.org/latest/network/ble_hs/ble_att.html
- **ESP-IDF NimBLE reference** (H2): https://docs.espressif.com/projects/esp-idf/en/stable/esp32h2/api-reference/bluetooth/nimble/index.html
- **BLE data exchange walkthrough**: https://docs.espressif.com/projects/esp-idf/en/stable/esp32/api-guides/ble/get-started/ble-data-exchange.html

---

## Debugging

Set `CONFIG_BT_LOG_NIMBLE_HOST_LOG_LEVEL` to DEBUG for verbose NimBLE logs. The `blecent` example has good log output showing every step of discovery/subscription that's useful as a reference for what events to expect and in what order.
