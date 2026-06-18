# z_pub.c — ESP32-S3 Zenoh Publisher Tutorial

## Overview

`z_pub.c` is a Zenoh publisher example for the **ESP32-S3**, built on **ESP-IDF v6.0**. It demonstrates how an embedded device joins a Zenoh network over WiFi and publishes messages at a fixed interval.

### Key Features

| Feature | Description |
|---------|-------------|
| WiFi STA Connection | Connects to a specified SSID with automatic retry (up to 5 times) |
| Zenoh Session | Opens a Zenoh session in Client or Peer mode |
| Message Publishing | Publishes an incrementing counter message every second to a key expression |
| Error Handling | Auto-erases NVS on init failure; exits immediately on Zenoh errors |

### Data Flow

```
[ESP32-S3] --- WiFi STA ---> [WiFi AP] ----> [Zenoh Network] ---> (Subscribers receive)
    │                                  │
    │ every 1 second                    │
    │ publishes "[N] [ESPIDF]{ESP32} "  │
    └──────────────────────────────────┘
```

---

## Prerequisites

### Hardware

- ESP32-S3 development board (e.g., ESP32-S3-DevKitC-1)
- USB-C cable (power and serial)

### Software

| Tool | Version | Purpose |
|------|---------|---------|
| ESP-IDF | v6.0.1 | Embedded development framework |
| zenoh-pico | v1.9.0 | Zenoh protocol stack (C library, embedded-friendly) |
| xtensa-esp32s3-elf-gcc | — | Cross-compiler toolchain |
| clang-format | — | Code formatter (optional) |

### Network

- A 2.4 GHz WiFi access point (SSID + password)
- A Zenoh router (`zenohd`) or a subscriber running on the same network to receive messages

---

## Code Walkthrough

The following sections explain each part of the source code in the order it appears.

### 1. License Header

```c
// Copyright (c) 2022 ZettaScale Technology
// ...
// SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
```

Written by the ZettaScale Zenoh team, dual-licensed under **Eclipse Public License 2.0** or **Apache License 2.0**.

### 2. Include Headers

```c
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include <nvs_flash.h>
#include <zenoh-pico.h>

#include <freertos/FreeRTOS.h>
#include <freertos/event_groups.h>
#include <freertos/task.h>
```

Organised into four groups (enforced by `.clang-format` `IncludeCategories`):

| Group | Headers | Purpose |
|-------|---------|---------|
| ESP-IDF | `esp_event.h` / `esp_log.h` / `esp_system.h` / `esp_wifi.h` | Event loop, logging, system calls, WiFi driver |
| C Standard | `stdio.h` / `stdlib.h` / `string.h` / `unistd.h` | printf / exit / strcmp / sleep |
| Third-party | `nvs_flash.h` / `zenoh-pico.h` | Non-volatile storage, Zenoh pub/sub protocol |
| FreeRTOS | `FreeRTOS.h` / `event_groups.h` / `task.h` | RTOS kernel, event group synchronisation, tasks |

### 3. Compile-Time Guard: `#if Z_FEATURE_PUBLICATION == 1`

```c
#if Z_FEATURE_PUBLICATION == 1
// ... main body ...
#else
void app_main()
{
    printf("ERROR: ...\n");
}
#endif
```

This is a **compile-time guard**. zenoh-pico supports feature-based trimming — if the library was built without publication support (`Z_FEATURE_PUBLICATION != 1`), `app_main` degrades to an error message instead of failing at link time. **First-time users should verify this feature is enabled in `sdkconfig`.**

### 4. WiFi Configuration Macros

```c
#define ESP_WIFI_SSID "SSID"
#define ESP_WIFI_PASS "PASS"
#define ESP_MAXIMUM_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
```

- `ESP_WIFI_SSID` / `ESP_WIFI_PASS` — **Hardcoded** WiFi credentials. Must be edited before use.
- `ESP_MAXIMUM_RETRY` — Maximum reconnection attempts after a disconnect.
- `WIFI_CONNECTED_BIT` — FreeRTOS event group flag signalling that DHCP has assigned an IP address.

> ⚠️ **Security note**: Hardcoded credentials are unsuitable for production. Production deployments should read credentials from NVS or a config file.

### 5. Global State Variables

```c
static bool               s_is_wifi_connected = false;
static EventGroupHandle_t s_event_group_handler;
static int                s_retry_count = 0;
```

