# z_get.md — ESP32-S3 Zenoh Queryable (Request-Response) Tutorial

[← Back to docs](../README.md)

---

## Overview

`main/z_get.c` is a Zenoh **queryable** example for the **ESP32-S3**, built on **ESP-IDF v6.0**. It demonstrates the server side of Zenoh's request-response pattern — the ESP32 declares a queryable on a key expression and replies when other nodes send GET queries.

### What is a Queryable?

In Zenoh, a **Queryable** is analogous to an HTTP server endpoint:

```
[Client]  ─── GET "demo/example/zenoh-pico-queryable" ───→  [Queryable (ESP32)]
                └─ Payload: "query data?"                     │
                                                              │
                ←── Reply: "[ESPIDF]{ESP32} Queryable..." ────┘
```

Unlike a subscriber (which receives pushed data), a queryable **waits to be asked**. The requesting node sends a GET query, and the queryable's handler function runs to produce a reply.

### Key Features

| Feature | Description |
|---------|-------------|
| WiFi STA Connection | Connects to a specified SSID with automatic retry (up to 5 times) |
| Zenoh Session | Opens a Zenoh session in Client or Peer mode |
| Queryable Declaration | Registers a handler for `"demo/example/zenoh-pico-queryable"` |
| Request-Response | Logs incoming queries with key expression, parameters, and payload |
| Memory Safety | Properly drops owned strings to prevent leaks |
| Error Handling | Exits immediately on Zenoh errors with clear messages |

### Data Flow

```
[Zenoh Client]                          [ESP32-S3 Queryable]
      │                                        │
      │  GET "demo/example/zenoh-pico-queryable"
      │  ──────────────────────────────────────→│
      │                                         │ query_handler() fires:
      │                                         │  1. Prints query details
      │                                         │  2. Builds reply payload
      │                                         │  3. Calls z_query_reply()
      │  ←──────────────────────────────────────│
      │  Reply: [ESPIDF]{ESP32} Queryable...    │
      ▼                                        ▼
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
| zenoh-pico | v1.9.0 | Zenoh protocol stack (must have queryable enabled) |
| xtensa-esp32s3-elf-gcc | — | Cross-compiler toolchain |
| zenoh (CLI or Python) | — | For sending test queries from a PC |

### Network

- A 2.4 GHz WiFi access point (SSID + password)
- A Zenoh router (`zenohd`) **or** peer on the same network (for routing GET queries)

---

## Code Walkthrough

The following sections explain each part of the source code in the order it appears.

### 1. License Header

```c
// Copyright (c) 2022 ZettaScale Technology
// SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
```

Dual-licensed under **Eclipse Public License 2.0** or **Apache License 2.0**.

### 2. Include Headers

```c
#include <esp_event.h>    /* ESP-IDF event loop */
#include <esp_log.h>      /* ESP-IDF logging */
#include <esp_system.h>   /* ESP-IDF system API */
#include <esp_wifi.h>     /* WiFi driver */

#include <stdio.h>   /* printf / fprintf */
#include <stdlib.h>  /* malloc / free / exit */
#include <string.h>  /* strcmp, memset */

#include <nvs_flash.h>   /* NVS flash storage */
#include <unistd.h>      /* sleep() */
#include <zenoh-pico.h>  /* Zenoh client library */

#include <freertos/FreeRTOS.h>     /* FreeRTOS kernel */
#include <freertos/event_groups.h> /* Event groups */
#include <freertos/task.h>         /* Task API */
```

Four groups in ESP-IDF's include category order: **ESP-IDF**, **C Standard**, **Third-party**, **FreeRTOS**.

### 3. Compile-Time Guard: `#if Z_FEATURE_QUERYABLE == 1`

```c
#if Z_FEATURE_QUERYABLE == 1
// ... main body ...
#else
void app_main() { printf("ERROR: ...\n"); }
#endif
```

