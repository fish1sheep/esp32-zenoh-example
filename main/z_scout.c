//
// Copyright (c) 2022 ZettaScale Technology
//
// This program and the accompanying materials are made available under the
// terms of the Eclipse Public License 2.0 which is available at
// http://www.eclipse.org/legal/epl-2.0, or the Apache License, Version 2.0
// which is available at https://www.apache.org/licenses/LICENSE-2.0.
//
// SPDX-License-Identifier: EPL-2.0 OR Apache-2.0
//
// Contributors:
//   ZettaScale Zenoh Team, <zenoh@zettascale.tech>

// ============================================================================
// z_scout.c -- Zenoh network discovery (Scout) example (ESP32-S3)
//
// This program demonstrates how to use Zenoh's "scout" feature on the
// ESP32-S3 to discover Zenoh nodes (peer/router) on the local network.
// Flow:
//   1) Initialize NVS flash (required by ESP-IDF for storing WiFi config, etc.)
//   2) Connect to WiFi (STA mode)
//   3) Send a Zenoh scout broadcast -- essentially shouts "Anyone using Zenoh?"
//   4) Collect replies (Hello messages), print each node's ID, role, and address
//
// Zenoh Scout vs. Ping:
//   - Scout discovers Zenoh nodes themselves (routers or peers), not just IP
//     reachability.
//   - Each Zenoh node replies with a Hello containing its ZID (Zenoh ID),
//     role (peer/router), and locators (communication endpoint addresses).
//
// Build requirement: zenoh-pico must be compiled with `Z_FEATURE_SCOUTING`
// enabled (see the #if guard at the bottom).
// ============================================================================

#include <esp_event.h>  // ESP-IDF event loop -- event-driven WiFi state machine
#include <esp_log.h>    // ESP-IDF logging (ESP_LOGI / ESP_LOGE etc.)
#include <esp_system.h> // ESP-IDF system-level API (esp_restart, chip_info, etc.)
#include <esp_wifi.h>   // WiFi driver -- STA / AP mode control

#include <stdio.h>  // C standard I/O -- printf / fprintf
#include <stdlib.h> // C standard library -- malloc / free
#include <string.h> // String manipulation

#include <nvs_flash.h>  // NVS (Non-Volatile Storage) -- WiFi config, calibration data
#include <unistd.h>     // POSIX -- sleep()
#include <zenoh-pico.h> // Zenoh-pico client library -- lightweight pub/sub/scout

#include <freertos/FreeRTOS.h>     // FreeRTOS kernel -- tasks, event groups, queues
#include <freertos/event_groups.h> // Event groups -- task synchronization (WiFi connect notification)
#include <freertos/task.h>         // FreeRTOS task API

// ============================================================================
// Compile-time feature guard
// ============================================================================
// Z_FEATURE_SCOUTING is a zenoh-pico compile-time switch. If scouting was
// disabled at build time (e.g. to reduce firmware size), the entire application
// logic is excluded and only an error message is emitted.
// This is a common pattern in embedded Zenoh applications -- feature toggling.
#if Z_FEATURE_SCOUTING == 1

// ============================================================================
// WiFi configuration constants
// ============================================================================
// [NOTE] In production, the SSID and password should be read from NVS or a
// config file, not hardcoded. They are simplified here for the example.
#define ESP_WIFI_SSID "SSID"  // WiFi SSID to connect to
#define ESP_WIFI_PASS "PASS" // WiFi password
#define ESP_MAXIMUM_RETRY 5        // Max WiFi connection retries
#define WIFI_CONNECTED_BIT BIT0    // Event group bit representing "WiFi connected"

// ============================================================================
// Global state variables
// ============================================================================
// In embedded programs, shared state between event callbacks and tasks is
// typically done with global variables or event groups. Here we use both:
//   - s_is_wifi_connected: bool flag polled by the while loop in app_main
//   - s_event_group_handler: FreeRTOS event group for blocking on WiFi connect
//   - s_retry_count: tracks consecutive retries; gives up past the limit
static bool s_is_wifi_connected = false; // Whether WiFi is connected (queried by main loop)
static EventGroupHandle_t s_event_group_handler; // Event group handle (WiFi event sync)
static int                s_retry_count = 0;     // Current WiFi retry count

