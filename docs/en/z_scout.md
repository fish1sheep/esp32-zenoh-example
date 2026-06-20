# z_scout.md — ESP32 (S3 / C5) Zenoh Scout (Network Discovery) Tutorial

[← Back to docs](../README.md)

---

## Overview

`main/z_scout.c` is a Zenoh **scout** example for the **ESP32 (S3 / C5)**, built on **ESP-IDF v6.0**. It discovers all Zenoh nodes (routers, peers, clients) on the local network by sending a UDP multicast query and printing the `Hello` replies.

### What is Scouting?

Zenoh scouting is the equivalent of shouting **"Is anyone using Zenoh on this network?"** and listening for responses. Each Zenoh node replies with a **Hello** message containing:

| Hello Field | Description | Example |
|-------------|-------------|---------|
| **ZID** | Zenoh ID — a globally unique 128-bit identifier | `Some(A1B2C3D4E5F6...)` |
| **WhatAmI** | Node role | `"router"`, `"peer"`, or `"client"` |
| **Locators** | Communication endpoints | `["tcp/192.168.1.100:7447"]` |

### Scout vs. Ping

| | Ping | Zenoh Scout |
|---|---|---|
| What it checks | IP reachability | Zenoh protocol availability |
| Response | ICMP echo reply | Hello with ZID, role, locators |
| Requires | IP address of target | No address needed (multicast) |
| Use case | Network connectivity | Zenoh network topology discovery |

### Key Features

| Feature | Description |
|---------|-------------|
| WiFi STA Connection | Connects to a specified SSID with automatic retry (up to 5 times) |
| Zenoh Scout | Sends a UDP multicast scout query and collects Hello replies |
| Pretty-Printing | Custom print functions for ZID, WhatAmI, locators, and Hello messages |
| Asynchronous Callbacks | Scout runs in the background; results arrive via callbacks |
| Counter Tracking | Counts how many Hello replies were received |

### Data Flow

```
[ESP32 (S3 / C5)] --- WiFi STA ---> [WiFi AP]
    │
    │  "Anyone using Zenoh?"  (UDP multicast scout)
    │
    ├──<── Hello {zid: A1B2..., whatami: "router", locators: ["tcp/..."]}
    ├──<── Hello {zid: C3D4..., whatami: "peer",   locators: ["udp/..."]}
    │
    ▼
Serial Console prints each Hello with formatted details
```

---

## Prerequisites

### Hardware

- ESP32 development board (ESP32-S3-DevKitC-1 or ESP32-C5-DevKitC)
- USB-C cable (power and serial)

### Software

| Tool | Version | Purpose |
|------|---------|---------|
| ESP-IDF | v6.0.1 | Embedded development framework |
| zenoh-pico | v1.9.0 | Zenoh protocol stack (must have scouting enabled) |
| xtensa-esp32s3-elf-gcc | — | Cross-compiler toolchain |

### Network

- A 2.4 GHz WiFi access point (SSID + password)
- At least one Zenoh node (a running `zenohd` or another Zenoh device) on the same network segment — scouting uses UDP multicast, which typically stays within the local subnet

---

## Code Walkthrough

The following sections explain each part of the source code in the order it appears.

### 1. License Header & Summary

```c
// Copyright (c) 2022 ZettaScale Technology
// SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
```

Dual-licensed under **Eclipse Public License 2.0** or **Apache License 2.0**.

The block comment at the top explains the purpose, flow, and distinction between Scout and Ping — read it first to orient yourself in the code.

### 2. Include Headers

```c
#include <esp_event.h>    /* ESP-IDF event loop */
#include <esp_log.h>      /* ESP-IDF logging */
#include <esp_system.h>   /* ESP-IDF system API */
#include <esp_wifi.h>     /* WiFi driver */

#include <stdio.h>   /* printf / fprintf */
#include <stdlib.h>  /* malloc / free */
#include <string.h>  /* string manipulation */

#include <nvs_flash.h>   /* NVS flash storage */
#include <unistd.h>      /* sleep() */
#include <zenoh-pico.h>  /* Zenoh client library */

#include <freertos/FreeRTOS.h>     /* FreeRTOS kernel */
#include <freertos/event_groups.h> /* Event groups */
#include <freertos/task.h>         /* Task API */
```

