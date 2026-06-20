# z_sub.md — ESP32 (S3 / C5) Zenoh Subscriber Tutorial

[← Back to docs](../README.md) | [中文版本](../zh/z_sub.md)

---

## Overview

`main/z_sub.c` is a Zenoh subscriber example for the **ESP32 (S3 / C5)**, built on **ESP-IDF v6.0**. It demonstrates how an embedded device connects to a WiFi network, opens a Zenoh session, subscribes to a topic, and prints incoming messages to the serial console.

### What is a Subscriber?

In Zenoh's publish/subscribe model:

- **Publisher** — sends messages on a key expression (topic)
- **Subscriber** — expresses interest in a key expression; receives all matching publications
- **Router (zenohd)** — optional; relays messages between publishers and subscribers that aren't directly connected

This example implements the **subscriber** side. Pair it with `scripts/z_pub.py` (or the C publisher `z_pub.c`) to see end-to-end communication.

### Key Features

| Feature | Description |
|---------|-------------|
| WiFi STA Connection | Connects to a specified SSID with automatic retry (up to 5 times) |
| Zenoh Session | Opens a Zenoh session in Client or Peer mode |
| Topic Subscription | Subscribes to `"demo/example/**"` and prints received messages |
| Memory Safety | Properly drops owned Zenoh strings to prevent leaks |
| Error Handling | Auto-erases NVS on init failure; exits immediately on Zenoh errors |

### Data Flow

```
[Publisher] ---> [Zenoh Network] ---> ESP32 (S3 / C5) (z_sub.c) ---> Serial Console
                                           │
                                    data_handler() callback
                                    prints key + payload
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
| zenoh-pico | v1.9.0 | Zenoh protocol stack (C library, embedded-friendly) |
| xtensa-esp32s3-elf-gcc | — | Cross-compiler toolchain |
| Python 3 | 3.8+ | For the companion `z_pub.py` publisher script |
| zenoh-python | — | Python Zenoh bindings (for the PC-side publisher) |

### Network

- A 2.4 GHz WiFi access point (SSID + password)
- A Zenoh router (`zenohd`) running on the same network, OR a peer publisher

---

## Code Walkthrough

The following sections explain each part of the source code in the order it appears.

### 1. License Header

```c
// Copyright (c) 2022 ZettaScale Technology
// SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
```

Written by the ZettaScale Zenoh team, dual-licensed under **Eclipse Public License 2.0** or **Apache License 2.0**.

### 2. Include Headers

```c
#include <esp_event.h>
#include <esp_log.h>
#include <esp_system.h>
#include <esp_wifi.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <nvs_flash.h>
#include <unistd.h>
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

### 3. Compile-Time Guard: `#if Z_FEATURE_SUBSCRIPTION == 1`

```c
#if Z_FEATURE_SUBSCRIPTION == 1
// ... main body (all subscriber logic) ...
#else
void app_main()
{
    printf("ERROR: ...\n");
}
#endif
```

This is a **compile-time feature guard**. zenoh-pico supports building with only the features you need to save flash and RAM. If the library was compiled **without** subscription support (`Z_FEATURE_SUBSCRIPTION != 1`), this example falls back to an error message instead of failing at link time.

> **First-time users**: Check `sdkconfig` to confirm subscription is enabled:
> ```
> idf.py menuconfig
> → Component config → Zenoh pico → Enable subscription feature
> ```

The guard is important because:

- Embedding on a tiny MCU may require stripping unnecessary code
- The linker would produce cryptic errors if the subscriber APIs aren't compiled in
- The `#else` branch makes the failure mode explicit and readable

### 4. WiFi Configuration Macros

```c
#define ESP_WIFI_SSID "SSID"
#define ESP_WIFI_PASS "PASS"
#define ESP_MAXIMUM_RETRY 5
#define WIFI_CONNECTED_BIT BIT0
```

| Macro | Meaning |
|-------|---------|
| `ESP_WIFI_SSID` | Your WiFi access point name (must be 2.4 GHz — ESP32-S3 does not support 5 GHz; ESP32-C5 supports 5 GHz but is used on 2.4 GHz here) |
| `ESP_WIFI_PASS` | Your WiFi password |
| `ESP_MAXIMUM_RETRY` | How many times to retry connection after a disconnect |
| `WIFI_CONNECTED_BIT` | `BIT0` = `0x01` — a single bit in the FreeRTOS event group used to signal "WiFi is ready" |