// ============================================================================
// WiFi event callback
// ============================================================================
// ESP-IDF uses an event-driven model for WiFi state changes. We register a
// callback that the system invokes automatically on state transitions.
//
// Parameters:
//   arg         -- user context pointer passed at registration (unused here)
//   event_base  -- event category: WIFI_EVENT or IP_EVENT
//   event_id    -- specific event ID, e.g. WIFI_EVENT_STA_START / IP_EVENT_STA_GOT_IP
//   event_data  -- event-specific data structure (unused here)
//
// Event flow:
//   WIFI_EVENT_STA_START        -> calls esp_wifi_connect() to begin connection
//   WIFI_EVENT_STA_DISCONNECTED -> retry (up to ESP_MAXIMUM_RETRY times)
//   IP_EVENT_STA_GOT_IP         -> connection succeeded, set event group bit, reset retry count
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    // --- WiFi driver events ---
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        // STA mode started -> initiate connection
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        // Connection lost (wrong password, weak signal, AP reboot, etc.) -> auto-retry
        if (s_retry_count < ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_count++;
        }
        // Past max retries: silently give up -- caller detects failure via event group timeout
    }
    // --- IP layer events ---
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        // Got an IP address (DHCP) -> WiFi connection fully established
        xEventGroupSetBits(s_event_group_handler, WIFI_CONNECTED_BIT);
        s_retry_count = 0; // Reset retry count for next use
    }
}

// ============================================================================
// WiFi STA mode initialization and connection
// ============================================================================
// This is a blocking function -- it waits for a successful WiFi connection
// before returning. Other FreeRTOS tasks (if any) can still run while blocked.
//
// Standard ESP-IDF WiFi initialization steps:
//   1. Create event group (for synchronization)
//   2. Initialize netif (network interface layer)
//   3. Create default event loop
//   4. Create default WiFi STA network interface
//   5. Initialize WiFi driver
//   6. Register event callbacks
//   7. Configure SSID / password
//   8. Start WiFi
//   9. Wait for connection (block on event group)
//   10. Cleanup: unregister callbacks, delete event group
void wifi_init_sta(void)
{
    // ---- Step 1: Create event group ----
    // Event groups are a FreeRTOS synchronization primitive that lets tasks wait
    // for arbitrary combinations of event bits.
    s_event_group_handler = xEventGroupCreate();

    // ---- Step 2: Initialize TCP/IP network interface ----
    // esp_netif_init() must be called before creating any netif.
    ESP_ERROR_CHECK(esp_netif_init());

    // ---- Step 3: Create default event loop ----
    // The event loop is ESP-IDF's event dispatch center. WiFi driver, IP stack,
    // etc. send events through it.
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // ---- Step 4: Create default WiFi STA netif ----
    // Create a netif (network interface) instance bound to WiFi STA mode.
    esp_netif_create_default_wifi_sta();

    // ---- Step 5: Initialize WiFi driver ----
    // WIFI_INIT_CONFIG_DEFAULT() provides sensible defaults for most use cases.
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));

    // ---- Step 6: Register event callbacks ----
    // Register two handlers: one for all WiFi events, one for IP acquisition.
    esp_event_handler_instance_t handler_any_id;
    esp_event_handler_instance_t handler_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &handler_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &handler_got_ip));

    // ---- Step 7: Configure WiFi (SSID + password) ----
    // Use C99 designated initializers to fill the wifi_config_t struct.
    wifi_config_t wifi_config = {.sta = {
                                     .ssid     = ESP_WIFI_SSID,
                                     .password = ESP_WIFI_PASS,
                                 }};

    // ---- Step 8: Set mode and start WiFi ----
    ESP_ERROR_CHECK(
        esp_wifi_set_mode(WIFI_MODE_STA)); // Set to STA (client) mode
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config)); // Apply config
    ESP_ERROR_CHECK(esp_wifi_start()); // Start WiFi -> triggers WIFI_EVENT_STA_START

    // ---- Step 9: Wait for connection ----
    // xEventGroupWaitBits blocks the current task until the specified event
    // bits are set (or a timeout). portMAX_DELAY means infinite wait --
    // production code should set a timeout to prevent permanent blocking.
    EventBits_t bits =
        xEventGroupWaitBits(s_event_group_handler, WIFI_CONNECTED_BIT, pdFALSE,
                            pdFALSE, portMAX_DELAY);

    // Verify the event bit was actually set (not a timeout return)
    if (bits & WIFI_CONNECTED_BIT)
    {
        s_is_wifi_connected = true;
    }

    // ---- Step 10: Cleanup ----
    // Once WiFi is connected, the temporary event listeners are no longer
    // needed -- unregister them to free resources.
    // Note: order of unregistration (IP_EVENT first vs. WIFI_EVENT first)
    // doesn't matter, but it's conventional to do IP first.
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        IP_EVENT, IP_EVENT_STA_GOT_IP, handler_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        WIFI_EVENT, ESP_EVENT_ANY_ID, handler_any_id));
    vEventGroupDelete(s_event_group_handler);
}

