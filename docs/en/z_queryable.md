# z_queryable.md — ESP32-S3 Zenoh Queryable (Request-Response) Tutorial

[← Back to docs](../README.md)

---

## Overview

`main/z_queryable.c` is a Zenoh **queryable** example for the **ESP32-S3**, built on **ESP-IDF v6.0**. Like `z_get.c`, it demonstrates the server side of Zenoh's request-response pattern — the ESP32 declares a queryable on a key expression and replies when other nodes send GET queries.

### What is a Queryable?

A **Queryable** in Zenoh is the equivalent of a REST API endpoint:

```
Client:  GET "demo/example/zenoh-pico-queryable?filter=all"
                │
                ▼
         Zenoh Router (zenohd)
                │
                ▼
Queryable:  query_handler() fires
                │
                ├─ Reads query key expression, parameters, payload
                ├─ Builds reply "[ESPIDF]{ESP32} Queryable from Zenoh-Pico!"
                └─ Sends reply via z_query_reply()
                │
                ▼
         Client receives reply
```

Unlike a subscriber (receives pushed data), a queryable **waits to be asked**.

### Key Features

| Feature | Description |
|---------|-------------|
| WiFi STA Connection | Connects to a specified SSID with automatic retry (up to 5 times) |
| Zenoh Session | Opens a Zenoh session in Client or Peer mode |
| Queryable Declaration | Registers a handler for `"demo/example/zenoh-pico-queryable"` |
| Full Query Logging | Prints key expression, parameters, and optional payload |
| Reply Sending | Responds with a static value via `z_query_reply()` |
| Memory Safety | Properly drops owned strings to prevent leaks |

---

## Prerequisites

### Hardware

- ESP32-S3 development board (e.g., ESP32-S3-DevKitC-1)
- USB-C cable (power and serial)

### Software

| Tool | Version | Purpose |
|------|---------|---------|
| ESP-IDF | v6.0.1 | Embedded development framework |
| zenoh-pico | v1.9.0 | Zenoh protocol stack (queryable enabled) |
| xtensa-esp32s3-elf-gcc | — | Cross-compiler toolchain |
| zenoh (CLI) | — | For sending test GET queries |

### Network

- A 2.4 GHz WiFi access point
- A Zenoh router (`zenohd`) or peer on the same network

---

## Code Walkthrough

### 1. License Header & Summary

```c
// Copyright (c) 2022 ZettaScale Technology
// SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
```

### 2. Include Headers

```c
#include <esp_event.h>    /* ESP-IDF event loop */
#include <esp_log.h>      /* ESP-IDF logging */
#include <esp_system.h>   /* ESP-IDF system API */
#include <esp_wifi.h>     /* WiFi driver */

#include <stdio.h>   /* printf */
#include <stdlib.h>  /* exit */
#include <string.h>  /* strcmp */

#include <nvs_flash.h>   /* NVS flash storage */
#include <unistd.h>      /* sleep() */
#include <zenoh-pico.h>  /* Zenoh client library */

#include <freertos/FreeRTOS.h>     /* FreeRTOS kernel */
#include <freertos/event_groups.h> /* Event groups */
#include <freertos/task.h>         /* Task API */
```

Grouped by category: **ESP-IDF**, **C Standard**, **Third-party**, **FreeRTOS**.

### 3. Compile-Time Guard

```c
#if Z_FEATURE_QUERYABLE == 1
// ... main body ...
#else
void app_main() { printf("ERROR: ...\n"); }
#endif
```

### 4. WiFi Configuration

```c
#define ESP_WIFI_SSID "SSID"
#define ESP_WIFI_PASS "PASS"
#define ESP_MAXIMUM_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
```

### 5. Global State

```c
static bool               s_is_wifi_connected = false;
static EventGroupHandle_t s_event_group_handler;
static int                s_retry_count = 0;
```

### 6. Session Mode Selection

```c
#define CLIENT_OR_PEER 0
#if CLIENT_OR_PEER == 0
#define MODE "client"
#define LOCATOR ""            // Empty → scout for router
#elif CLIENT_OR_PEER == 1
#define MODE "peer"
#define LOCATOR "udp/224.0.0.225:7447#iface=en0"
#endif
```

### 7. Key Expression and Reply Payload

```c
#define KEYEXPR "demo/example/zenoh-pico-queryable"
#define VALUE "[ESPIDF]{ESP32} Queryable from Zenoh-Pico!"
```

### 8. WiFi Event Handler

```c
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
```

The standard three-event pattern:

| Event | What Happens |
|-------|-------------|
| `WIFI_EVENT_STA_START` | Calls `esp_wifi_connect()` |
| `WIFI_EVENT_STA_DISCONNECTED` | Retries up to `ESP_MAXIMUM_RETRY` times |
| `IP_EVENT_STA_GOT_IP` | Signals the event group, resets retry counter |

### 9. `wifi_init_sta()` — WiFi Initialisation

Follows the same ESP-IDF sequence as all other examples:

```
Event group → Netif init → Event loop → STA netif → WiFi driver init
→ Register handlers → Set credentials → Start → Block (event group)
→ Unregister handlers → Delete event group
```

This function **blocks until DHCP succeeds** (an IP address is assigned).

### 10. `query_handler()` — Query Callback

This is the function that runs each time a remote node sends a GET query targeting our key expression.

```c
void query_handler(z_loaned_query_t *query, void *ctx)
{
    (void)(ctx);  /* Unused context parameter */
```

#### Step 1: Extract and print the key expression

```c
z_view_string_t keystr;
z_keyexpr_as_view_string(z_query_keyexpr(query), &keystr);
```

`z_query_keyexpr(query)` returns a loaned reference to the key expression that the GET query targeted. This might be the exact key or a wildcard match.

`z_view_string_t` is a **non-owning** type — no memory to free.

#### Step 2: Extract and print query parameters

```c
z_view_string_t params;
z_query_parameters(query, &params);
```

Queries can carry URL-style parameters like `?format=json&limit=100`. These are extracted separately from the key expression.

#### Step 3: Extract and print the query payload

```c
z_owned_string_t payload_string;
z_bytes_to_string(z_query_payload(query), &payload_string);
if (z_string_len(z_loan(payload_string)) > 0) {
    printf("     with value '%.*s'\n", ...);
}
z_drop(z_move(payload_string));
```

Some GET queries carry a payload (similar to an HTTP GET body). We:
1. Convert the payload bytes to an owned string
2. Print it if non-empty
3. **Drop it** — this is crucial to avoid a memory leak

#### Step 4: Build the reply

```c
z_view_keyexpr_t ke;
z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);

z_owned_bytes_t reply_payload;
z_bytes_from_static_str(&reply_payload, VALUE);

z_query_reply(query, z_loan(ke), z_move(reply_payload), NULL);
```

**`z_query_reply()`** sends the response back to the querying node:

| Parameter | What it is |
|-----------|------------|
| `query` | The original query reference (loaned) |
| `z_loan(ke)` | The key expression to reply on |
| `z_move(reply_payload)` | The payload (ownership transferred) |
| `NULL` | No reply attachments |

**Important:** `z_query_reply()` is **non-blocking**. It queues the reply for the transport layer. Also, a queryable can send **multiple replies** to one query (for aggregation). To signal "last reply," use `z_query_reply_final()` if available in your API version.

### 11. `app_main()` — Entry Point

```
┌─────────────────────────────────────────────┐
│  Step 1: NVS init                           │
│  → nvs_flash_init() + erase-retry pattern   │
├─────────────────────────────────────────────┤
│  Step 2: WiFi connect                       │
│  → wifi_init_sta() + polling safeguard      │
├─────────────────────────────────────────────┤
│  Step 3: Build Zenoh config                 │
│  → z_config_default() + zp_config_insert()  │
├─────────────────────────────────────────────┤
│  Step 4: Open Zenoh session                 │
│  → z_open() — connects to router/peer       │
├─────────────────────────────────────────────┤
│  Step 5: Declare Queryable                  │
│  → z_declare_queryable() — live handler     │
├─────────────────────────────────────────────┤
│  Step 6: Idle loop                          │
│  → while(1) { sleep(1); }                   │
├─────────────────────────────────────────────┤
│  Step 7: Cleanup (unreachable)              │
│  → z_drop(qable) → z_drop(session)          │
└─────────────────────────────────────────────┘
```

#### Steps 1–4

Standard ESP-IDF + Zenoh setup. See `z_get.md` or `z_pub.md` for a detailed walkthrough.

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

**`z_closure(&callback, query_handler, NULL, NULL)`** builds a Zenoh closure with:
1. `query_handler` — the function called on each incoming query
2. `NULL` — no context pointer (passed as `ctx` to the handler)
3. `NULL` — no drop function

**`z_declare_queryable(...)`** registers the queryable with the Zenoh session:
- `z_loan(s)` — borrowed session reference
- `&qable` — receives the queryable handle
- `z_loan(ke)` — borrowed key expression
- `z_move(callback)` — closure ownership transferred to the session
- `NULL` — no additional options