- `s_is_wifi_connected` — Set to `true` by the event handler on `GOT_IP`; polled by `app_main`.
- `s_event_group_handler` — FreeRTOS event group handle for synchronising between the event callback and the main task.
- `s_retry_count` — Current retry count; reset to 0 on `GOT_IP`.

### 6. Zenoh Mode Selection

```c
#define CLIENT_OR_PEER 0 // 0: Client mode; 1: Peer mode
#if CLIENT_OR_PEER == 0
#define MODE "client"
#define LOCATOR ""
#elif CLIENT_OR_PEER == 1
#define MODE "peer"
#define LOCATOR "udp/224.0.0.225:7447#iface=en0"
#else
#error "Unknown Zenoh operation mode."
#endif
```

| Mode | Value | Description |
|------|-------|-------------|
| **Client** | `CLIENT_OR_PEER=0` | Connects to a Zenoh router (`zenohd`). Requires a running router on the network. With an empty `LOCATOR`, auto-discovers the router via UDP multicast scouting. |
| **Peer** | `CLIENT_OR_PEER=1` | Communicates directly with other endpoints via UDP multicast — no router needed. The `LOCATOR` specifies a multicast address and network interface (adjust `en0` to your environment). |

**Recommendation**: start with **Client mode** — launch `zenohd` first, then run the device.

### 7. Topic and Message Template

```c
#define KEYEXPR "demo/example/zenoh-pico-pub"
#define VALUE "[ESPIDF]{ESP32} Publication from Zenoh-Pico!"
```

- `KEYEXPR` — The Zenoh key expression (topic path). Subscribers use the same key expression to receive messages.
- `VALUE` — Message template, prefixed with an incrementing index at runtime.

### 8. WiFi Event Handler

```c
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
```

ESP-IDF's event system forwards WiFi state changes to this callback. Three key events:

| Event | Trigger | Handler Logic |
|-------|---------|---------------|
| `WIFI_EVENT_STA_START` | WiFi driver initialised | Calls `esp_wifi_connect()` to begin connection |
| `WIFI_EVENT_STA_DISCONNECTED` | Disconnected from AP | Retries `esp_wifi_connect()` if retry count is below the limit, then increments counter |
| `IP_EVENT_STA_GOT_IP` | DHCP lease obtained | Sets `WIFI_CONNECTED_BIT` to wake `app_main`, resets retry count |

> The handler instances (`handler_got_ip`, `handler_any_id`) are unregistered after the connection succeeds to avoid unnecessary callbacks.

### 9. `wifi_init_sta()` — WiFi Initialisation

This is the standard ESP-IDF STA initialisation sequence in 5 steps:

**Step 1 — Create Event Group**
```c
s_event_group_handler = xEventGroupCreate();
```
Used to block until WiFi connects.

**Step 2 — Initialise Network Interface**
```c
esp_netif_init();
esp_event_loop_create_default();
esp_netif_create_default_wifi_sta();
```
Creates the default event loop and WiFi STA network interface.

**Step 3 — Initialise WiFi Driver**
```c
wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
esp_wifi_init(&config);
```

**Step 4 — Register Event Handlers**
```c
esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, ...);
esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ...);
```
Handlers are registered *before* starting WiFi to prevent missing early events.

**Step 5 — Configure and Start**
```c
wifi_config_t wifi_config = {
    .sta = { .ssid = ESP_WIFI_SSID, .password = ESP_WIFI_PASS }
};
esp_wifi_set_mode(WIFI_MODE_STA);
esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
esp_wifi_start();
```

**Blocking Wait:**
```c
xEventGroupWaitBits(s_event_group_handler, WIFI_CONNECTED_BIT,
                    pdFALSE, pdFALSE, portMAX_DELAY);
```
`portMAX_DELAY` means **wait indefinitely** — until DHCP assigns an IP or the system resets.

**Cleanup:**
```c
esp_event_handler_instance_unregister(...)  // unregister callbacks
vEventGroupDelete(s_event_group_handler);    // delete event group
```
These resources are no longer needed once connected.

### 10. `app_main()` — Entry Point

As the FreeRTOS application entry point, `app_main` executes sequentially:

```
┌─────────────────────────────┐
│  Initialise NVS             │
│  (nvs_flash_init)           │
└──────────┬──────────────────┘
           ↓
┌─────────────────────────────┐
│  Connect WiFi               │
│  (wifi_init_sta)            │
│  Blocks until GOT_IP        │
└──────────┬──────────────────┘
           ↓
┌─────────────────────────────┐
│  Open Zenoh Session         │
│  (z_open)                   │
│  Client/Peer from macros    │
└──────────┬──────────────────┘
           ↓
┌─────────────────────────────┐
│  Declare Publisher          │
│  (z_declare_publisher)      │
│  KEYEXPR → "demo/example/"  │
└──────────┬──────────────────┘
           ↓
┌─────────────────────────────┐
│  Infinite Publish Loop      │
│  sleep(1) → publish → inc   │
└─────────────────────────────┘
```