// ============================================================================
// Zenoh ID print function
// ============================================================================
// z_id_t is a Zenoh node's globally unique identifier (128 bits), stored as a
// raw byte array. This function formats it as an uppercase hexadecimal string
// for human readability.
//
// Possible output:
//   - "None"                           (ZID length 0 -- unknown / uninitialized)
//   - "Some(A1B2C3D4E5F6...)"          (normal case)
void fprintzid(FILE *stream, z_id_t zid)
{
    unsigned int zidlen = _z_id_len(zid);
    if (zidlen == 0)
    {
        fprintf(stream, "None");
    }
    else
    {
        fprintf(stream, "Some(");
        for (unsigned int i = 0; i < zidlen; i++)
        {
            fprintf(stream, "%02X", (int)zid.id[i]); // Convert byte by byte to hex
        }
        fprintf(stream, ")");
    }
}

// ============================================================================
// Zenoh node role print function
// ============================================================================
// z_whatami_t represents the role of a Zenoh node:
//   - Z_WHATAMI_ROUTER  -- forwards messages, typically runs on servers/gateways
//   - Z_WHATAMI_PEER    -- peer-to-peer communication, no central router
//   - Z_WHATAMI_CLIENT  -- connects to a single router only
//
// z_whatami_to_view_string converts the enum to a human-readable string (e.g. "router").
void fprintwhatami(FILE *stream, z_whatami_t whatami)
{
    z_view_string_t s;
    z_whatami_to_view_string(whatami, &s);
    fprintf(stream, "\"%.*s\"", (int)z_string_len(z_loan(s)),
            z_string_data(z_loan(s)));
}

// ============================================================================
// Locator list print function
// ============================================================================
// A locator is a Zenoh node's communication endpoint address, e.g.:
//   "tcp/192.168.1.100:7447"
//   "udp/192.168.1.100:7447"
// A node may have multiple locators (e.g. listening on both TCP and UDP).
//
// z_string_array_t is the zenoh-pico string array type.
void fprintlocators(FILE *stream, const z_loaned_string_array_t *locs)
{
    fprintf(stream, "[");
    for (unsigned int i = 0; i < z_string_array_len(locs); i++)
    {
        fprintf(stream, "\"");
        const z_loaned_string_t *str = z_string_array_get(locs, i);
        fprintf(stream, "%.*s", (int)z_string_len(str), z_string_data(str));
        fprintf(stream, "\"");
        if (i < z_string_array_len(locs) - 1)
        {
            fprintf(stream, ", "); // Comma separator except after the last element
        }
    }
    fprintf(stream, "]");
}

// ============================================================================
// Hello message full-print function
// ============================================================================
// A scout reply is called a "Hello" message and contains three parts:
//   1. zid       -- sender's Zenoh ID
//   2. whatami   -- sender's role (router / peer / client)
//   3. locators  -- sender's list of communication endpoints
//
// Output example:
//   Hello { zid: Some(A1B2C3D4E5F6), whatami: "router", locators:
//   ["tcp/192.168.1.100:7447"] }
void fprinthello(FILE *stream, const z_loaned_hello_t *hello)
{
    fprintf(stream, "Hello { zid: ");
    fprintzid(stream, z_hello_zid(hello)); // Extract and print ZID
    fprintf(stream, ", whatami: ");
    fprintwhatami(stream, z_hello_whatami(hello)); // Extract and print role
    fprintf(stream, ", locators: ");
    fprintlocators(stream, zp_hello_locators(hello)); // Extract and print locator list
    fprintf(stream, " }");
}

