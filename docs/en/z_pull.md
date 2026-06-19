# z_pull.md — ESP32-S3 Zenoh Pull Subscriber (Ring Channel) Tutorial

[← Back to docs](../README.md)

---

## Overview

`main/z_pull.c` is a Zenoh **pull-based subscriber** example for the **ESP32-S3**, built on **ESP-IDF v6.0**. It demonstrates how to use a **ring channel** — a bounded FIFO buffer — to receive publications at the application's own pace rather than via an immediate callback.

### Pull vs. Callback Subscriber

Zenoh supports two subscriber models:

| Aspect | Callback Subscriber (`z_sub.c`) | Pull Subscriber (`z_pull.c`) |
|--------|-------------------------------|------------------------------|
| Receive method | Zenoh calls your handler immediately | You call `z_try_recv()` when ready |
| Concurrency | Handler runs in Zenoh's internal context | Pull in your main loop |
| Buffer | No buffering (must process in handler) | Ring channel stores up to N samples |
| Best for | Real-time response, low latency | Polling cycles, fixed-interval processing |
| Overflow | Messages are synchronous | Oldest sample is dropped when full |

### How the Ring Channel Works

```
[Publisher] → publication
       │
       ▼
[Zenoh Session] receives and deserialises
       │
       ▼
[Closure] (secretly writes into ring buffer)
       │
       ├─ Ring slot available → sample is stored
       └─ Ring full           → oldest sample is dropped
       │
       ▼
[Handler] — application calls z_try_recv() to drain
       │
       ├─ z_try_recv returns Z_OK → sample available
       └─ z_try_recv returns Z_CHANNEL_NODATA → buffer empty
```

The ring channel is created as a **pair**:
- **Closure** — passed to `z_declare_subscriber()`; Zenoh writes into it
- **Handler** — the application reads from it via `z_try_recv()`

### Key Features

| Feature | Description |
|---------|-------------|
| WiFi STA Connection | Connects to a specified SSID with automatic retry |
| Ring Channel Subscriber | Buffers up to 3 samples; oldest dropped on overflow |
| Periodic Polling | Drains the ring every 5 seconds |
| Non-Blocking Receive | `z_try_recv()` never blocks — returns immediately |
| Memory Safety | Properly drops each sample after processing |

### Data Flow

```
[Peer or Router]             [ESP32-S3 Pull Subscriber]
      │                               │
      │  pub "demo/example/foo"       │
      │ ─────────────────────────────→│
      │                               │  Ring: [sample1]
      │  pub "demo/example/bar"       │
      │ ─────────────────────────────→│
      │                               │  Ring: [sample1] [sample2]
      │                               │
      │                          (every 5 seconds)
      │                               │
      │                               │  z_try_recv() → sample1
      │                               │  z_try_recv() → sample2
      │                               │  z_try_recv() → Z_CHANNEL_NODATA
      │                               │  → sleep 5 seconds
      │                               ▼
      │                         Serial Console
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
| zenoh-pico | v1.9.0 | Zenoh protocol stack (with subscription support) |
| xtensa-esp32s3-elf-gcc | — | Cross-compiler toolchain |

### Network

- A 2.4 GHz WiFi access point (SSID + password)
- A Zenoh publisher (e.g., `z_pub.c` on another ESP32, `zenoh pub` CLI, or Python script) on the same network

---

## Code Walkthrough

### 1. License Header

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

### 3. Compile-Time Guard: `#if Z_FEATURE_SUBSCRIPTION == 1`

```c
#if Z_FEATURE_SUBSCRIPTION == 1
// ... main body ...
#else
void app_main() { printf("ERROR: ...\n"); }
#endif
```

The subscription feature must be enabled in zenoh-pico's build. If not, a clear error message is printed instead of a link failure.

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
#define LOCATOR ""
#elif CLIENT_OR_PEER == 1
#define MODE "peer"
#define LOCATOR "udp/224.0.0.225:7447#iface=en0"
#endif
```

| Mode | How it connects | Router needed? |
|------|----------------|----------------|
| Client | Connects to `zenohd` | Yes |
| Peer | UDP multicast directly | No |

### 7. Key Expression & Polling Parameters

```c
#define KEYEXPR "demo/example/**"