Organised into four groups: **ESP-IDF**, **C Standard**, **Third-party**, **FreeRTOS** (enforced by `.clang-format` `IncludeCategories`).

### 3. Compile-Time Guard: `#if Z_FEATURE_SCOUTING == 1`

```c
#if Z_FEATURE_SCOUTING == 1
// ... main body (all scout logic) ...
#else
void app_main() { printf("ERROR: ...\n"); }
#endif
```

Scouting can be disabled during the zenoh-pico build to save firmware space. This guard ensures a clear error message rather than a cryptic link failure. **Check `sdkconfig` if compilation fails:**

```
idf.py menuconfig
→ Component config → Zenoh pico → Enable scouting feature
```

### 4. WiFi Configuration Macros

```c
#define ESP_WIFI_SSID "Your_SSID"    // ← Replace with your WiFi SSID
#define ESP_WIFI_PASS "Your_Password" // ← Replace with your WiFi password
#define ESP_MAXIMUM_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
```

| Macro | Meaning |
|-------|---------|
| `ESP_WIFI_SSID` | WiFi access point name (2.4 GHz — ESP32-S3 does not support 5 GHz; ESP32-C5 supports 5 GHz but is used on 2.4 GHz here) |
| `ESP_WIFI_PASS` | WiFi password |
| `ESP_MAXIMUM_RETRY` | Max reconnection attempts after a disconnect |
| `WIFI_CONNECTED_BIT` | Event group bit (`BIT0 = 0x01`) signalling that DHCP has assigned an IP |

> ⚠️ **Security note**: The credentials shown here are example values from development. Never commit real credentials to source control. Use NVS or ESP-IDF's `wifi_provisioning` for production.

### 5. Global State Variables

```c
static bool               s_is_wifi_connected = false;
static EventGroupHandle_t s_event_group_handler;
static int                s_retry_count = 0;
```

File-scope variables shared between the WiFi event handler and `app_main`:

| Variable | Purpose |
|----------|---------|
| `s_is_wifi_connected` | Polled by `app_main`; set to `true` when DHCP succeeds |
| `s_event_group_handler` | FreeRTOS event group — wakes the main task from blocking wait |
| `s_retry_count` | Tracks consecutive reconnect attempts; stops at `ESP_MAXIMUM_RETRY` |

### 6. WiFi Event Handler

```c
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
```

ESP-IDF's event system calls this callback on WiFi state transitions:

| Event | Cause | Response |
|-------|-------|----------|
| `WIFI_EVENT_STA_START` | WiFi driver initialised | Call `esp_wifi_connect()` to begin association |
| `WIFI_EVENT_STA_DISCONNECTED` | Link lost (AP down, signal weak, etc.) | Retry up to `ESP_MAXIMUM_RETRY` times |
| `IP_EVENT_STA_GOT_IP` | DHCP assigned an IP address | Set event group bit, reset retry counter |

**Why wait for `GOT_IP` instead of `CONNECTED`?** The `CONNECTED` event fires when the WiFi association completes (Layer 2). But you don't have an IP address yet — `GOT_IP` fires only after DHCP succeeds (Layer 3), which is when you can actually use network sockets.

### 7. `wifi_init_sta()` — WiFi Initialisation (Blocking)

This function blocks the calling task until the ESP32 obtains an IP address.

```
xEventGroupCreate()
       │
esp_netif_init()
esp_event_loop_create_default()
esp_netif_create_default_wifi_sta()
       │
esp_wifi_init(&config)
       │
Register event_handler for WIFI_EVENT + IP_EVENT
       │
esp_wifi_set_mode(WIFI_MODE_STA)
esp_wifi_set_config(...)
esp_wifi_start()
       │
xEventGroupWaitBits(... portMAX_DELAY)  ← BLOCKS HERE
       │
Unregister handlers + delete event group
       │
s_is_wifi_connected = true
```

