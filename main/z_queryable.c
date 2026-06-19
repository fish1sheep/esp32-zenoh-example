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
// z_queryable.c  —  Zenoh Queryable example (responds to GET queries)  (ESP32-S3)
//
// This program demonstrates how to declare a Zenoh **queryable** on the
// ESP32-S3 — a node that listens for incoming GET requests and replies.
//
// Flow:
//   1) Initialise NVS flash.
//   2) Connect to WiFi (STA mode, blocking).
//   3) Open a Zenoh session (client or peer mode).
//   4) Declare a queryable on "demo/example/zenoh-pico-queryable".
//   5) Sleep forever — the query handler runs on each incoming query.
//
// What is a "Queryable"?
// ----------------------
// In Zenoh, a Queryable is the server side of the request-response pattern.
// Another node calls GET on a key expression; our handler receives the
// query and calls z_query_reply() to send back data.  This is analogous to
// a REST endpoint handler in an HTTP server.
//
// Build requirement: zenoh-pico must be compiled with Z_FEATURE_QUERYABLE
// enabled (see the #if guard below).
// ============================================================================

#include <esp_event.h>  /* ESP-IDF event loop — WiFi lifecycle */
#include <esp_log.h>    /* ESP-IDF logging */
#include <esp_system.h> /* ESP-IDF system API */
#include <esp_wifi.h>   /* WiFi driver */

/* Standard C */
#include <stdio.h>  /* printf / fprintf */
#include <stdlib.h> /* exit */
#include <string.h> /* strcmp */

/* ESP-IDF components */
#include <nvs_flash.h>  /* NVS flash storage */
#include <unistd.h>     /* sleep() */
#include <zenoh-pico.h> /* Zenoh client library */

/* FreeRTOS */
#include <freertos/FreeRTOS.h>     /* Kernel */
#include <freertos/event_groups.h> /* Event groups */
#include <freertos/task.h>         /* Task API */

// ============================================================================
// Compile-time feature guard
// ============================================================================
#if Z_FEATURE_QUERYABLE == 1

// ============================================================================
// WiFi configuration  (compile-time constants)
// ============================================================================
// [NOTE] Use NVS or a config file for production.
#define ESP_WIFI_SSID "SSID"             /* WiFi SSID           */
#define ESP_WIFI_PASS "PASS"             /* WiFi password       */
#define ESP_MAXIMUM_RETRY 5              /* Max WiFi retries    */
#define WIFI_CONNECTED_BIT BIT0          /* Event group: "Got IP" */

// ============================================================================
// Global state  (shared between event handler and main loop)
// ============================================================================
static bool               s_is_wifi_connected = false;
static EventGroupHandle_t s_event_group_handler;
static int                s_retry_count = 0;

// ============================================================================
// Session mode select  (client vs. peer)
// ============================================================================
// Controls how the ESP32 joins the Zenoh network.
//   Client mode: connect to a Zenoh router (or auto-discover via scout).
//   Peer mode:   listen on a UDP multicast address.
#define CLIENT_OR_PEER 0  // 0 = client, 1 = peer
#if CLIENT_OR_PEER == 0
#define MODE "client"
#define LOCATOR ""  /* Empty → scout for router */
#elif CLIENT_OR_PEER == 1
#define MODE "peer"
#define LOCATOR "udp/224.0.0.225:7447#iface=en0"
#else
#error "Unknown Zenoh operation mode.  Check CLIENT_OR_PEER value."
#endif

// ============================================================================
// Zenoh key expression and default reply value
// ============================================================================
#define KEYEXPR "demo/example/zenoh-pico-queryable"
#define VALUE "[ESPIDF]{ESP32} Queryable from Zenoh-Pico!"

// ============================================================================
// WiFi event callback
// ============================================================================
// Fired by ESP-IDF on WiFi / IP state transitions.
//   GOT_IP  →  signals the waiting task to continue.
//   DISCONNECTED  →  retries up to ESP_MAXIMUM_RETRY times.
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        /* WiFi driver ready — start association */
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        /* Link lost — retry if under the limit */
        if (s_retry_count < ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_count++;
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        /* DHCP success — signal app_main */
        xEventGroupSetBits(s_event_group_handler, WIFI_CONNECTED_BIT);
        s_retry_count = 0;
    }
}