> ⚠️ **Security note**: Hardcoded credentials are fine for prototyping on a lab network, but never commit real credentials to source control. For production, read WiFi settings from NVS or use ESP-IDF's `wifi_provisioning` component.

### 5. Global State Variables

```c
static bool               s_is_wifi_connected = false;
static EventGroupHandle_t s_event_group_handler;
static int                s_retry_count = 0;
```

These are `static` (file-scope) variables shared between the event handler and `wifi_init_sta()`:

| Variable | Purpose |
|----------|---------|
| `s_is_wifi_connected` | Polled by `app_main` to confirm WiFi is ready; set to `true` by the event handler on `GOT_IP` |
| `s_event_group_handler` | FreeRTOS event group handle — the mechanism for the event callback to wake up the waiting main task |
| `s_retry_count` | Incremented on each disconnect; reset to 0 on successful DHCP; stops retrying when it reaches `ESP_MAXIMUM_RETRY` |

Why not just use a simple flag without the event group? Because `xEventGroupWaitBits()` puts the task into the **blocked** state — it consumes zero CPU while waiting. A polling loop with a simple flag would waste CPU cycles.

### 6. Zenoh Mode Selection

```c
#define CLIENT_OR_PEER 0 // 0: Client mode; 1: Peer mode
#if CLIENT_OR_PEER == 0
#define MODE "client"
#define LOCATOR ""       // If empty, it will scout
#elif CLIENT_OR_PEER == 1
#define MODE "peer"
#define LOCATOR "udp/224.0.0.225:7447#iface=en0"
#else
#error "Unknown Zenoh operation mode. Check CLIENT_OR_PEER value."
#endif
```

This preprocessor block selects between two fundamentally different networking topologies:

#### Client Mode (`CLIENT_OR_PEER = 0`)

```
[ESP32 Client] ---tcp/7447---> [zenohd Router] <---tcp/7447---> [PC Publisher]
```

- The ESP32 **connects** to a central Zenoh router (`zenohd`)
- With an **empty `LOCATOR`**, the ESP32 broadcasts a UDP multicast "scout" message to discover the router automatically
- The router handles message routing — publishers and subscribers never talk directly
- **Requires** a running `zenohd` on the same network
- **Recommended** for first-time users

#### Peer Mode (`CLIENT_OR_PEER = 1`)

```
[ESP32 Peer] ---udp/multicast---> [PC Peer]
```

- No router needed — devices discover each other via UDP multicast (address `224.0.0.225:7447`)
- The ESP32 **listens** on the multicast group and can both send and receive
- `#iface=en0` specifies the network interface (`en0` on macOS; adjust to `eth0` / `wlan0` / `以太网` on other OSes)
- Good for simple two-device setups, but less scalable than client mode

#### Choosing the Right Mode

| Situation | Recommended Mode |
|-----------|-----------------|
| Lab with a PC running `zenohd` | Client (scouting) |
| Head-to-head ESP32 ↔ laptop, no router | Peer |
| Production deployment | Client (with explicit endpoint) |
| Testing in a Docker container | Client (explicit `--connect tcp/...`) |

### 7. Key Expression (Topic Filter)

```c
#define KEYEXPR "demo/example/**"
```

This is the **topic filter** the subscriber declares interest in.

Zenoh key expressions support wildcards:

| Pattern | Matches |
|---------|---------|
| `demo/example/**` | Anything under `demo/example/`, e.g. `demo/example/foo`, `demo/example/a/b/c` |
| `demo/example/*` | Only one level: `demo/example/foo` ✓ ; `demo/example/a/b/c` ✗ |
| `demo/**/temp` | Any depth: `demo/temp`, `demo/room1/temp`, `demo/building/floor2/temp` |

The `**` wildcard is especially useful for subscribers that want to listen to a whole namespace — our publisher uses `demo/example/zenoh-pico-pub`, which falls right under the `demo/example/**` umbrella.

### 8. WiFi Event Handler

```c
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
```

ESP-IDF uses an event-driven model for WiFi. Instead of polling, we register a callback. Three events matter:

```
┌──────────────────────────────────────────────────────────┐
│  WIFI_EVENT_STA_START                                    │
│    └─ esp_wifi_connect()  →  begins association with AP  │
│                                                          │
│  WIFI_EVENT_STA_DISCONNECTED                             │
│    └─ if retries left: esp_wifi_connect()  →  retry      │
│    └─ else: stay disconnected                            │
│                                                          │
│  IP_EVENT_STA_GOT_IP                                     │
│    └─ Set WIFI_CONNECTED_BIT  →  wake up app_main        │
│    └─ Reset retry count                                  │
└──────────────────────────────────────────────────────────┘
```

Key insight: **we wait for `GOT_IP`, not `CONNECTED`**. The `CONNECTED` event fires when the WiFi association completes (layer 2), but at that point we don't yet have an IP address. `GOT_IP` fires only after DHCP succeeds (layer 3), which is when we can actually use TCP/UDP.

The `s_retry_count` mechanism prevents infinite reconnection loops. If the AP is down or credentials are wrong, the ESP32 retries up to `ESP_MAXIMUM_RETRY` (5) times and then stops.

### 9. `wifi_init_sta()` — WiFi Initialisation (Blocking)

This function implements the **standard ESP-IDF STA initialisation sequence**. It will NOT return until the ESP32 has a valid IP address.

```
┌───────────────────────────────────────────────┐
│  xEventGroupCreate()                          │
│  → creates a FreeRTOS event group for sync    │
├───────────────────────────────────────────────┤
│  esp_netif_init()                             │
│  → initialises the TCP/IP stack (lwIP)        │
├───────────────────────────────────────────────┤
│  esp_event_loop_create_default()              │
│  → starts the default system event loop       │
├───────────────────────────────────────────────┤
│  esp_netif_create_default_wifi_sta()          │
│  → creates a WiFi STA network interface object│
├───────────────────────────────────────────────┤
│  esp_wifi_init(&config)                       │
│  → initialises the WiFi driver (allocates     │
│    memory, sets up internal state machines)   │
├───────────────────────────────────────────────┤
│  Register event_handler for WIFI_EVENT        │
│     and IP_EVENT_STA_GOT_IP                   │
├───────────────────────────────────────────────┤
│  esp_wifi_set_mode(WIFI_MODE_STA)             │
│  esp_wifi_set_config(...)                     │
│  esp_wifi_start()                             │
│  → configures credentials and starts WiFi     │
├───────────────────────────────────────────────┤
│  xEventGroupWaitBits(... portMAX_DELAY)       │
│  → BLOCKS here until GOT_IP fires             │
├───────────────────────────────────────────────┤
│  Unregister handlers                          │
│  vEventGroupDelete()                          │
│  → cleanup — we're done with these            │
└───────────────────────────────────────────────┘
```

#### Step-by-step:

**Step 1 — Create Event Group**
```c
s_event_group_handler = xEventGroupCreate();
```
An event group is a FreeRTOS primitive. Each bit can be set from any context (including ISRs). Here we use bit 0 (`WIFI_CONNECTED_BIT = BIT0`) as a "WiFi ready" flag.

**Step 2 — Initialise Network Stack**
```c
esp_netif_init();
esp_event_loop_create_default();
esp_netif_create_default_wifi_sta();
```
- `esp_netif_init()` — starts lwIP, the lightweight TCP/IP stack
- `esp_event_loop_create_default()` — creates the task that dispatches system events
- `esp_netif_create_default_wifi_sta()` — creates the STA interface; without this, the WiFi driver has nowhere to attach

**Step 3 — Initialise WiFi Driver**
```c
wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
ESP_ERROR_CHECK(esp_wifi_init(&config));
```
`WIFI_INIT_CONFIG_DEFAULT()` is a macro that expands to a struct with sensible defaults. If you need customisation (e.g., dynamic RX buffer size), you can modify the struct fields before passing it to `esp_wifi_init`.

**Step 4 — Register Event Handlers**
```c
esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, ...);
esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, ...);
```
> **Why register before starting?** If we started WiFi first, the `STA_START` event could fire before the handler is registered, and we'd miss it.

Two separate registrations because WiFi events (`WIFI_EVENT`) and IP events (`IP_EVENT`) are on different event base types.

**Step 5 — Configure and Start**
```c
wifi_config_t wifi_config = {
    .sta = {
        .ssid     = ESP_WIFI_SSID,
        .password = ESP_WIFI_PASS,
    }
};
ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
ESP_ERROR_CHECK(esp_wifi_start());
```
The `wifi_config` struct uses C99 **designated initialisers** (the `.sta = {...}` syntax). The struct also supports optional fields like `.scan_method` or `.threshold` for advanced WiFi tuning.