#### Step-by-step:

**Event group creation:**
```c
s_event_group_handler = xEventGroupCreate();
```
A FreeRTOS event group lets the WiFi event callback (running in interrupt/event context) signal the waiting task.

**Network stack:**
```c
esp_netif_init();
esp_event_loop_create_default();
esp_netif_create_default_wifi_sta();
```
Initialises lwIP (the TCP/IP stack), the system event loop, and the STA network interface.

**WiFi driver:**
```c
wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
esp_wifi_init(&config);
```

**Register handlers BEFORE starting WiFi:**
```c
esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, ...);
esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ...);
```
If you started WiFi first, you might miss the `STA_START` event.

**Configure + Start:**
```c
wifi_config_t wifi_config = {
    .sta = {
        .ssid = ESP_WIFI_SSID,
        .password = ESP_WIFI_PASS,
    }
};
esp_wifi_set_mode(WIFI_MODE_STA);
esp_wifi_set_config(WIFI_IF_STA, &wifi_config);
esp_wifi_start();
```

**Blocking wait:**
```c
EventBits_t bits = xEventGroupWaitBits(
    s_event_group_handler, WIFI_CONNECTED_BIT,
    pdFALSE, pdFALSE, portMAX_DELAY);
```
`portMAX_DELAY` = wait forever. The task consumes **zero CPU** while blocked — FreeRTOS runs other tasks (or the idle task).

**Cleanup:**
```c
esp_event_handler_instance_unregister(...);  // remove temporary handlers
vEventGroupDelete(s_event_group_handler);     // free event group memory
```

### 8. `fprintzid()` — Zenoh ID Pretty-Print

```c
void fprintzid(FILE *stream, z_id_t zid)
```

`z_id_t` is a 128-bit globally unique identifier stored as a raw byte array. This function formats it as an uppercase hex string.

```
Input:  {0xA1, 0xB2, 0xC3, 0xD4, ...}
Output: Some(A1B2C3D4E5F6...)

Input:  ZID length == 0
Output: None
```

The iteration `for (unsigned int i = 0; i < zidlen; i++)` prints each byte with `%02X` (uppercase, zero-padded).

### 9. `fprintwhatami()` — Node Role Pretty-Print

```c
void fprintwhatami(FILE *stream, z_whatami_t whatami)
```

Converts the `z_whatami_t` enum to a human-readable string:

```c
z_view_string_t s;
z_whatami_to_view_string(whatami, &s);
fprintf(stream, "\"%.*s\"", ...);
```

| Enum Value | Printed Output |
|------------|----------------|
| `Z_WHATAMI_ROUTER` | `"router"` |
| `Z_WHATAMI_PEER` | `"peer"` |
| `Z_WHATAMI_CLIENT` | `"client"` |

The `%.*s` format takes a `(length, pointer)` pair — safe even if the internal string isn't null-terminated.

### 10. `fprintlocators()` — Locator Array Pretty-Print

```c
void fprintlocators(FILE *stream, const z_loaned_string_array_t *locs)
```

A locator is a Zenoh communication endpoint. This function prints the array as a JSON-style list:

```
Input:  ["tcp/192.168.1.100:7447", "udp/192.168.1.100:7447"]
Output: ["tcp/192.168.1.100:7447", "udp/192.168.1.100:7447"]
```

The comma separator logic:
```c
if (i < z_string_array_len(locs) - 1)
    fprintf(stream, ", ");
```
This avoids a trailing comma after the last element.

### 11. `fprinthello()` — Full Hello Message Pretty-Print

```c
void fprinthello(FILE *stream, const z_loaned_hello_t *hello)
```

Combines the three printers above into one formatted output:

```
Hello { zid: Some(A1B2C3D4E5F6), whatami: "router", locators: ["tcp/192.168.1.100:7447"] }
```

This function is called by the scout callback every time a Hello is received.

### 12. `callback()` — Scout Reply Callback

```c
void callback(z_loaned_hello_t *hello, void *context)
```