The queryable feature can be compiled out of zenoh-pico. **If you get a build error**, check:

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable queryable feature
```

### 4. WiFi Configuration Macros

```c
#define ESP_WIFI_SSID "SSID"
#define ESP_WIFI_PASS "PASS"
#define ESP_MAXIMUM_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
```

| Macro | Meaning |
|-------|---------|
| `ESP_WIFI_SSID` | Your WiFi access point name (2.4 GHz only) |
| `ESP_WIFI_PASS` | WiFi password |
| `ESP_MAXIMUM_RETRY` | Max reconnection attempts after a disconnect |
| `WIFI_CONNECTED_BIT` | Event group bit (`BIT0 = 0x01`) for WiFi-ready signal |

### 5. Global State Variables

```c
static bool               s_is_wifi_connected = false;
static EventGroupHandle_t s_event_group_handler;
static int                s_retry_count = 0;
```

File-scope variables shared between the WiFi event handler and `wifi_init_sta()`:

| Variable | Purpose |
|----------|---------|
| `s_is_wifi_connected` | Polled by `app_main`; set to `true` on `GOT_IP` |
| `s_event_group_handler` | FreeRTOS event group for blocking WiFi synchronisation |
| `s_retry_count` | Reconnect attempts counter; stops at `ESP_MAXIMUM_RETRY` |

### 6. Zenoh Mode Selection

```c
#define CLIENT_OR_PEER 0   // 0: Client mode; 1: Peer mode
#if CLIENT_OR_PEER == 0
#define MODE "client"
#define LOCATOR ""         // Empty → auto-discover via scout
#elif CLIENT_OR_PEER == 1
#define MODE "peer"
#define LOCATOR "udp/224.0.0.225:7447#iface=en0"
#endif
```

| Mode | `CLIENT_OR_PEER` | How it connects | Pros | Cons |
|------|-----------------|-----------------|------|------|
| **Client** | 0 | Connects to a Zenoh router (`zenohd`) | Reliable, scalable | Requires a router |
| **Peer** | 1 | UDP multicast, no router needed | Simple, self-contained | Less scalable, multicast limitations |

**Recommendation**: Start with **Client mode**. Run `zenohd` on a PC, then run the ESP32.

### 7. Key Expression and Reply Value

```c
#define KEYEXPR "demo/example/zenoh-pico-queryable"
#define VALUE "[ESPIDF]{ESP32} Queryable from Zenoh-Pico!"
```

| Macro | Purpose |
|-------|---------|
| `KEYEXPR` | The topic this queryable answers — queries targeting this key trigger the handler |
| `VALUE` | The static reply payload sent back to every query |

### 8. WiFi Event Handler

```c
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
```

Three events drive the WiFi lifecycle:

```
WIFI_EVENT_STA_START         →  esp_wifi_connect()
WIFI_EVENT_STA_DISCONNECTED  →  Retry if under limit
IP_EVENT_STA_GOT_IP          →  Set event group bit, reset retry count
```

The handler runs in ESP-IDF's event task context — keep it fast and non-blocking.

### 9. `wifi_init_sta()` — WiFi Initialisation (Blocking)

Blocks the calling task until the ESP32 has a valid IP address. The standard ESP-IDF sequence:

```
Event group    →  xEventGroupCreate()
Network stack  →  esp_netif_init() + esp_event_loop_create_default()
STA netif      →  esp_netif_create_default_wifi_sta()
WiFi driver    →  esp_wifi_init(&config)
Handlers       →  Register event_handler for WIFI_EVENT + IP_EVENT
Configure      →  Set SSID/password, mode = STA, start
BLOCK          →  xEventGroupWaitBits(... portMAX_DELAY)
Cleanup        →  Unregister handlers, delete event group
```

Key details:

```c
// Register BEFORE starting — otherwise you might miss STA_START
esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, ...);
esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, ...);

// C99 designated initialisers for the WiFi config struct
wifi_config_t wifi_config = {
    .sta = {
        .ssid = ESP_WIFI_SSID,
        .password = ESP_WIFI_PASS,
    }
};

// Blocking wait — 0% CPU usage while blocked
xEventGroupWaitBits(s_event_group_handler, WIFI_CONNECTED_BIT,
                    pdFALSE, pdFALSE, portMAX_DELAY);
```

### 10. `query_handler()` — Query Callback

```c
void query_handler(z_loaned_query_t *query, void *ctx)
```

This is the central function of the queryable. It runs each time another node sends a GET query.

#### Step-by-step

**Extract and print the key expression:**
```c
z_view_string_t keystr;
z_keyexpr_as_view_string(z_query_keyexpr(query), &keystr);
```
`z_view_string_t` is a **borrowed** string — no free needed. It borrows from the query's internal data.

**Extract and print query parameters:**
```c
z_view_string_t params;
z_query_parameters(query, &params);
```
Queries can include parameters like `?threshold=0.5&unit=celsius`. These appear after the key expression.

**Extract and print the query payload (if any):**
```c
z_owned_string_t payload_string;
z_bytes_to_string(z_query_payload(query), &payload_string);
if (z_string_len(z_loan(payload_string)) > 0) {
    printf("     with value '%.*s'\n", ...);
}
z_drop(z_move(payload_string));  // MUST free the owned string!
```
The query may include a payload (the "body" of the request). We convert it to an owned string, print it, then **drop** it to avoid a memory leak.

**Build and send the reply:**
```c
z_view_keyexpr_t ke;
z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);