// ============================================================================
// WiFi STA initialisation  (blocking — returns only after DHCP success)
// ============================================================================
void wifi_init_sta(void)
{
    s_event_group_handler = xEventGroupCreate();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));

    esp_event_handler_instance_t handler_any_id;
    esp_event_handler_instance_t handler_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &handler_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &handler_got_ip));

    wifi_config_t wifi_config = {.sta = {
                                     .ssid     = ESP_WIFI_SSID,
                                     .password = ESP_WIFI_PASS,
                                 }};

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    EventBits_t bits =
        xEventGroupWaitBits(s_event_group_handler, WIFI_CONNECTED_BIT, pdFALSE,
                            pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        s_is_wifi_connected = true;
    }

    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        IP_EVENT, IP_EVENT_STA_GOT_IP, handler_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        WIFI_EVENT, ESP_EVENT_ANY_ID, handler_any_id));
    vEventGroupDelete(s_event_group_handler);
}

// ============================================================================
// Query handler  —  called when a remote node sends GET on our key expr
// ============================================================================
// Receives a "loaned" (borrowed) query reference.  We read the query's
// key expression and optional payload, print them for logging, then send
// a reply with our pre-defined VALUE.
void query_handler(z_loaned_query_t *query, void *ctx)
{
    (void)(ctx);

    /* Print the key expression this query targeted */
    z_view_string_t keystr;
    z_keyexpr_as_view_string(z_query_keyexpr(query), &keystr);

    /* Print any query parameters (e.g. "?foo=bar") */
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

    /* Build and send the reply */
    z_view_keyexpr_t ke;
    z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);

    z_owned_bytes_t reply_payload;
    z_bytes_from_static_str(&reply_payload, VALUE);

    /*
     * z_query_reply() sends the response back to the querying node.
     * It is non-blocking — the reply is queued for the transport layer.
     */
    z_query_reply(query, z_loan(ke), z_move(reply_payload), NULL);
}

// ============================================================================
//  app_main  —  ESP32 application entry point
// ============================================================================
// Flow:  NVS → WiFi → Zenoh session → Declare queryable → Idle
void app_main()
{
    // ========================================================================
    // Step 1 — Initialise NVS flash
    // ========================================================================
    // NVS stores WiFi calibration and DHCP data.  Re-initialise if the
    // partition format has changed after a firmware upgrade.
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
    // Step 4 — Open Zenoh session
    // ========================================================================
    printf("Opening Zenoh Session...");
    z_owned_session_t s;
    if (z_open(&s, z_move(config), NULL) < 0)
    {
        printf("Unable to open session!\n");
        exit(-1);
    }
    printf("OK\n");

    // ========================================================================
    // Step 5 — Declare Queryable
    // ========================================================================
    /*
     * Register our query_handler for the key expression.
     * After this, any GET query on "demo/example/zenoh-pico-queryable"
     * from another Zenoh node will trigger a reply.
     */
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
    // Step 6 — Idle loop (wait for queries)
    // ========================================================================
    // The queryable runs asynchronously; we keep the task alive with a
    // simple infinite sleep.
    while (1)
    {
        sleep(1);
    }

    // ========================================================================
    // Step 7 — Cleanup  (unreachable)
    // ========================================================================
    printf("Closing Zenoh Session...");
    z_drop(z_move(qable));

    z_drop(z_move(s));
    printf("OK!\n");
}

// ============================================================================
// Fallback — Z_FEATURE_QUERYABLE not enabled
// ============================================================================
#else
void app_main()
{
    printf("ERROR: Zenoh pico was compiled without Z_FEATURE_QUERYABLE but "
           "this example requires it.\n");
}
#endif /* Z_FEATURE_QUERYABLE */