Called by Zenoh each time a node replies to the scout query:

```c
fprinthello(stdout, hello);    // Print the Hello to serial
fprintf(stdout, "\n");
(*(int *)context)++;            // Increment the reply counter
```

The `context` pointer is the `int *` counter allocated in `app_main`.

### 13. `drop()` — Scout End Callback

```c
void drop(void *context)
```

Called when the scout operation completes (timeout or cancellation):

```c
int count = *(int *)context;
free(context);
if (!count)
    printf("Did not find any zenoh process.\n");
else
    printf("Dropping scout results.\n");
```

This is where the cleanup happens:
- **Frees** the counter memory allocated in `app_main`
- **Prints** a summary — either "found N nodes" or "found nothing"
- The `if (!count)` check is especially helpful for debugging: if the network has no Zenoh nodes, you get a clear message instead of silent failure

### 14. `app_main()` — Entry Point

The application runs through 3 main steps:

```
┌─────────────────────────────────────────────┐
│  Step 1: NVS initialisation                 │
│  (nvs_flash_init)                           │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 2: WiFi connection (blocking)         │
│  (wifi_init_sta)                            │
│  → blocks until DHCP assigns an IP          │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 3: Zenoh Scout                        │
│  1. Allocate context counter                │
│  2. Create default Zenoh config             │
│  3. Create closure (callback + drop)        │
│  4. Call z_scout() — UDP multicast          │
│  → prints Hello replies asynchronously      │
└─────────────────────────────────────────────┘
```

#### Step 1 — NVS Initialisation

```c
esp_err_t ret = nvs_flash_init();
if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
    ret == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    ret = nvs_flash_init();
}
ESP_ERROR_CHECK(ret);
```

NVS (Non-Volatile Storage) stores WiFi calibration data and DHCP client configuration. The erase-and-retry pattern handles partition format changes after IDF upgrades.

#### Step 2 — WiFi Connection

```c
printf("Connecting to WiFi...");
wifi_init_sta();
while (!s_is_wifi_connected) {
    printf(".");
    sleep(1);
}
printf("OK!\n");
```

The polling loop is a **secondary safeguard** — if the event group mechanism fails for any reason, execution won't proceed without WiFi.

#### Step 3 — Zenoh Scout

```c
int *context = (int *)malloc(sizeof(int));
*context = 0;

z_owned_config_t config;
z_config_default(&config);   // Use default config

z_owned_closure_hello_t closure;
z_closure_hello(&closure, callback, drop, context);

printf("Scouting...\n");
z_scout(z_config_move(&config), z_closure_hello_move(&closure), NULL);
```

This is the core scout operation:

1. **Context allocation** — a heap-allocated counter that tracks how many Hello replies we receive
2. **Default config** — scouting uses default parameters (UDP multicast on port 7446)
3. **Closure construction** — binds `callback` (on each reply) and `drop` (on completion) with the context
4. **`z_scout()`** — sends the UDP multicast scout query and returns immediately

**Asynchronous behaviour:** `z_scout()` does NOT block. It sends the query, registers the callbacks, and returns. Zenoh's transport layer collects replies in the background. The `drop` function fires when the collection times out (default ~1 second).

**Third parameter (`NULL`):** This is an optional scout configuration — passing `NULL` uses defaults. You could pass a config here to tune scout behaviour (e.g., scout only a specific subnet).

**Important caveat:** After `z_scout()` returns, `app_main` also returns immediately. In a simple example like this, the program may terminate before all scout replies arrive. In production, you'd use a semaphore or a longer sleep to keep the task alive until `drop` fires.

---

## Build & Flash

### 1. Configure WiFi Credentials

Update the WiFi credentials in `main/z_scout.c` to match your own network:

```c
#define ESP_WIFI_SSID "Your_SSID"       // ← Your 2.4 GHz WiFi SSID
#define ESP_WIFI_PASS "Your_Password"   // ← Your WiFi password
```

### 2. Ensure Scouting is Enabled

```bash
idf.py menuconfig
# → Component config → Zenoh pico → Enable scouting feature
# Verify this is CHECKED
```

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