z_owned_bytes_t reply_payload;
z_bytes_from_static_str(&reply_payload, VALUE);

z_query_reply(query, z_loan(ke), z_move(reply_payload), NULL);
```

The reply is sent via `z_query_reply()`:
1. Reuses the same key expression the query targeted
2. Packs the static `VALUE` string into a Zenoh `Bytes` payload
3. Sends the reply (non-blocking — queued for transport delivery)

**Important: `z_query_reply()` does NOT end the query.** A queryable can send multiple replies to a single query (for aggregation scenarios). If you want to signal "this is the last reply," pass a reply context with `z_query_reply_final()` instead.

### 11. `app_main()` — Entry Point

```
┌─────────────────────────────────────────────┐
│  Step 1: NVS initialisation                 │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 2: WiFi connection (blocking)         │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 3: Build Zenoh config                 │
│  (z_config_default + zp_config_insert)      │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 4: Open Zenoh session                 │
│  (z_open) — connects to router or peer      │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 5: Declare Queryable                  │
│  (z_declare_queryable)                      │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 6: Idle loop                          │
│  (while(1) { sleep(1); })                   │
│  → query_handler runs on each GET           │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 7: Cleanup (unreachable)              │
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

Standard ESP-IDF NVS init with partition recovery. The erase-and-retry pattern handles:
- `ESP_ERR_NVS_NO_FREE_PAGES` — partition is full
- `ESP_ERR_NVS_NEW_VERSION_FOUND` — NVS format changed after IDF upgrade

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

`wifi_init_sta()` blocks internally via the event group; the polling loop is a secondary safeguard.

#### Step 3 — Build Zenoh Configuration

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

- `z_config_default()` — starts with Zenoh's default parameters
- `zp_config_insert()` — overrides individual config keys
- `z_loan_mut()` — borrows the owned config mutably for the call
- Empty `LOCATOR` (client mode) = auto-discover router via scout

#### Step 4 — Open Zenoh Session

```c
printf("Opening Zenoh Session...");
z_owned_session_t s;
if (z_open(&s, z_move(config), NULL) < 0) {
    printf("Unable to open session!\n");
    exit(-1);
}
printf("OK\n");
```

`z_open()` establishes a transport connection and negotiates capabilities. After success, `config` is **moved** (invalidated) — don't use it again.

#### Step 5 — Declare Queryable

```c
printf("Declaring Queryable on %s...", KEYEXPR);
z_owned_closure_query_t callback;
z_closure(&callback, query_handler, NULL, NULL);
z_owned_queryable_t qable;
z_view_keyexpr_t ke;
z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);
if (z_declare_queryable(z_loan(s), &qable, z_loan(ke), z_move(callback), NULL) < 0) {
    printf("Unable to declare queryable.\n");
    exit(-1);
}
```

**Closure construction:**
```c
z_closure(&callback, query_handler, NULL, NULL);
```
- 1st parameter: the closure output
- 2nd parameter: the handler function (called per query)
- 3rd parameter: optional context pointer (`NULL`)
- 4th parameter: optional drop function (`NULL`)

**Key expression:**
```c
z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);
```
`_unchecked` skips validation — we trust the compile-time macro.

**Declaration:**
```c
z_declare_queryable(z_loan(s), &qable, z_loan(ke), z_move(callback), NULL);
```
- `z_loan(s)` — borrowed session reference
- `&qable` — output: receives the queryable handle
- `z_loan(ke)` — borrowed key expression
- `z_move(callback)` — closure ownership transferred to the queryable
- `NULL` — no additional queryable options

After this call, the queryable is **live**. Any GET query on `"demo/example/zenoh-pico-queryable"` triggers `query_handler`.

#### Step 6 — Idle Loop

```c
while (1) {
    sleep(1);
}
```

The queryable runs asynchronously in the background. We just keep the task alive.