#### 10.1 NVS Initialisation

```c
esp_err_t ret = nvs_flash_init();
if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
    ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
}
ESP_ERROR_CHECK(ret);
```

NVS (Non-Volatile Storage) is the ESP-IDF flash partition used for WiFi calibration data, MAC addresses, etc. Two special errors trigger an erase-and-retry:
- `ESP_ERR_NVS_NO_FREE_PAGES` — Partition is full
- `ESP_ERR_NVS_NEW_VERSION_FOUND` — NVS format version mismatch (common after IDF upgrades)

#### 10.2 WiFi Connection (with Polling Fallback)

```c
printf("Connecting to WiFi...");
wifi_init_sta();
while (!s_is_wifi_connected) {
    printf(".");
    sleep(1);
}
printf("OK!\n");
```

Although `wifi_init_sta()` blocks internally via the event group, this polling loop acts as a **secondary safeguard**. If the event mechanism fails to wake in some edge case, this loop ensures execution does not proceed without WiFi.

#### 10.3 Building the Zenoh Configuration

```c
z_owned_config_t config;
z_config_default(&config);
zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, MODE);
if (strcmp(LOCATOR, "") != 0) {
    if (strcmp(MODE, "client") == 0)
        zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, LOCATOR);
    else
        zp_config_insert(z_loan_mut(config), Z_CONFIG_LISTEN_KEY, LOCATOR);
}
```

- `z_owned_config_t` — An "owned" Zenoh type; its memory is managed by the current scope.
- `z_loan_mut(config)` — Borrows a mutable reference from the owned type.
- `Z_CONFIG_MODE_KEY` — Sets "client" or "peer".
- When `LOCATOR` is non-empty, it either becomes the connect target (`Z_CONFIG_CONNECT_KEY`) or the listen endpoint (`Z_CONFIG_LISTEN_KEY`) depending on the mode.

In Client mode with an empty `LOCATOR`, zenoh-pico uses **UDP multicast scouting** to auto-discover the router — no manual address needed.

#### 10.4 Opening the Zenoh Session

```c
z_owned_session_t s;
if (z_open(&s, z_move(config), NULL) < 0) {
    printf("Unable to open session!\n");
    exit(-1);
}
```

- `z_move(config)` — Transfers **ownership** of the config into the session. After a successful `z_open`, `config` is no longer valid. This follows Zenoh's move semantics to avoid unnecessary copying.
- On failure (network unreachable, no router found) the program calls `exit(-1)`.

#### 10.5 Declaring the Publisher

```c
z_owned_publisher_t pub;
z_view_keyexpr_t ke;
z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);
if (z_declare_publisher(z_loan(s), &pub, z_loan(ke), NULL) < 0) {
    printf("Unable to declare publisher for key expression!\n");
    exit(-1);
}
```

- `z_view_keyexpr_t` — A **view type** that borrows the key expression string without owning it. The `_unchecked` suffix skips key expression validation.
- `z_declare_publisher` — Registers this node as a publisher for the topic with the Zenoh network (via the router or directly with peers). Failure typically indicates a network issue.

#### 10.6 Publish Loop

```c
char buf[256];
for (int idx = 0; 1; ++idx) {
    sleep(1);
    sprintf(buf, "[%4d] %s", idx, VALUE);
    printf("Putting Data ('%s': '%s')...\n", KEYEXPR, buf);

    z_owned_bytes_t payload;
    z_bytes_copy_from_str(&payload, buf);
    z_publisher_put(z_loan(pub), z_move(payload), NULL);
}
```

This is the core publishing logic:

1. `sleep(1)` — Publishes once per second
2. `sprintf(buf, "[%4d] %s", idx, VALUE)` — Composes the message, e.g. `" [  42] [ESPIDF]{ESP32} Publication from Zenoh-Pico!"`
3. `z_bytes_copy_from_str` — Copies the string into Zenoh's `Bytes` type
4. `z_publisher_put` — Publishes the data to the key expression; subscribers receive this payload

> `for (int idx = 0; 1; ++idx)` is an infinite loop — the condition `1` is always true. The cleanup code after the comment `// Unreachable` never executes in normal operation.

---

## Build & Flash

