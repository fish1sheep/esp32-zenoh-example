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
// z_get.c  —  Zenoh Queryable example (responds to GET queries)  (ESP32-S3)
//
// This program demonstrates how to declare a Zenoh **queryable** on the
// ESP32-S3 — a node that listens for incoming GET requests and replies.
//
// Flow:
//   1) Initialise NVS flash (required by ESP-IDF's WiFi stack).
//   2) Connect to WiFi (STA mode, blocking until IP assigned).
//   3) Open a Zenoh session (client or peer mode).
//   4) Declare a queryable on "demo/example/zenoh-pico-queryable".
//   5) Sleep forever — the queryable handler runs on each incoming query.
//
// What is a "Queryable"?
// ----------------------
// In Zenoh, a Queryable is the server side of the request-response pattern.
// Another Zenoh node can send a GET query on a key expression, and our
// queryable handler runs to produce a reply.  This is similar to an HTTP
// server handling GET requests.
//
// Build requirement: zenoh-pico must be compiled with Z_FEATURE_QUERYABLE
// enabled (see the #if guard below).
// ============================================================================

#include <esp_event.h>  /* ESP-IDF event loop — WiFi lifecycle events */
#include <esp_log.h>    /* ESP-IDF logging macros (ESP_LOGI, ESP_LOGE) */
#include <esp_system.h> /* ESP-IDF system API (esp_restart, chip_info) */
#include <esp_wifi.h>   /* WiFi driver — STA / AP mode */

/* Standard C headers */
#include <stdio.h>  /* printf / fprintf */
#include <stdlib.h> /* malloc / free / exit */
#include <string.h> /* strcmp, memset */

/* ESP-IDF components */
#include <nvs_flash.h>  /* Non-Volatile Storage (WiFi calibration data) */
#include <unistd.h>     /* POSIX sleep() */
#include <zenoh-pico.h> /* Zenoh lightweight client library */

/* FreeRTOS headers — task synchronisation primitives */
#include <freertos/FreeRTOS.h>     /* Kernel types (TickType_t, etc.) */
#include <freertos/event_groups.h> /* Event groups for WiFi sync */
#include <freertos/task.h>         /* Task creation API */

// ============================================================================
// Compile-time feature guard
// ============================================================================
// Z_FEATURE_QUERYABLE is set when zenoh-pico is built with queryable
// support.  If it's disabled (to save flash on constrained devices), the
// entire application body is replaced by a one-line error stub.
#if Z_FEATURE_QUERYABLE == 1

// ============================================================================
// WiFi configuration  (compile-time constants)
// ============================================================================
// [NOTE] In production, replace these with NVS reads or a config file.
#define ESP_WIFI_SSID "SSID"             /* WiFi SSID           */
#define ESP_WIFI_PASS "PASS"             /* WiFi password       */
#define ESP_MAXIMUM_RETRY 5              /* Max reconnection attempts */
#define WIFI_CONNECTED_BIT BIT0          /* Event group bit — "We have an IP" */

// ============================================================================
// Global state  (shared between main loop and WiFi event callback)
// ============================================================================
// These variables live for the entire lifetime of the program.
//   - s_is_wifi_connected:  flag polled by app_main's while loop
//   - s_event_group_handler:  FreeRTOS event group (signals GOT_IP)
//   - s_retry_count:  how many times we've retried WiFi association
static bool               s_is_wifi_connected = false;
static EventGroupHandle_t s_event_group_handler;
static int                s_retry_count = 0;

// ============================================================================
// Session mode select  (client vs. peer)
// ============================================================================
// CLIENT_OR_PEER controls how this node joins the Zenoh network:
//   0  →  Client mode: connect to a Zenoh router (or discover one via scout).
//   1  →  Peer mode:   listen on a UDP multicast address for other peers.
//
// LOCATOR:
//   - In client mode with LOCATOR empty, Zenoh uses UDP multicast (scout)
//     to find a router automatically.
//   - In client mode with a LOCATOR, it connects to that specific endpoint.
//   - In peer mode, LOCATOR is the listening endpoint.
#define CLIENT_OR_PEER 0  // 0 = client, 1 = peer
#if CLIENT_OR_PEER == 0
#define MODE "client"
#define LOCATOR ""  /* Empty → auto-discover via scout */
#elif CLIENT_OR_PEER == 1
#define MODE "peer"
#define LOCATOR "udp/224.0.0.225:7447#iface=en0"
#else
#error "Unknown Zenoh operation mode.  Check CLIENT_OR_PEER value."
#endif

// ============================================================================
// Zenoh key expression and default reply value
// ============================================================================
// KEYEXPR is the topic we "answer" — any GET query targeting this key or
// its wildcard expansion will reach our query handler.
#define KEYEXPR "demo/example/zenoh-pico-queryable"
#define VALUE "[ESPIDF]{ESP32} Queryable from Zenoh-Pico!"