const size_t INTERVAL = 5000;   /* Polling interval in milliseconds */
const size_t SIZE     = 3;      /* Ring channel capacity */
```

| Constant | Meaning |
|----------|---------|
| `KEYEXPR` | Topic filter — `**` matches any sub-path |
| `INTERVAL` | How often we check for new samples (5 seconds) |
| `SIZE` | Ring buffer capacity — at most 3 unread samples |

**Why `SIZE = 3`?** In a real application, you'd set this based on:
- Your expected publication rate
- Your maximum acceptable latency
- Available RAM (each sample carries the full key expression + payload)

If the ring is full when a new publication arrives, **the oldest sample is silently dropped**. This avoids back-pressure on the publisher.

### 8. WiFi Event Handler & `wifi_init_sta()`

These follow the same standard ESP-IDF pattern used across all examples in this project:

```
WIFI_EVENT_STA_START        →  esp_wifi_connect()
WIFI_EVENT_STA_DISCONNECTED →  Retry up to 5 times
IP_EVENT_STA_GOT_IP         →  Set event group bit, wake up app_main
```

`wifi_init_sta()` blocks until DHCP succeeds. See `z_pub.md` or `z_sub.md` for a detailed walkthrough of this function.

### 9. `app_main()` — Entry Point

The differences from `z_sub.c` (the callback subscriber) are in **Steps 5 and 6**.

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
│  Step 5: Declare subscriber                 │
│  (z_ring_channel_sample_new + declare)      │  ← KEY DIFFERENCE
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 6: Pull loop                          │
│  (z_try_recv every 5 seconds)               │  ← KEY DIFFERENCE
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 7: Cleanup                            │
└─────────────────────────────────────────────┘
```

#### Steps 1–4

Identical to the other examples: NVS init → WiFi connect → build Zenoh config → open session.

#### Step 5 — Ring Channel Subscriber Declaration

This is where the pull subscriber differs from the callback subscriber.

```c
printf("Declaring Subscriber on '%s'...\n", KEYEXPR);

// 1. Create the ring channel pair
z_owned_closure_sample_t       closure;
z_owned_ring_handler_sample_t  handler;
z_ring_channel_sample_new(&closure, &handler, SIZE);

// 2. Declare the subscriber (pass the closure, NOT a callback)
z_owned_subscriber_t sub;
z_view_keyexpr_t     ke;
z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);
if (z_declare_subscriber(z_loan(s), &sub, z_loan(ke), z_move(closure), NULL) < 0) {
    printf("Unable to declare subscriber.\n");
    exit(-1);
}
```

**`z_ring_channel_sample_new(&closure, &handler, SIZE)`** creates a bounded FIFO:

| Output | Type | Purpose |
|--------|------|---------|
| `closure` | `z_owned_closure_sample_t` | Passed to `z_declare_subscriber`; Zenoh writes into it |
| `handler` | `z_owned_ring_handler_sample_t` | Application reads from this side |
| `SIZE` | `size_t` | Max buffered samples (3) |

**Under the hood:** The closure internally holds a reference to a shared ring buffer. When a publication arrives, Zenoh's transport calls the closure, which writes the sample into the ring. The handler side can then pull from the ring at any time.

**When the ring is full:** The oldest sample is overwritten. This is a **bounded, lock-free** design — the publisher is never blocked by a slow consumer.

#### Step 6 — Pull Loop (Main Difference)

```c
printf("Pulling data every %zu ms... Ring size: %zd\n", INTERVAL, SIZE);
z_owned_sample_t sample;
while (true) {
    z_result_t res;
    /*
     * Drain ALL pending samples from the ring.
     * z_try_recv() returns:
     *   Z_OK             → a sample was dequeued
     *   Z_CHANNEL_NODATA → ring is empty (no publications waiting)
     */
    for (res = z_try_recv(z_loan(handler), &sample); res == Z_OK;
         res = z_try_recv(z_loan(handler), &sample)) {
        /* Print key expression and payload */
        z_view_string_t keystr;
        z_keyexpr_as_view_string(z_sample_keyexpr(z_loan(sample)), &keystr);
        z_owned_string_t value;
        z_bytes_to_string(z_sample_payload(z_loan(sample)), &value);
        printf(">> [Subscriber] Pulled ('%.*s': '%.*s')\n",
               (int)z_string_len(z_loan(keystr)),
               z_string_data(z_loan(keystr)),
               (int)z_string_len(z_loan(value)),
               z_string_data(z_loan(value)));

        /* Free owned resources */
        z_drop(z_move(value));
        z_drop(z_move(sample));
    }

    if (res == Z_CHANNEL_NODATA) {
        /* Nothing to read — sleep and try again later */
        printf(">> [Subscriber] Nothing to pull... sleep for %zu ms\n", INTERVAL);
        z_sleep_ms(INTERVAL);
    } else {
        /* Unexpected error — exit pull loop */
        break;
    }
}
```