After this call succeeds, the queryable is **active**. Any matching GET query will invoke `query_handler`.

#### Step 6 — Idle Loop

```c
while (1) {
    sleep(1);
}
```

The queryable runs in the background. We just keep the task alive.

#### Step 7 — Cleanup (Unreachable)

```c
z_drop(z_move(qable));   // Undeclare the queryable
z_drop(z_move(s));       // Close the session
```

---

## Build & Flash

### 1. Configure WiFi

```c
#define ESP_WIFI_SSID "Your_SSID"
#define ESP_WIFI_PASS "Your_Password"
```

### 2. Verify Queryable Feature

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable queryable feature
```

### 3. Build, Flash, Monitor

```bash
idf.py build flash monitor
```

Expected output:

```
Connecting to WiFi...OK!
Opening Zenoh Session...OK
Declaring Queryable on demo/example/zenoh-pico-queryable...OK
Zenoh setup finished!
```

---

## Testing with GET Queries

### Using the Python Querier (Recommended)

Install zenoh-python and run `z_get.py` from this project:

```bash
pip install zenoh

# Query the default key expression
uv run python3 scripts/z_get.py

# Query with a payload
uv run python3 scripts/z_get.py "Hello ESP32!"

# Peer mode with custom timeout
uv run python3 scripts/z_get.py --mode peer --timeout 5.0
```

ESP32 output:
```
 >> [Queryable handler] Received Query 'demo/example/zenoh-pico-queryable'
```

PC output:
```
>> Received ('demo/example/zenoh-pico-queryable': '[ESPIDF]{ESP32} Queryable from Zenoh-Pico!')
```

---

## Customisation Guide

### Dynamic Reply Based on Query Payload

```c
void query_handler(z_loaned_query_t *query, void *ctx) {
    z_owned_string_t payload;
    z_bytes_to_string(z_query_payload(query), &payload);

    z_owned_bytes_t reply;
    if (z_string_len(z_loan(payload)) > 0 &&
        strcmp(z_string_data(z_loan(payload)), "time") == 0) {
        char timebuf[64];
        sprintf(timebuf, "Current uptime: %d secs", uptime_seconds);
        z_bytes_from_static_str(&reply, timebuf);
    } else {
        z_bytes_from_static_str(&reply, VALUE);
    }

    z_query_reply(query, z_loan(ke), z_move(reply), NULL);
    z_drop(z_move(payload));
}
```

### Multiple Queryables

Declare handlers for different topics:

```c
z_declare_queryable(..., "sensor/temperature", ...);
z_declare_queryable(..., "sensor/humidity", ...);
```

### Using a Context Pointer

Pass a struct to share state with the handler:

```c
typedef struct { int counter; } query_ctx_t;

query_ctx_t *ctx = malloc(sizeof(query_ctx_t));
ctx->counter = 0;
z_closure(&callback, query_handler, ctx, free_context);

void query_handler(z_loaned_query_t *query, void *ctx_ptr) {
    query_ctx_t *ctx = (query_ctx_t *)ctx_ptr;
    ctx->counter++;
    // ... reply with counter value
}
```

---

## Troubleshooting

### ❌ `Unable to open session!`

| Cause | Solution |
|-------|----------|
| No router running (client mode) | Start `zenohd` |
| WiFi not connected | Verify SSID/password |
| Different subnet | Must be on same subnet for scouting |
| Firewall | Open UDP 7447 (scout) and TCP/UDP 7447 (session) |

### ❌ `Unable to declare queryable.`

The session might have disconnected. Check network stability.

### ❌ GET returns no data

| Check | What to verify |
|-------|----------------|
| Key expression | Both sides use `"demo/example/zenoh-pico-queryable"` |
| Session mode | Client ↔ Client (via router) or Peer ↔ Peer (direct) |
| Router logs | `zenohd` shows both endpoints connected? |

### ❌ `Z_FEATURE_QUERYABLE` missing

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable queryable feature
```

---

## Reference Resources

| Resource | Link |
|----------|------|
| Zenoh Queryable Concept | https://zenoh.io/docs/manual/abstractions/#queryable |
| zenoh-pico API | https://zenoh-pico.readthedocs.io/en/1.9.0/ |
| Zenoh CLI get command | https://zenoh.io/docs/getting-started/quick-test/#rest-api |
| ESP-IDF Programming Guide | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/ |
| Eclipse Public License 2.0 | https://www.eclipse.org/legal/epl-2.0/ |

---

## License

The source code corresponding to this document is dual-licensed under Eclipse Public License 2.0 or Apache License 2.0 — see the file header for details.