// ============================================================================
// WiFi event callback
// ============================================================================
// ESP-IDF calls this function when WiFi or IP-layer events fire.
//
// Event lifecycle:
//   WIFI_EVENT_STA_START        →  Driver ready; begin association.
//   WIFI_EVENT_STA_DISCONNECTED →  Link dropped; retry up to the limit.
//   IP_EVENT_STA_GOT_IP         →  DHCP assigned an address; wake up.
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        /* WiFi driver initialised — try to associate with the AP */
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        /* Association lost — retry a few times, then give up silently */
        if (s_retry_count < ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_count++;
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        /* DHCP lease obtained — signal the waiting task to proceed */
        xEventGroupSetBits(s_event_group_handler, WIFI_CONNECTED_BIT);
        s_retry_count = 0;
    }
}

// ============================================================================
// WiFi initialisation (blocking)
// ============================================================================
// This function will NOT return until the ESP32 has a valid IP address.
//
// Steps:
//   1.  Create a FreeRTOS event group for WiFi synchronisation.
//   2.  Initialise ESP-IDF's network interface layer (lwIP).
//   3.  Create the default event loop + STA netif.
//   4.  Initialise the WiFi driver.
//   5.  Register our event handlers for WiFi and IP events.
//   6.  Configure SSID / password and start WiFi.
//   7.  Block on xEventGroupWaitBits() until GOT_IP fires.
//   8.  Clean up: unregister handlers and delete the event group.
void wifi_init_sta(void)
{
    /*
     * Step 1 — Create a FreeRTOS event group.
     * The WiFi event handler will set a bit in this group when DHCP
     * succeeds; we wait on that bit here.
     */
    s_event_group_handler = xEventGroupCreate();

    /*
     * Step 2 — Initialise the TCP/IP stack (lwIP via ESP-IDF's netif).
     * This must happen before any esp_netif_create_* call.
     */
    ESP_ERROR_CHECK(esp_netif_init());

    /*
     * Step 3 — Event loop + default STA network interface.
     * The event loop is the central dispatcher for all system events.
     * esp_netif_create_default_wifi_sta() creates a netif bound to WiFi.
     */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /*
     * Step 4 — WiFi driver initialisation.
     * WIFI_INIT_CONFIG_DEFAULT() provides sensible defaults for the
     * ESP32-S3's WiFi hardware.
     */
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));

    /*
     * Step 5 — Register event handlers.
     * We need TWO registrations: one for WiFi link events (start /
     * disconnect) and one for the IP-layer GOT_IP event.
     */
    esp_event_handler_instance_t handler_any_id;
    esp_event_handler_instance_t handler_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &handler_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &handler_got_ip));

    /*
     * Step 6 — Configure SSID + password, then start WiFi.
     * esp_wifi_start() triggers WIFI_EVENT_STA_START, which our event
     * handler catches to call esp_wifi_connect().
     */
    wifi_config_t wifi_config = {.sta = {
                                     .ssid     = ESP_WIFI_SSID,
                                     .password = ESP_WIFI_PASS,
                                 }};

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /*
     * Step 7 — Block until DHCP obtains an IP address.
     * portMAX_DELAY means wait forever.  When the handler sets
     * WIFI_CONNECTED_BIT, xEventGroupWaitBits returns.
     */
    EventBits_t bits =
        xEventGroupWaitBits(s_event_group_handler, WIFI_CONNECTED_BIT, pdFALSE,
                            pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        s_is_wifi_connected = true;
    }

    /*
     * Step 8 — Cleanup.
     * The event handlers are no longer needed; unregister them and
     * delete the event group to free memory.
     */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        IP_EVENT, IP_EVENT_STA_GOT_IP, handler_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        WIFI_EVENT, ESP_EVENT_ANY_ID, handler_any_id));
    vEventGroupDelete(s_event_group_handler);
}

// ============================================================================
// Query handler  —  called when a remote node sends a GET on our key expr
// ============================================================================
// When another Zenoh node publishes a GET query matching our key
// expression, Zenoh calls this function with a "loaned" query reference.
//
// The handler:
//   1. Extracts the query's key expression and parameters (for logging).
//   2. Reads the optional payload value from the query.
//   3. Sends a reply back with our predefined VALUE.
//
// z_loaned_query_t is a borrowed (non-owning) reference — Zenoh manages
// its lifetime.  We must not free it.
void query_handler(z_loaned_query_t *query, void *ctx)
{
    (void)(ctx);  /* Unused — kept for API compatibility */

    /* Extract and print the key expression that the query targeted */
    z_view_string_t keystr;
    z_keyexpr_as_view_string(z_query_keyexpr(query), &keystr);

    /* Extract and print any query parameters (e.g. "?foo=bar") */
    z_view_string_t params;
    z_query_parameters(query, &params);

    printf(" >> [Queryable handler] Received Query '%.*s%.*s'\n",
           (int)z_string_len(z_loan(keystr)),
           z_string_data(z_loan(keystr)),
           (int)z_string_len(z_loan(params)),
           z_string_data(z_loan(params)));

    /* If the query carried a payload, extract and print it */
    z_owned_string_t payload_string;
    z_bytes_to_string(z_query_payload(query), &payload_string);
    if (z_string_len(z_loan(payload_string)) > 0)
    {
        printf("     with value '%.*s'\n",
               (int)z_string_len(z_loan(payload_string)),
               z_string_data(z_loan(payload_string)));
    }
    z_drop(z_move(payload_string));

    /*
     * Build the reply.
     * We reply on the same key expression that the query targeted.
     */
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);

    z_owned_bytes_t reply_payload;
    z_bytes_from_static_str(&reply_payload, VALUE);

    /*
     * Send the reply.  z_query_reply() is non-blocking — it queues the
     * reply for delivery by the Zenoh transport layer.
     */
    z_query_reply(query, z_loan(ke), z_move(reply_payload), NULL);
}