### 1. Configure WiFi Credentials

Edit `main/z_pub.c` — update the macros:

```c
#define ESP_WIFI_SSID "Your_SSID"
#define ESP_WIFI_PASS "Your_Password"
```

### 2. Select Zenoh Mode (optional)

```c
#define CLIENT_OR_PEER 0   // Client mode (recommended)
#define CLIENT_OR_PEER 1   // Peer mode
```

**Client mode** requires a running `zenohd` on the same network:

```bash
# Terminal 1: Start the Zenoh router
zenohd
```

**Peer mode** does not need a router, but the multicast address and interface name must match your environment.

### 3. Build

```bash
idf.py build
```

### 4. Flash

```bash
idf.py flash
```

### 5. Monitor Serial Output

```bash
idf.py monitor
```

Expected output:

```
Connecting to WiFi....OK!
Opening Zenoh Session...OK
Declaring publisher for 'demo/example/zenoh-pico-pub'...OK
Putting Data ('demo/example/zenoh-pico-pub': '[   0] [ESPIDF]{ESP32} Publication from Zenoh-Pico!')...
Putting Data ('demo/example/zenoh-pico-pub': '[   1] [ESPIDF]{ESP32} Publication from Zenoh-Pico!')...
Putting Data ('demo/example/zenoh-pico-pub': '[   2] [ESPIDF]{ESP32} Publication from Zenoh-Pico!')...
```

Press `Ctrl+]` to exit the monitor.

---

## Receiving Messages (Subscriber Side)

Run a subscriber on a PC or another ESP32 on the same network:

```bash
# Using the zenoh CLI
zenoh sub -k "demo/example/zenoh-pico-pub"
```

If using another ESP32 running a subscriber program, ensure it uses the same `KEYEXPR` and the same Zenoh network mode.

---

## Customisation Guide

### Change Publish Interval

Replace `sleep(1)` with a different value (seconds):

```c
sleep(5);  // publish every 5 seconds
```

For sub-second intervals, use FreeRTOS's `vTaskDelay(pdMS_TO_TICKS(500))` instead of `sleep`.

### Change Message Content

```c
#define VALUE "[ESPIDF]{ESP32} Temperature sensor data"
```

Or use dynamic data in the loop:

```c
float temp = read_temperature_sensor();
sprintf(buf, "{\"temp\": %.2f, \"idx\": %d}", temp, idx);
```

### Change Topic

```c
#define KEYEXPR "sensor/temperature/room1"
```

Ensure subscribers use the same key expression.

### Scouting vs. Manual Endpoint

- **Scouting (auto-discovery, default)**: Leave `LOCATOR` empty. The router must have scouting responses enabled.
- **Manual endpoint**: Set `LOCATOR` to the router address:
  ```c
  #define LOCATOR "tcp/192.168.1.100:7447"
  ```
  This is more reliable and avoids multicast network limitations.

---

## Troubleshooting

### ❌ `Unable to open session!`

| Possible Cause | Solution |
|----------------|----------|
| `zenohd` not running (Client mode) | Start `zenohd` in a terminal |
| WiFi connection failed | Check SSID and password |
| Firewall blocking UDP 7447 | Open the port or allow multicast |
| Wrong network interface (Peer mode) | Change `en0` to the actual interface name (`eth0`, `wlan0`, `以太网`, etc.) |

### ❌ `Unable to declare publisher for key expression!`

Usually occurs right after opening the session. Possible reasons:
- Router-side permission restrictions
- Unstable network causing session disconnection

### ❌ Garbled Serial Output

```
ESP-ROM:esp32s3-xxxxxxxx
```
Check that `idf.py monitor` uses the correct baud rate (115200 by default), or press `Ctrl+T` → `Ctrl+Y` to reset the board.

### ❌ Compilation Error: `Z_FEATURE_PUBLICATION` undefined

Ensure the feature is enabled in `sdkconfig`:

```bash
idf.py menuconfig
# → Navigate to "Component config → Zenoh pico → Enable publication feature"
# Verify it is checked
```

---

## Reference Resources

| Resource | Link |
|----------|------|
| Zenoh Documentation | https://zenoh.io/docs/ |
| zenoh-pico API | https://zenoh.io/docs/apis/zenoh-pico/ |
| ESP-IDF Programming Guide | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/ |
| Eclipse Public License 2.0 | https://www.eclipse.org/legal/epl-2.0/ |

---

## License

The source code corresponding to this document is dual-licensed under Eclipse Public License 2.0 or Apache License 2.0 — see the file header for details.
