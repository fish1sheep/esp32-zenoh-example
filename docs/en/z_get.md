# z_get.md — ESP32 (S3 / C5) Zenoh GET Client (Request-Response) Tutorial

[← Back to docs](../README.md)

---

## Overview

`main/z_get.c` is a Zenoh **GET client** example for the **ESP32 (S3 / C5)**, built on
**ESP-IDF v6.0**. It demonstrates the client side of Zenoh's request-response
pattern — the ESP32 periodically sends GET queries on a key expression and
prints any replies received from Queryables on the network.

### What is a GET client?

In Zenoh, a **GET client** is analogous to an HTTP client making a request:

```
[GET Client (ESP32)]  ─── GET "demo/example/**" ───→  [Queryable (PC)]
                            └─ (no payload)              │
                                                         │
                ←── Reply: "[Python] Hello from PC!" ────┘
```

Unlike a subscriber (which receives pushed data continuously), a GET client
sends a specific **query** and collects **replies**. This is a pull model — the
client decides when to ask, and the queryable responds on demand.

### Key Features

| Feature | Description |
|---------|-------------|
| WiFi STA Connection | Connects to a specified SSID with automatic retry (up to 5 times) |
| Zenoh Session | Opens a Zenoh session in Client or Peer mode |
| Periodic Querying | Sends `z_get()` every 5 seconds on `"demo/example/**"` |
| Reply Handling | Prints each received reply via `reply_handler` callback |
| Completion Notification | `reply_dropper` signals when all replies for a query are received |
| Error Detection | `z_reply_is_ok()` distinguishes successful replies from errors |
| Memory Safety | Properly drops owned strings to prevent leaks |

### Data Flow

```
[ESP32 (S3 / C5) GET Client]                    [Queryable (PC / another ESP32)]
      │                                            │
      │  GET "demo/example/**"                      │
      │  ─────────────────────────────────────────→ │
      │                                             │ query_handler fires:
      │                                             │  1. Builds reply
      │                                             │  2. Calls z_query_reply()
      │  ←───────────────────────────────────────── │
      │  Reply: "[Python] Hello from PC!"            │
      │                                             │
      │  GET "demo/example/**"                      │
      │  ─────────────────────────────────────────→ │
      │  ←───────────────────────────────────────── │
      │  Reply: "[Python] Hello from PC!"            │
      │                    ...every 5 seconds        │
      ▼                                             ▼
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
| zenoh-pico | v1.9.0 | Zenoh protocol stack (must have **query** feature enabled) |
| xtensa-esp32s3-elf-gcc | — | Cross-compiler toolchain |
| Python + zenoh library | — | For running a Queryable on PC to test against |

### Network

- A 2.4 GHz WiFi access point (SSID + password)
- A Zenoh router (`zenohd`) **or** peer on the same network (for routing queries)
- A Queryable registered on a key matching `"demo/example/**"` to reply

---

## Code Walkthrough

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
#include <stdlib.h>  /* exit */
#include <string.h>  /* strcmp */

#include <nvs_flash.h>   /* NVS flash storage */
#include <unistd.h>      /* sleep() */
#include <zenoh-pico.h>  /* Zenoh client library */

#include <freertos/FreeRTOS.h>     /* FreeRTOS kernel */
#include <freertos/event_groups.h> /* Event groups */
#include <freertos/task.h>         /* Task API */
```

Four groups in ESP-IDF's include category order: **ESP-IDF**, **C Standard**,
**Third-party**, **FreeRTOS**.

### 3. Compile-Time Guard: `#if Z_FEATURE_QUERY == 1`

```c
#if Z_FEATURE_QUERY == 1
// ... main body ...
#else
void app_main() { printf("ERROR: Zenoh pico was compiled without "
                         "Z_FEATURE_QUERY but this example requires it.\n"); }
#endif
```