#### Step 7 — Cleanup (Unreachable)

```c
printf("Closing Zenoh Session...");
z_drop(z_move(qable));
z_drop(z_move(s));
```

Shown for completeness — correct teardown order:
1. `z_drop(z_move(qable))` — undeclares the queryable
2. `z_drop(z_move(s))` — closes the session

---

## Build & Flash

### 1. Configure WiFi Credentials

```c
#define ESP_WIFI_SSID "Your_SSID"
#define ESP_WIFI_PASS "Your_Password"
```

### 2. Ensure Queryable Feature is Enabled

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable queryable feature
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

Expected output:

```
Connecting to WiFi...OK!
Opening Zenoh Session...OK
Declaring Queryable on demo/example/zenoh-pico-queryable...OK
Zenoh setup finished!
```

Press `Ctrl+]` to exit the monitor.

---

## Testing with GET Queries

### Using the Zenoh CLI

On a PC on the same network, send a query:

```bash
zenoh get -k "demo/example/zenoh-pico-queryable"
```

Expected ESP32 output:

```
 >> [Queryable handler] Received Query 'demo/example/zenoh-pico-queryable'
```

Expected PC output:

```
>> Received ('demo/example/zenoh-pico-queryable': '[ESPIDF]{ESP32} Queryable from Zenoh-Pico!')
```

### Query with a Payload

```bash
zenoh get -k "demo/example/zenoh-pico-queryable" -v "ping"
```

ESP32 output:

```
 >> [Queryable handler] Received Query 'demo/example/zenoh-pico-queryable'
     with value 'ping'
```

### Query with Parameters

```bash
zenoh get -k "demo/example/zenoh-pico-queryable?format=json&limit=10"
```

---

## Customisation Guide

### Change the Reply Value

```c
#define VALUE "{\"sensor\": \"temperature\", \"value\": 25.3}"
```

### Add Query Routing Logic

```c
void query_handler(z_loaned_query_t *query, void *ctx) {
    z_view_string_t params;
    z_query_parameters(query, &params);

    if (/* params contain "format=json" */) {
        // Reply with JSON
    } else {
        // Reply with plain text
    }
}
```

### Register Multiple Queryables

```c
// Answer queries on two different topics
z_declare_queryable(..., "sensor/temperature", ...);
z_declare_queryable(..., "sensor/humidity", ...);
```

### Send Multiple Replies to One Query

A single queryable can send multiple replies:

```c
z_query_reply(query, z_loan(ke1), z_move(payload1), NULL);
z_query_reply(query, z_loan(ke2), z_move(payload2), NULL);

// Mark as final (if supported by the API version)
z_query_reply_final(query, z_loan(ke), z_move(payload), NULL);
```

---

## Troubleshooting

### ❌ `Unable to open session!`

| Possible Cause | Solution |
|----------------|----------|
| `zenohd` not running (Client mode) | Start `zenohd` on a PC on the same network |
| WiFi connection failed | Check SSID and password |
| Firewall blocking Zenoh ports | Open TCP/UDP 7447 |
| Different subnet | Must be on the same subnet for scouting discovery |

### ❌ `Unable to declare queryable.`

Usually occurs right after a session open. Check:
- Network connectivity (is the ESP32 still connected to WiFi?)
- Zenoh router is still running

### ❌ No response when sending a GET

| Possible Cause | Solution |
|----------------|----------|
| Wrong key expression | Both GET and queryable must use the same `KEYEXPR` |
| Different session modes | Mixing client/peer modes without a router to bridge them |
| Router not forwarding | Check `zenohd` logs — ensure it sees both endpoints |

### ❌ `Z_FEATURE_QUERYABLE` undefined in `sdkconfig`

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable queryable feature
```
Enable it and rebuild.

---

## Reference Resources

| Resource | Link |
|----------|------|
| Zenoh Queryable Concept | https://zenoh.io/docs/manual/abstractions/#queryable |
| zenoh-pico API | https://zenoh-pico.readthedocs.io/en/1.9.0/ |
| ESP-IDF WiFi Driver | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp_wifi.html |
| FreeRTOS Event Groups | https://www.freertos.org/event-groups.html |
| Eclipse Public License 2.0 | https://www.eclipse.org/legal/epl-2.0/ |

---

## License

The source code corresponding to this document is dual-licensed under Eclipse Public License 2.0 or Apache License 2.0 — see the file header for details.