// ============================================================================
// Scout reply callback function
// ============================================================================
// The Zenoh library calls this callback each time scout receives a Hello reply
// from a node. The context pointer points to a counter (int *) allocated in
// app_main.
//
// This is a typical Zenoh closure pattern:
//   - callback (handler)    : called on each message received
//   - drop (cleanup)        : called when the scout operation ends
//   - context               : shared state between the two
void callback(z_loaned_hello_t *hello, void *context)
{
    fprinthello(stdout, hello); // Format the Hello message as readable text
    fprintf(stdout, "\n");
    (*(int *)context)++; // Increment counter -- track how many replies received
}

// ============================================================================
// Scout end callback function
// ============================================================================
// Called when the scout operation finishes (timeout or manual cancellation).
// Prints a summary and frees the context memory.
//
// Note: if no replies were received, print a hint rather than staying silent --
// this is helpful for debugging network connectivity issues.
void drop(void *context)
{
    int count = *(int *)context;
    free(context); // Free the counter memory allocated by malloc in app_main
    if (!count)
    {
        printf("Did not find any zenoh process.\n");
    }
    else
    {
        printf("Dropping scout results.\n");
    }
}

// ============================================================================
// Main function -- ESP32 application entry point
// ============================================================================
// ESP-IDF's app_main is equivalent to standard C's main() -- it is the first
// function executed after system boot. Note that it is NOT a FreeRTOS task,
// but it can create tasks. In this example all operations execute sequentially
// in app_main's context (except when interrupted by event callbacks).
void app_main()
{
    // ========================================================================
    // Step 1: Initialize NVS flash
    // ========================================================================
    // NVS (Non-Volatile Storage) is ESP32's flash-based key-value store.
    // The WiFi stack, IP stack, and other components depend on NVS for storing
    // configuration data.
    //
    // ESP_ERR_NVS_NO_FREE_PAGES or ESP_ERR_NVS_NEW_VERSION_FOUND indicate that
    // the NVS partition needs to be erased and rebuilt (e.g. after a firmware
    // upgrade that changes the NVS format).
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase()); // Erase entire NVS partition
        ret = nvs_flash_init();             // Re-initialize
    }
    ESP_ERROR_CHECK(ret);

    // ========================================================================
    // Step 2: Connect to WiFi
    // ========================================================================
    printf("Connecting to WiFi...");
    wifi_init_sta(); // Block until connection succeeds
    // wifi_init_sta sets s_is_wifi_connected to true before returning,
    // but for robustness we still check the flag and wait.
    while (!s_is_wifi_connected)
    {
        printf(".");
        sleep(1);
    }
    printf("OK!\n");

    // ========================================================================
    // Step 3: Perform Zenoh Scout
    // ========================================================================
    // Procedure:
    //   1. Allocate context (counter) -- tracks how many Hello replies received
    //   2. Create default Zenoh config -- only defaults needed for scouting
    //   3. Create closure -- bind callback, drop functions and context
    //   4. Call z_scout -- sends a UDP broadcast, waits for replies
    //
    // z_scout is asynchronous: it returns immediately, and the Zenoh library
    // collects replies in the background. When collection completes (default
    // timeout), the drop function is called.
    int *context = (int *)malloc(sizeof(int));
    *context     = 0;

    z_owned_config_t config;
    z_config_default(&config); // Use default config -- can be modified to tune scout behavior

    z_owned_closure_hello_t closure;
    z_closure_hello(&closure, callback, drop, context);

    printf("Scouting...\n");
    // The third argument (NULL) to z_scout is scout configuration -- passing
    // NULL uses defaults. It can be used to control the scout scope (e.g. same
    // WiFi subnet vs. cross-subnet).
    z_scout(z_config_move(&config), z_closure_hello_move(&closure), NULL);

    // Note: at this point app_main returns, but the Zenoh scout operation may
    // still be running in the background (the drop callback hasn't been called
    // yet). In more complex applications, you would typically keep a task alive
    // to wait for async completion, or use a semaphore for synchronization.
}

// ============================================================================
// Fallback when Z_FEATURE_SCOUTING is not enabled
// ============================================================================
#else
void app_main()
{
    printf("ERROR: Zenoh pico was compiled without Z_FEATURE_SCOUTING but this "
           "example requires it.\n");
}
#endif