**Key API: `z_try_recv()`**

| Return Value | Meaning |
|-------------|---------|
| `Z_OK` | Sample dequeued successfully; process it |
| `Z_CHANNEL_NODATA` | Ring buffer is empty — no publications since last drain |
| Other | Error — should break out of the loop |

**The inner `for` loop** drains all samples at once:
```c
for (res = z_try_recv(...); res == Z_OK; res = z_try_recv(...))
```
This ensures the ring is completely emptied each polling cycle. If you only call `z_try_recv()` once per poll, samples can accumulate and overflow the buffer.

**Memory management for each sample:**
1. Convert the payload to an owned string: `z_bytes_to_string(...)`
2. Print it
3. Drop the owned string: `z_drop(z_move(value))`
4. Drop the sample itself: `z_drop(z_move(sample))`

Skipping either `z_drop` call causes a memory leak on each received publication.

#### Step 7 — Cleanup

```c
z_drop(z_move(sub));      // Undeclare subscriber
z_drop(z_move(handler));  // Free ring channel handler
z_drop(z_move(s));        // Close session
```

Note that we drop **both** the subscriber and the handler — they are separate owned objects created by `z_ring_channel_sample_new()`.

---

## Build & Flash

### 1. Configure WiFi Credentials

```c
#define ESP_WIFI_SSID "Your_SSID"
#define ESP_WIFI_PASS "Your_Password"
```

### 2. Ensure Subscription is Enabled

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable subscription feature
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

Expected output (waiting for data):

```
Connecting to WiFi...OK!
Opening Zenoh Session...OK
Declaring Subscriber on 'demo/example/**'...
Pulling data every 5000 ms... Ring size: 3
>> [Subscriber] Nothing to pull... sleep for 5000 ms
```

---

## Testing with a Publisher

### Using the Python Batch Publisher (Recommended)

Install zenoh-python and run `z_pull.py` from this project:

```bash
pip install zenoh

# Send a burst of 5 messages, 1 second apart
uv run python3 scripts/z_pull.py

# Custom burst: 10 messages, 0.5 seconds apart
uv run python3 scripts/z_pull.py -n 10 -d 0.5 "Batch test"

# Peer mode
uv run python3 scripts/z_pull.py --mode peer
```

Expected ESP32 output after the next poll cycle:

```
>> [Subscriber] Pulled ('demo/example/test': 'Hello ESP32 pull!')
>> [Subscriber] Nothing to pull... sleep for 5000 ms
```

### Using Another ESP32 (z_pub.c)

Flash `z_pub.c` to another ESP32-S3. It publishes every second. The pull subscriber collects up to 3 samples per 5-second window:

```
>> [Subscriber] Pulled ('demo/example/zenoh-pico-pub': '[   0] [ESPIDF]{ESP32} Publication...')
>> [Subscriber] Pulled ('demo/example/zenoh-pico-pub': '[   1] [ESPIDF]{ESP32} Publication...')
>> [Subscriber] Pulled ('demo/example/zenoh-pico-pub': '[   2] [ESPIDF]{ESP32} Publication...')
>> [Subscriber] Pulled ('demo/example/zenoh-pico-pub': '[   3] [ESPIDF]{ESP32} Publication...')
>> [Subscriber] Pulled ('demo/example/zenoh-pico-pub': '[   4] [ESPIDF]{ESP32} Publication...')
>> [Subscriber] Nothing to pull... sleep for 5000 ms
```

Note: with `SIZE = 3`, if more than 3 publications arrive before the next poll, **the oldest are dropped**. In the example above, 5 publications per 5-second window (1/sec) all fit comfortably.

### High-Rate Test

To test ring buffer overflow, publish faster than the pull rate:

```bash
# 50 messages at 0.1 second intervals — ring will overflow (capacity 3)
uv run python3 scripts/z_pull.py -n 50 -d 0.1 "Fast data"
```

The ring holds only the **latest 3** — you'll miss the older ones. Increase `SIZE` if you need a larger buffer.

---

## Customisation Guide

### Change Ring Buffer Size

```c
const size_t SIZE = 64;   // Hold up to 64 samples
```

Larger rings use more RAM but tolerate longer intervals between polls.

### Change Polling Frequency

```c
const size_t INTERVAL = 1000;   // Poll every 1 second
```

### Non-Blocking Poll (Check and Continue)

If you don't want to sleep between polls:

```c
while (true) {
    z_result_t res;
    for (res = z_try_recv(z_loan(handler), &sample);
         res == Z_OK;
         res = z_try_recv(z_loan(handler), &sample)) {
        // process sample...
        z_drop(z_move(sample));
    }
    if (res == Z_CHANNEL_NODATA) {
        // No data — do other work instead of sleeping
        process_sensor_readings();
        vTaskDelay(pdMS_TO_TICKS(100));  // Still yield to WiFi task
    }
}
```

### Process Different Key Expressions

The `**` wildcard matches any sub-path. You can inspect the key expression and handle different topics differently:

```c
z_view_string_t keystr;
z_keyexpr_as_view_string(z_sample_keyexpr(z_loan(sample)), &keystr);
const char *key = z_string_data(z_view_string_loan(&keystr));

if (strstr(key, "temperature")) {
    handle_temperature(payload);
} else if (strstr(key, "humidity")) {
    handle_humidity(payload);
}
```

---

## Troubleshooting

### ❌ `Z_FEATURE_SUBSCRIPTION` undefined

```bash
idf.py menuconfig
→ Component config → Zenoh pico → Enable subscription feature
```

### ❌ `Unable to open session!`

| Cause | Fix |
|-------|-----|
| No router (client mode) | Start `zenohd` or switch to peer mode |
| WiFi not connected | Check SSID and password |
| Firewall | Open UDP 7447 |

### ❌ `Unable to declare subscriber.`

Usually indicates a network issue after session open. Check router connectivity.

### ❌ Missing samples (gaps in received data)

The ring buffer starts dropping the oldest samples when it overflows. Increase `SIZE` or decrease `INTERVAL`:

```c
const size_t SIZE     = 64;     // Larger buffer
const size_t INTERVAL = 1000;   // Poll more frequently
```

### ❌ Ring channel functions not found at link time

Ensure zenoh-pico is compiled with subscription **and** ring channel support. Some builds strip optional features.

---

## Comparison: Pull vs. Callback Subscriber

| Decision Factor | Pick Pull (`z_pull.c`) | Pick Callback (`z_sub.c`) |
|-----------------|------------------------|---------------------------|
| Processing model | Polling cycle (check every N ms) | Event-driven (immediate callback) |
| Can block in handler? | N/A — processing in main loop | Yes — `sleep` or `vTaskDelay` is OK |
| Maximum latency | N ms (polling interval) | Sub-millisecond |
| CPU usage | Wakes up and processes in bursts | Idle until message arrives |
| Overflow behaviour | Drops oldest (bounded buffer) | N/A — callback is synchronous |
| RAM usage | Ring buffer pre-allocated | Minimal (no buffer) |

**Use the pull subscriber when:**
- Your application already has a fixed polling cycle (e.g., sensors read at 5 Hz)
- You want to batch-process publications
- You can tolerate the polling latency

**Use the callback subscriber when:**
- You need real-time response to every publication
- Publications are infrequent and you want zero CPU when idle
- You don't want to manage buffer sizing

---

## Reference Resources

| Resource | Link |
|----------|------|
| Zenoh Pub/Sub Concept | https://zenoh.io/docs/manual/abstractions/#publish-subscribe |
| zenoh-pico API | https://zenoh-pico.readthedocs.io/en/1.9.0/ |
| ESP-IDF WiFi Driver | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/api-reference/network/esp_wifi.html |
| FreeRTOS Event Groups | https://www.freertos.org/event-groups.html |
| Eclipse Public License 2.0 | https://www.eclipse.org/legal/epl-2.0/ |

---

## License

The source code corresponding to this document is dual-licensed under Eclipse Public License 2.0 or Apache License 2.0 — see the file header for details.