**Step 6 — Block and Wait**
```c
EventBits_t bits = xEventGroupWaitBits(
    s_event_group_handler,     // event group handle
    WIFI_CONNECTED_BIT,        // bit(s) to wait on
    pdFALSE,                   // don't clear the bit on wake
    pdFALSE,                   // don't wait for ALL bits (only this one)
    portMAX_DELAY              // wait forever
);
```
`portMAX_DELAY` is the FreeRTOS constant for "block indefinitely". The scheduler will put this task to sleep and run other tasks (or the idle task). This consumes **0% CPU** while waiting — much better than a `while (!flag) {}` spin loop.

**Step 7 — Cleanup**
```c
ESP_ERROR_CHECK(esp_event_handler_instance_unregister(...));
vEventGroupDelete(s_event_group_handler);
```
After connection, we no longer need the event handlers or the event group. Deleting them frees a small amount of RAM. The WiFi connection itself persists — it's managed by the WiFi driver, not by these handlers.

### 10. `data_handler()` — Zenoh Subscriber Callback

```c
void data_handler(z_loaned_sample_t *sample, void *arg)
```

This is the heart of the subscriber. Every time a matching publication arrives, Zenoh calls this function.

```
[Publisher] → publication on "demo/example/hello"
       │
       ▼
[Zenoh transport] receives and decodes the message
       │
       ▼
[zenoh-pico] constructs a z_loaned_sample_t
       │
       ▼
[data_handler()] is invoked with the loaned sample
       │
       ├─ Extract keyexpr → "demo/example/hello"
       ├─ Extract payload → e.g. "Hello world!"
       ├─ Print to serial
       └─ Drop owned string
```

#### Understanding "Loaned" vs "Owned" Types

This is one of the most important concepts in zenoh-pico (and Rust-influenced C APIs):

| Concept | Meaning | Example | Must you free it? |
|---------|---------|---------|-------------------|
| **Loaned** (borrowed) | A read-only view of data owned by someone else | `z_loaned_sample_t *sample` | **No** — Zenoh will free it later |
| **Owned** | Memory you control | `z_owned_string_t value` | **Yes** — call `z_string_drop()` |
| **View** | A non-owning string slice | `z_view_string_t keystr` | **No** — it borrows from the sample |

```c
void data_handler(z_loaned_sample_t *sample, void *arg)
{
    // 1. Extract key expression as a VIEW (borrowed, no free needed)
    z_view_string_t keystr;
    z_keyexpr_as_view_string(z_sample_keyexpr(sample), &keystr);

    // 2. Convert payload bytes to an OWNED string (MUST free later)
    z_owned_string_t value;
    z_bytes_to_string(z_sample_payload(sample), &value);

    // 3. Print — using "%.*s" which takes (length, pointer) for safety
    printf(" >> [Subscriber handler] Received ('%.*s': '%.*s')\n",
           (int)z_string_len(z_view_string_loan(&keystr)),
           z_string_data(z_view_string_loan(&keystr)),
           (int)z_string_len(z_string_loan(&value)),
           z_string_data(z_string_loan(&value)));

    // 4. Free the owned string — LEAK if this is skipped!
    z_string_drop(z_string_move(&value));
}
```

**Why `z_string_drop(z_string_move(&value))`?** This is Zenoh's move-and-drop pattern:
- `z_string_move(&value)` — takes ownership away from the local variable, leaving it in a "moved" (invalid) state
- `z_string_drop(...)` — frees the underlying memory

If you forget this call, each received message leaks however many bytes the payload was.

**Note on `printf` format**: `%.*s` takes two arguments — an `int` (length) and a `char*` (pointer). It prints exactly that many characters, which is safer than `%s` because the payload might contain null bytes or might not be null-terminated in the loaned view.

### 11. `app_main()` — Entry Point

As the FreeRTOS application entry point, `app_main` runs sequentially through 7 logical steps:

```
┌─────────────────────────────────────────────┐
│  Step 1: NVS initialisation                 │
│  (nvs_flash_init)                           │
│  → mounts the NVS partition                 │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 2: WiFi connection (blocking)         │
│  (wifi_init_sta)                            │
│  → blocks until DHCP assigns an IP          │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 3: Zenoh config                       │
│  (z_config_default + zp_config_insert)      │
│  → builds client or peer configuration      │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 4: Open Zenoh session                 │
│  (z_open)                                   │
│  → connects to router or peer network       │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 5: Declare subscriber                 │
│  (z_declare_subscriber)                     │
│  → subscribes to "demo/example/**"          │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 6: Idle loop                          │
│  (while(1) { sleep(1); })                   │
│  → callback handles all incoming messages   │
└────────────────┬────────────────────────────┘
                 │
┌────────────────▼────────────────────────────┐
│  Step 7: Cleanup (unreachable)              │
│  (z_drop)                                   │
│  → undeclare subscriber, close session      │
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

**Why do we need NVS?** ESP-IDF stores WiFi calibration data, MAC addresses, and DHCP client configuration in the NVS (Non-Volatile Storage) partition. The network stack (`esp_netif`) won't work until this is initialised.

The erase-and-retry pattern handles two specific errors:
- `ESP_ERR_NVS_NO_FREE_PAGES` — the NVS partition is full (e.g., after many writes during development)
- `ESP_ERR_NVS_NEW_VERSION_FOUND` — the on-flash NVS format is from an older ESP-IDF version; the current code can't read it

`ESP_ERROR_CHECK(ret)` is a macro that prints the error and aborts if `ret` is not `ESP_OK`. It's the ESP-IDF equivalent of an assertion.

#### Step 2 — WiFi Connection (Blocking)

```c
printf("Connecting to WiFi...");
wifi_init_sta();
while (!s_is_wifi_connected) {
    printf(".");
    sleep(1);
}
printf("OK!\n");
```

While `wifi_init_sta()` blocks internally via the event group, the polling loop is a **secondary safeguard**. If the event mechanism fails to wake in some edge case, this loop ensures execution does not proceed without WiFi.

The dots printed by the loop give visual feedback during development — you can see how many seconds the connection attempt took.

#### Step 3 — Zenoh Configuration

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

Zenoh's configuration is built incrementally:

1. `z_config_default()` — starts with sensible defaults (e.g., scouting enabled, 10-second timeout)
2. `zp_config_insert()` — overrides individual keys
3. `z_loan_mut(config)` — borrows the owned config mutably for the insert call

**When LOCATOR is empty (client scouting)**: Zenoh sends a UDP multicast "scout" message. Any router on the network that hears it responds with its connection info. The ESP32 then connects automatically.

**When LOCATOR is set**: The address is used directly — faster startup, no multicast dependency.

#### Step 4 — Open Zenoh Session

```c
z_owned_session_t s;
if (z_open(&s, z_move(config), NULL) < 0) {
    printf("Unable to open session!\n");
    exit(-1);
}
```

`z_open()`:
1. Resolves the endpoint (via scouting or directly)
2. Opens a transport connection (TCP for client, UDP multicast for peer)
3. Negotiates protocol capabilities with the router/peer
4. Returns an **owned session handle**

**`z_move(config)` semantics**: After this call, `config` is in a "moved" state — do not read from or free it. The session now owns whatever memory the config held.

**Error handling**: If `z_open` returns negative (router unreachable, network timeout, etc.), the program calls `exit(-1)`. On real hardware this causes a watchdog reset, and the ESP32 reboots to try again.

#### Step 5 — Declare Subscriber

```c
z_owned_closure_sample_t callback;
z_closure(&callback, data_handler, NULL, NULL);
z_owned_subscriber_t sub;
z_view_keyexpr_t ke;
z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);
if (z_declare_subscriber(z_loan(s), &sub, z_loan(ke), z_move(callback), NULL) < 0) {
    printf("Unable to declare subscriber.\n");
    exit(-1);
}
```

This is where the subscription actually happens:

**Building a closure:**
```c
z_owned_closure_sample_t callback;
z_closure(&callback, data_handler, NULL, NULL);
```
`z_closure` takes three function pointers:
1. The **callback** (`data_handler`) — called for each matching publication
2. An optional **context** (`NULL` here) — passed as the `arg` parameter to the callback
3. An optional **drop function** (`NULL` here) — called when the closure is destroyed

**Creating a key expression view:**
```c
z_view_keyexpr_t ke;
z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);
```
`z_view_keyexpr_t` is a **non-owning** string type. The `_unchecked` variant skips validation — we trust the compile-time macro to be a well-formed key expression.

**Declaring the subscriber:**
```c
z_declare_subscriber(z_loan(s), &sub, z_loan(ke), z_move(callback), NULL));
```
Parameters:
1. `z_loan(s)` — a borrowed reference to the session
2. `&sub` — output parameter; receives the subscriber handle
3. `z_loan(ke)` — borrowed key expression
4. `z_move(callback)` — ownership of the closure is transferred to the subscriber
5. `NULL` — no additional subscriber configuration

After this call, **the subscriber is active**. Any publication on `demo/example/**` will trigger `data_handler`.

#### Step 6 — Main Loop (Idle)

```c
while (1) {
    sleep(1);
}
```

The subscriber runs in the **background**. When a publication arrives:

1. Zenoh's transport layer (running inside the session) receives the bytes
2. zenoh-pico deserialises the message
3. The library calls `data_handler` from its internal context

Our loop just sleeps. No polling, no active work. This is the ideal subscriber pattern — **event-driven, zero CPU usage between messages**.

The `sleep(1)` call also gives the FreeRTOS idle task time to run, which handles low-power housekeeping.

#### Step 7 — Cleanup (Unreachable)

```c
z_drop(z_move(sub));
z_drop(z_move(s));
```

This code is unreachable because of the infinite loop above. It's shown for **educational purposes** — showing the correct teardown order:

1. `z_drop(z_move(sub))` — undeclares the subscriber and frees associated memory
2. `z_drop(z_move(s))` — closes the session and tears down the transport

### 12. Compile-Time Fallback

```c
#else
void app_main()
{
    printf("ERROR: Zenoh pico was compiled without Z_FEATURE_SUBSCRIPTION but "
           "this example requires it.\n");
}
#endif
```

If `Z_FEATURE_SUBSCRIPTION` is not 1, a stub `app_main` prints a clear error message. This is much more helpful than a linker error about missing symbols — especially for someone new to the project who might not know about the feature flags.

---

## Build & Flash

### 1. Configure WiFi Credentials

Edit `main/z_sub.c` — update the macros:

```c
#define ESP_WIFI_SSID "Your_SSID"
#define ESP_WIFI_PASS "Your_Password"
```

### 2. Select Zenoh Mode (optional)

```c
#define CLIENT_OR_PEER 0   // Client mode (recommended)
```

**Client mode** requires a running `zenohd` on the same network:

```bash
# Start the Zenoh router (on a PC on the same network)
zenohd
```

**Peer mode** does not need a router:

```c
#define CLIENT_OR_PEER 1
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
Connecting to WiFi....OK!
Opening Zenoh Session...OK
Declaring Subscriber on 'demo/example/**'...OK!
```

The subscriber is now waiting for messages. Press `Ctrl+]` to exit the monitor.

---

## Testing with a Publisher

### Using the Python Publisher (Recommended)

Install zenoh-python and run `z_pub.py` from this project:

```bash
pip install zenoh

# One-shot message
python scripts/z_pub.py "Hello ESP32!"

# Interactive mode (type line by line)
python scripts/z_pub.py
```

When a message is published, the ESP32's serial output shows:

```
 >> [Subscriber handler] Received ('demo/example/zenoh-pico-pub': 'Hello ESP32!')
```

### Using Another ESP32 (z_pub.c)

Flash `z_pub.c` to another ESP32 (S3 / C5) on the same network. It publishes `[N] [ESPIDF]{ESP32} Publication from Zenoh-Pico!` every second — the subscriber will print each one.

### What You Should See

With the publisher running, the subscriber console shows:

```
Connecting to WiFi....OK!
Opening Zenoh Session...OK
Declaring Subscriber on 'demo/example/**'...OK!
 >> [Subscriber handler] Received ('demo/example/zenoh-pico-pub': 'Hello ESP32!')
 >> [Subscriber handler] Received ('demo/example/zenoh-pico-pub': '[   0] [ESPIDF]{ESP32} Publication from Zenoh-Pico!')
 >> [Subscriber handler] Received ('demo/example/zenoh-pico-pub': '[   1] [ESPIDF]{ESP32} Publication from Zenoh-Pico!')
```

---

## Customisation Guide

### Change the Subscription Topic

```c
#define KEYEXPR "sensor/temperature/#"
```

Remember: the publisher must publish to a key that matches this pattern.

### Change from Subscriber to Queryable

Zenoh also supports a **request-response** pattern (like HTTP, but over Zenoh). Replace subscriber with a queryable:

```c
z_owned_closure_query_t q_callback;
z_closure(&q_callback, query_handler, NULL, NULL);
z_owned_queryable_t qable;
z_declare_queryable(z_loan(s), &qable, z_loan(ke), z_move(q_callback), NULL);
```

### Read WiFi Credentials from NVS (Production)

Instead of hardcoded macros:

```c
nvs_handle_t nvs;
nvs_open("storage", NVS_READONLY, &nvs);
size_t len = sizeof(ssid_buf);
nvs_get_str(nvs, "wifi_ssid", ssid_buf, &len);
nvs_close(nvs);
```

This allows changing credentials without recompiling.

### Add a Timeout to WiFi Connection

Replace `portMAX_DELAY` with a timeout:

```c
EventBits_t bits = xEventGroupWaitBits(
    ..., pdMS_TO_TICKS(30000));  // 30-second timeout

if (!(bits & WIFI_CONNECTED_BIT)) {
    printf("WiFi timeout!\n");
    esp_restart();  // reboot and try again
}
```

---

## Troubleshooting

### ❌ `Unable to open session!`

| Possible Cause | Solution |
|----------------|----------|
| `zenohd` not running (Client mode) | Start `zenohd` on a PC on the same network |
| WiFi connection failed | Check SSID and password |
| Firewall blocking UDP 7447 | Open the port or allow multicast |
| Wrong network interface (Peer mode) | Change `en0` to the actual interface name |

### ❌ `Unable to declare subscriber!`

- **Router unreachable**: The session opened but the router dropped the connection before the subscriber declaration completed. Check `zenohd` console for errors.
- **Key expression invalid**: If you changed `KEYEXPR`, ensure it's a valid Zenoh key expression (slashes, alphanumeric, `*`, `**`).

### ❌ Subscriber connects but receives nothing

- **Key mismatch**: The publisher's key must match the subscriber's filter. If subscriber uses `demo/example/**`, the publisher must publish under `demo/example/...`.
- **Network isolation**: Ensure both devices are on the same subnet (client mode) or can reach the multicast group (peer mode).
- **Router not forwarding**: Some `zenohd` configurations block certain key expressions. Check the router's access control settings.

### ❌ Garbled Serial Output

```
ESP-ROM:esp32s3-xxxxxxxx
```
Check that `idf.py monitor` uses the correct baud rate (115200 by default), or press `Ctrl+T` → `Ctrl+Y` to reset the board.

### ❌ Compilation Error: `Z_FEATURE_SUBSCRIPTION` undefined

Ensure the feature is enabled in `sdkconfig`:

```bash
idf.py menuconfig
# → Component config → Zenoh pico → Enable subscription feature
# Verify it is checked
```

### ❌ Memory grows over time (leak)

Make sure `z_string_drop(z_string_move(&value))` is called in `data_handler`. Every received publication creates an owned string — skipping the drop leaks memory.

---

## Comparison: C Subscriber vs Python Publisher

| Aspect | C Subscriber (`z_sub.c`) | Python Publisher (`z_pub.py`) |
|--------|--------------------------|-------------------------------|
| Platform | ESP32 (S3 / C5) (embedded) | PC (any OS) |
| Role | Receives messages | Sends messages |
| Key expression | `demo/example/**` (wildcard) | `demo/example/zenoh-pico-pub` (fixed) |
| Mode | Client (scouting) | Client (explicit `tcp/...`) |
| WiFi | Built-in (ESP-IDF) | OS-managed |
| Memory model | Borrowed / Owned (manual drop) | Garbage collected |
| Lifecycle | Runs forever on device | One-shot or interactive |
| Transport | zenoh-pico (C) | zenoh-python |

---

## Reference Resources

| Resource | Link |
|----------|------|
| Zenoh Documentation | https://zenoh.io/docs/ |
| zenoh-pico API | https://zenoh-pico.readthedocs.io/en/1.9.0/ |
| Zenoh Python API | https://zenoh-python.readthedocs.io/ |
| ESP-IDF Programming Guide | https://docs.espressif.com/projects/esp-idf/en/latest/esp32s3/ |
| FreeRTOS Event Groups | https://www.freertos.org/event-groups.html |
| Eclipse Public License 2.0 | https://www.eclipse.org/legal/epl-2.0/ |

---

## License

The source code corresponding to this document is dual-licensed under Eclipse Public License 2.0 or Apache License 2.0 — see the file header for details.