The **query** feature is separate from the **queryable** feature in zenoh-pico.
`Z_FEATURE_QUERY` enables the client side (`z_get()`). If you get a build
error, check:

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable query feature
```

### 4. WiFi Configuration

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
| `WIFI_CONNECTED_BIT` | Event group bit (`BIT0 = 0x01`) — signals DHCP success |

### 5. Global State Variables

```c
static bool               s_is_wifi_connected = false;
static EventGroupHandle_t s_event_group_handler;
static int                s_retry_count = 0;
```

Same shared-state pattern used across all examples in this project:
- `s_is_wifi_connected` — polled by `app_main`; set to `true` on `GOT_IP`
- `s_event_group_handler` — FreeRTOS event group for WiFi synchronisation
- `s_retry_count` — tracks reconnection attempts; stops at `ESP_MAXIMUM_RETRY`

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

| Mode | `CLIENT_OR_PEER` | How it connects |
|------|-----------------|-----------------|
| **Client** | 0 | Connects to a Zenoh router (`zenohd`) |
| **Peer** | 1 | UDP multicast, no router needed |

**Recommendation**: Start with **Client mode**. Run `zenohd` on a PC, then run
both the ESP32 GET client and a PC-side Queryable (e.g. `scripts/z_queryable.py`).

### 7. Key Expression and Query Payload

```c
#define KEYEXPR "demo/example/**"
#define VALUE ""
```

| Macro | Purpose |
|-------|---------|
| `KEYEXPR` | Key selector for `z_get()` — uses `**` wildcard to match **any** queryable under `demo/example/` |
| `VALUE` | Optional query payload (currently empty). Set to a string to send data with each GET request |

**Why `**` wildcard?** Using `demo/example/**` means the query matches every
key under that tree. A Queryable registered on `demo/example/zenoh-pico-pub`,
`demo/example/reply`, or any other sub-key will all respond. This makes testing
flexible — you don't need to match an exact key.

### 8. WiFi Event Handler & `wifi_init_sta()`

Identical to the other examples in this project. See
`docs/en/z_sub.md` for the full WiFi walkthrough. Briefly:

```
WIFI_EVENT_STA_START         →  esp_wifi_connect()
WIFI_EVENT_STA_DISCONNECTED  →  Retry if under limit
IP_EVENT_STA_GOT_IP          →  Signal the event group, proceed
```

### 9. `reply_dropper()` — Completion Callback

```c
void reply_dropper(void *ctx) {
    printf(" >> Received query final notification\n");
}
```

When `z_get()` sends a query, it expects one or more replies. The **drop
function** is called when **all replies have been received** (or the query
times out). This is how you know a query cycle is complete — useful for
counting how many Queryables responded.

### 10. `reply_handler()` — Per-Reply Callback

```c
void reply_handler(z_loaned_reply_t *oreply, void *ctx)
```

This function fires once per reply. It distinguishes success from error:

```c
if (z_reply_is_ok(oreply)) {
    // Successful reply — extract key expression and payload
    const z_loaned_sample_t *sample = z_reply_ok(oreply);
    z_view_string_t keystr;
    z_keyexpr_as_view_string(z_sample_keyexpr(sample), &keystr);
    z_owned_string_t replystr;
    z_bytes_to_string(z_sample_payload(sample), &replystr);

    printf(" >> Received ('%.*s': '%.*s')\n",
           (int)z_string_len(z_loan(keystr)), z_string_data(z_loan(keystr)),
           (int)z_string_len(z_loan(replystr)), z_string_data(z_loan(replystr)));
    z_drop(z_move(replystr));
} else {
    printf(" >> Received an error\n");
}
```

Key zenoh-pico API calls:

| Call | Purpose |
|------|---------|
| `z_reply_is_ok()` | Check if the reply is a success or an error |
| `z_reply_ok()` | Extract the sample from a successful reply |
| `z_sample_keyexpr()` | Get the key expression the Queryable replied on |
| `z_sample_payload()` | Get the payload bytes from the sample |
| `z_bytes_to_string()` | Convert Zenoh `Bytes` to an owned C string |
| `z_drop(z_move(replystr))` | **Must drop** the owned string to avoid leaks |
| `z_keyexpr_as_view_string()` | Borrow a view of the key expression (no free needed) |

**Important:** If no Queryable is registered on a matching key, the reply will
be an **error** (`z_reply_is_ok()` returns false). This is how you distinguish
"nobody answered" from "someone answered."

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
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 4: Open Zenoh session                 │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 5: Infinite query loop                │
│  ┌─ sleep(5)                                │
│  ├─ printf("Sending Query '%s'...")         │
│  ├─ z_get_options_default(&opts)            │
│  ├─ z_closure(&callback, reply_handler,     │
│  │              reply_dropper, NULL)         │
│  ├─ z_get(...)  ─── send the GET query ────→│
│  └─ reply_handler fires per reply ────────→ │
│     reply_dropper fires when done            │
│                    ...every 5 seconds         │
└─────────────────────────────────────────────┘
```

#### Step 1–4 — NVS, WiFi, Config, Session

These steps are identical to the other examples (`z_sub.c`, `z_pub.c`, etc.).
Refer to `docs/en/z_sub.md` for the detailed walkthrough.

#### Step 5 — The Query Loop

```c
while (1) {
    sleep(5);
    printf("Sending Query '%s'...\n", KEYEXPR);

    // ── Build query options ────────────────────────────────────────
    z_get_options_t opts;
    z_get_options_default(&opts);

    // ── Attach a payload (if VALUE is non-empty) ────────────────────
    z_owned_bytes_t payload;
    if (strcmp(VALUE, "") != 0) {
        z_bytes_from_static_str(&payload, VALUE);
        opts.payload = z_move(payload);
    }

    // ── Build the reply closure ────────────────────────────────────
    z_owned_closure_reply_t callback;
    z_closure(&callback, reply_handler, reply_dropper, NULL);

    // ── Send the query ─────────────────────────────────────────────
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);
    if (z_get(z_loan(s), z_loan(ke), "", z_move(callback), &opts) < 0) {
        printf("Unable to send query.\n");
        exit(-1);
    }
}
```

**Step by step:**

| Sub-step | Code | What happens |
|----------|------|-------------|
| Sleep | `sleep(5)` | Waits 5 seconds between each query |
| Options | `z_get_options_default(&opts)` | Initialises query options with defaults |
| Payload | `z_bytes_from_static_str(...)` | Converts `VALUE` into Zenoh `Bytes` (skipped if empty) |
| Closure | `z_closure(&callback, reply_handler, reply_dropper, NULL)` | Pairs the per-reply handler with the completion handler |
| Keyexpr | `z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR)` | Creates a key expression from the compile-time string |
| Query | `z_get(z_loan(s), z_loan(ke), "", z_move(callback), &opts)` | Sends the GET and returns immediately (async) |

**Key observation:** With `VALUE ""`, **no payload is sent**. The query is
just a request — "is anyone there?" If you set `VALUE` to something like
`"ping"`, every GET client on the network can see the payload and respond
accordingly.

**The second argument to `z_get()` (`""`):** This is the **query parameters**
string — URL-style parameters like `"?threshold=0.5"`. Empty means no
parameters.

---

## Build & Flash

### 1. WiFi Credentials

Set your WiFi credentials in the source code:

```c
#define ESP_WIFI_SSID "Your_SSID"
#define ESP_WIFI_PASS "Your_Password"
```

### 2. Ensure Query Feature is Enabled

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable query feature
```

**Note:** This is `Z_FEATURE_QUERY` (not `Z_FEATURE_QUERYABLE`).

### 3. Build

```bash
idf.py build
```

### 4. Flash & Monitor

```bash
idf.py flash monitor
```

Expected output (when a Queryable is present):

```
Connecting to WiFi...OK!
Opening Zenoh Session...W (4142) wifi:<ba-add>...
OK
Sending Query 'demo/example/**'...
 >> Received ('demo/example/reply': '[Python] Hello from PC! ...')
 >> Received query final notification
Sending Query 'demo/example/**'...
 >> Received ('demo/example/reply': '[Python] Hello from PC! ...')
 >> Received query final notification
```

Press `Ctrl+]` to exit the monitor.

---

## Testing

### 1. Python Queryable (Recommended)

Run the paired Python queryable on your PC, then power on the ESP32:

```bash
# Terminal 1 — Python Queryable (start first)
python3 scripts/z_queryable.py --connect tcp/<ROUTER_IP>:7447

# Terminal 2 — ESP32 monitor
idf.py monitor
```

ESP32 output (every 5 seconds):

```
Sending Query 'demo/example/**'...
 >> Received ('demo/example/reply': '[Python] Hello from PC! Received your query on ...')
 >> Received query final notification
```

Python side output:

```
[17:09:43] ⇐ Query on 'demo/example/**' (no payload)
[17:09:43] ⇒ Replied: '[Python] Hello from PC! ...'
```

If you see `>> Received an error` instead, it means no Queryable is
registered — check that `z_queryable.py` is running and the network is
reachable.

### 2. With Another ESP32 Queryable

Flash `main/z_queryable.c` to a second ESP32. The GET client's wildcard
`"demo/example/**"` will match the queryable's key
`"demo/example/zenoh-pico-queryable"` and show:

```
Sending Query 'demo/example/**'...
 >> Received ('demo/example/zenoh-pico-queryable': '[ESPIDF]{ESP32} Queryable from Zenoh-Pico!')
 >> Received query final notification
```

### 3. Reverse Direction — Python GET Client Testing ESP32 Queryable

The `scripts/z_get.py` script is a Python GET client you can use to test
the ESP32's Queryable (`main/z_queryable.c`):

```bash
python3 scripts/z_get.py --connect tcp/<ROUTER_IP>:7447 "ping"
```

ESP32 serial output:

```
 >> [Queryable handler] Received Query 'demo/example/zenoh-pico-queryable'
     with value 'ping'
```

---

## Customisation Guide

### Change the Interval

```c
sleep(1);   // Query every 1 second
// or
vTaskDelay(pdMS_TO_TICKS(500));  // Use FreeRTOS delay for sub-second
```

### Send a Payload with Every Query

```c
#define VALUE "ping"
```

Now every GET includes a `"ping"` payload the Queryable can inspect.

### Request a Specific Key (No Wildcard)

```c
#define KEYEXPR "demo/example/my-device"
```

Only Queryables registered on this exact key will respond.

### Count Replies

Use the closure's context pointer to track how many replies you received:

```c
typedef struct { int count; } reply_ctx_t;

void reply_handler(z_loaned_reply_t *oreply, void *ctx) {
    reply_ctx_t *c = (reply_ctx_t *)ctx;
    c->count++;
    // ...
}

void reply_dropper(void *ctx) {
    reply_ctx_t *c = (reply_ctx_t *)ctx;
    printf("Query complete — %d replies received.\n", c->count);
    free(c);
}

reply_ctx_t *ctx = malloc(sizeof(reply_ctx_t));
ctx->count = 0;
z_closure(&callback, reply_handler, reply_dropper, ctx);
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

### ❌ `Unable to send query.`

Usually means the session was lost. Check:
- WiFi connection (ESP32 logs)
- `zenohd` is still running and reachable

### ❌ Only "Received an error" — no valid replies

No Queryable is registered on a key matching `"demo/example/**"`.
Start `scripts/z_queryable.py` on a PC on the same network.

### ❌ `Z_FEATURE_QUERY` not found in `sdkconfig`

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable query feature
```

**Don't confuse with `Z_FEATURE_QUERYABLE`** — they are separate toggles:
- `Z_FEATURE_QUERY` enables `z_get()` (this is the GET **client** side)
- `Z_FEATURE_QUERYABLE` enables `z_declare_queryable()` (this is the Queryable **server** side)

---

## Reference Resources

| Resource | Link |
|----------|------|
| Zenoh Query / Get Concept | https://zenoh.io/docs/manual/abstractions/#querying |
| zenoh-pico API Docs | https://zenoh-pico.readthedocs.io/en/1.9.0/ |
| ESP-IDF WiFi Driver | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp_wifi.html |
| FreeRTOS Event Groups | https://www.freertos.org/event-groups.html |
| Eclipse Public License 2.0 | https://www.eclipse.org/legal/epl-2.0/ |

---

## License

The source code corresponding to this document is dual-licensed under
Eclipse Public License 2.0 or Apache License 2.0 — see the file header for
details.