Expected output (with a Zenoh router running on the same network):

```
Connecting to WiFi...OK!
Scouting...
Hello { zid: Some(0123456789ABCDEF0123456789ABCDEF), whatami: "router", locators: ["tcp/192.168.1.100:7447"] }
Dropping scout results.
```

If no Zenoh nodes are present:

```
Connecting to WiFi...OK!
Scouting...
Did not find any zenoh process.
```

Press `Ctrl+]` to exit the monitor.

---

## Testing with Zenoh Nodes

### Run Another ESP32 Example (Recommended)

Running `z_pub.c` or `z_sub.c` on another ESP32 (S3 / C5) on the same network will make it discoverable via scouting (if they're in peer mode or using scouting to find a router).

### Run a Zenoh Router or Peer

For a more complete test, start a Zenoh router or peer on a PC connected to the same WiFi. The scout will discover these nodes and print their Hello messages.

---

## Customisation Guide

### Change Scout Timeout

The scout timeout controls how long Zenoh waits for Hello replies. To change it, pass a scout configuration instead of `NULL`:

```c
z_owned_scouting_config_t scout_config;
z_scouting_config_default(&scout_config);
zp_scouting_config_insert(z_loan_mut(scout_config), "timeout", "5000");
z_scout(z_config_move(&config), z_closure_hello_move(&closure), z_loan(scout_config));
```

### Filter by Node Role

You can ignore certain node types in the callback:

```c
void callback(z_loaned_hello_t *hello, void *context) {
    z_whatami_t role = z_hello_whatami(hello);
    if (role == Z_WHATAMI_ROUTER) {
        fprinthello(stdout, hello);
        fprintf(stdout, "\n");
        (*(int *)context)++;
    }
}
```

### Keep `app_main` Alive for Async Completion

Since `z_scout()` is asynchronous, the program might exit before `drop()` fires. A simple fix:

```c
printf("Scouting...\n");
z_scout(z_config_move(&config), z_closure_hello_move(&closure), NULL);

// Wait for scout to complete (adjust sleep duration as needed)
sleep(2);
```

---

## Troubleshooting

### ❌ `Z_FEATURE_SCOUTING` undefined

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable scouting feature
```
Ensure it's checked, then rebuild.

### ❌ `Did not find any zenoh process.`

| Possible Cause | Solution |
|----------------|----------|
| No Zenoh node on the network | Start `zenohd` or another Zenoh device |
| Firewall blocking UDP 7446 | Open UDP port 7446 (scout port) |
| WiFi not connected | Check SSID/password and serial output for WiFi status |
| Different subnet | Scouting uses multicast — both devices must be on the same subnet |

### ❌ Serial output shows only `Scouting...` then nothing

The scout may have completed before the Hello arrived. Increase the implicit timeout, or add a `sleep(2)` after `z_scout()` (see Customisation section).

### ❌ Garbled serial output

Press the ESP32 (S3 / C5) reset button, or press `Ctrl+T` → `Ctrl+Y` in the monitor to reset the board.

### ❌ Compilation Error: `z_scout` not found

Verify that:
1. `Z_FEATURE_SCOUTING` is enabled in `sdkconfig`
2. `zenoh-pico` is listed in `REQUIRES` in `main/CMakeLists.txt`

---

## Reference Resources

| Resource | Link |
|----------|------|
| Zenoh Scouting Concept | https://zenoh.io/docs/manual/abstractions/#scouting |
| zenoh-pico API | https://zenoh-pico.readthedocs.io/en/1.9.0/ |
| ESP-IDF WiFi Driver | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp_wifi.html |
| ESP-IDF Event Loop | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/system/esp_event.html |
| FreeRTOS Event Groups | https://www.freertos.org/event-groups.html |
| Eclipse Public License 2.0 | https://www.eclipse.org/legal/epl-2.0/ |

---

## License

The source code corresponding to this document is dual-licensed under Eclipse Public License 2.0 or Apache License 2.0 — see the file header for details.