// ============================================================================
//  app_main  —  ESP32 application entry point
// ============================================================================
// ESP-IDF calls app_main() after the ROM bootloader and FreeRTOS startup
// have finished.  It runs as a FreeRTOS task (not the idle task).
//
// Flow:  1. NVS init  →  2. WiFi connect  →  3. Zenoh session  →
//       4. Declare queryable  →  5. Idle (wait for queries forever)
void app_main()
{
    // ========================================================================
    // Step 1 — Initialise NVS (Non-Volatile Storage)
    // ========================================================================
    // NVS is ESP-IDF's flash-based key-value store.  The WiFi stack and
    // the TCP/IP stack use it internally.  If the NVS partition format
    // has changed (firmware upgrade), erase and re-initialise.
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ========================================================================
    // Step 2 — Connect to WiFi (blocking)
    // ========================================================================
    printf("Connecting to WiFi...");
    wifi_init_sta();
    while (!s_is_wifi_connected)
    {
        printf(".");
        sleep(1);
    }
    printf("OK!\n");

    // ========================================================================
    // Step 3 — Build Zenoh configuration
    // ========================================================================
    // Start from the default config, then set the session mode and
    // optionally the locator based on the CLIENT_OR_PEER flag.
    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, MODE);
    if (strcmp(LOCATOR, "") != 0)
    {
        if (strcmp(MODE, "client") == 0)
        {
            zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, LOCATOR);
        }
        else
        {
            zp_config_insert(z_loan_mut(config), Z_CONFIG_LISTEN_KEY, LOCATOR);
        }
    }

    // ========================================================================
    // Step 4 — Open the Zenoh session
    // ========================================================================
    // z_open() establishes a Zenoh protocol connection (TCP or UDP
    // transport) with a router or peer.  Returns < 0 on failure.
    printf("Opening Zenoh Session...");
    z_owned_session_t s;
    if (z_open(&s, z_move(config), NULL) < 0)
    {
        printf("Unable to open session!\n");
        exit(-1);
    }
    printf("OK\n");

    // ========================================================================
    // Step 5 — Declare a Queryable (responds to GET queries)
    // ========================================================================
    // A Queryable is the server side of Zenoh's request-response pattern.
    // We give it:
    //   - the session handle
    //   - the key expression to answer
    //   - a closure containing our query_handler callback
    //
    // After this, any peer/router that sends a GET on "demo/example/zenoh-pico-queryable"
    // will trigger our query_handler and receive our VALUE as the reply.
    printf("Declaring Queryable on %s...", KEYEXPR);
    z_owned_closure_query_t callback;
    z_closure(&callback, query_handler, NULL, NULL);
    z_owned_queryable_t qable;
    z_view_keyexpr_t    ke;
    z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);
    if (z_declare_queryable(z_loan(s), &qable, z_loan(ke), z_move(callback),
                            NULL) < 0)
    {
        printf("Unable to declare queryable.\n");
        exit(-1);
    }
    printf("OK\n");
    printf("Zenoh setup finished!\n");

    // ========================================================================
    // Step 6 — Idle loop
    // ========================================================================
    // The queryable handler runs asynchronously when queries arrive.
    // We just sleep — the FreeRTOS scheduler handles background tasks.
    while (1)
    {
        sleep(1);
    }

    // ========================================================================
    // Step 7 — Cleanup  (unreachable under current infinite loop)
    // ========================================================================
    // In production code you might break out on a shutdown signal and
    // clean up resources in reverse declaration order.
    printf("Closing Zenoh Session...");
    z_drop(z_move(qable));

    z_drop(z_move(s));
    printf("OK!\n");
}

// ============================================================================
// Fallback  —  Z_FEATURE_QUERYABLE not enabled at build time
// ============================================================================
#else
void app_main()
{
    printf("ERROR: Zenoh pico was compiled without Z_FEATURE_QUERYABLE but "
           "this example requires it.\n");
}
#endif /* Z_FEATURE_QUERYABLE */
