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
// z_pull.c  —  Zenoh Pull-based Subscriber example  (ESP32-S3)
//
// This program demonstrates subscribing to a Zenoh topic using a **ring
// channel** — a bounded buffer where incoming publications are queued and
// the application "pulls" them at its own pace.
//
// Flow:
//   1) Initialise NVS flash.
//   2) Connect to WiFi (STA mode, blocking).
//   3) Open a Zenoh session.
//   4) Declare a subscriber on "demo/example/**" using a ring channel.
//   5) Poll the ring channel every 5 seconds, printing any pending data.
//
// Pull vs. Callback Subscriber:
// ------------------------------
//   - Callback subscriber  (z_sub.c):
//     The Zenoh library calls your data_handler immediately on every
//     received publication — real-time but requires the callback to be
//     quick (or push work to a queue).
//
//   - Pull subscriber  (this file):
//     Publications are buffered in a ring channel.  You call
//     z_try_recv() to drain the buffer when it's convenient.  This is
//     useful when the application has a fixed polling cycle and cannot
//     be interrupted.
//
// Build requirement: zenoh-pico must have Z_FEATURE_SUBSCRIPTION enabled.
// ============================================================================

#include <esp_event.h>  /* ESP-IDF event loop */
#include <esp_log.h>    /* ESP-IDF logging */
#include <esp_system.h> /* ESP-IDF system API */
#include <esp_wifi.h>   /* WiFi driver */

/* Standard C */
#include <stdio.h>  /* printf */
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
// The subscription feature can be compiled out of zenoh-pico — protect
// against link-time failures with this #if guard.
#if Z_FEATURE_SUBSCRIPTION == 1

// ============================================================================
// WiFi configuration
// ============================================================================
#define ESP_WIFI_SSID "SSID"             /* WiFi SSID to connect to      */
#define ESP_WIFI_PASS "PASS"             /* WiFi password                */
#define ESP_MAXIMUM_RETRY 5              /* Max retries on disconnect    */
#define WIFI_CONNECTED_BIT BIT0          /* Event group bit: "Got IP"    */

// ============================================================================
// Global state
// ============================================================================
static bool               s_is_wifi_connected = false;
static EventGroupHandle_t s_event_group_handler;
static int                s_retry_count = 0;

// ============================================================================
// Session mode  (client vs. peer)
// ============================================================================
#define CLIENT_OR_PEER 0  // 0 = client, 1 = peer
#if CLIENT_OR_PEER == 0
#define MODE "client"
#define LOCATOR ""  /* Empty → scout for a router */
#elif CLIENT_OR_PEER == 1
#define MODE "peer"
#define LOCATOR "udp/224.0.0.225:7447#iface=en0"
#else
#error "Unknown Zenoh operation mode.  Check CLIENT_OR_PEER value."
#endif

// ============================================================================
// Key expression and pull interval
// ============================================================================
#define KEYEXPR "demo/example/**"  /* Wildcard: matches any sub-path */

const size_t INTERVAL = 5000;  /* Polling interval in milliseconds (5 s) */
const size_t SIZE     = 3;     /* Ring channel capacity (max buffered msgs) */

// ============================================================================
// WiFi event callback
// ============================================================================
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        /* WiFi driver initialised — start association */
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        /* Connection dropped — retry a few times */
        if (s_retry_count < ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_count++;
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        /* DHCP success — wake up the waiting task */
        xEventGroupSetBits(s_event_group_handler, WIFI_CONNECTED_BIT);
        s_retry_count = 0;
    }
}

// ============================================================================
// WiFi STA initialisation (blocking)
// ============================================================================
void wifi_init_sta(void)
{
    /* Create event group for WiFi synchronisation */
    s_event_group_handler = xEventGroupCreate();

    /* Initialise the TCP/IP stack */
    ESP_ERROR_CHECK(esp_netif_init());

    /* Create the default event loop + STA netif */
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* WiFi driver init */
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));

    /* Register event handlers (WiFi link + IP layer) */
    esp_event_handler_instance_t handler_any_id;
    esp_event_handler_instance_t handler_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &handler_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &handler_got_ip));

    /* Set SSID + password */
    wifi_config_t wifi_config = {.sta = {
                                     .ssid     = ESP_WIFI_SSID,
                                     .password = ESP_WIFI_PASS,
                                 }};

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Block until GOT_IP */
    EventBits_t bits =
        xEventGroupWaitBits(s_event_group_handler, WIFI_CONNECTED_BIT, pdFALSE,
                            pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        s_is_wifi_connected = true;
    }

    /* Cleanup */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        IP_EVENT, IP_EVENT_STA_GOT_IP, handler_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        WIFI_EVENT, ESP_EVENT_ANY_ID, handler_any_id));
    vEventGroupDelete(s_event_group_handler);
}

// ============================================================================
//  app_main  —  ESP32 application entry point
// ============================================================================
//
// Unlike the callback-based subscriber (z_sub.c), this example uses a
// **ring channel** — a bounded FIFO buffer provided by Zenoh-pico.
//
// How the ring channel works:
//   1. z_ring_channel_sample_new() creates a closure + handler pair.
//      The closure is passed to z_declare_subscriber(); it secretly
//      writes incoming samples into the ring buffer.
//   2. z_try_recv() attempts to read one sample from the handler side.
//      Returns Z_OK on success, Z_CHANNEL_NODATA if the buffer is empty.
//   3. If the ring is full when a new publication arrives, the oldest
//      sample is dropped — the ring never blocks the publisher.
//
// This pattern is ideal for applications that poll on a fixed timer
// (e.g. sensors reading at 5 Hz) and cannot be interrupted by a callback.
void app_main()
{
    // ========================================================================
    // Step 1 — Initialise NVS
    // ========================================================================
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    // ========================================================================
    // Step 2 — Connect to WiFi
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
    // Step 5 — Declare a subscriber with a ring channel
    // ========================================================================
    /*
     * This is the key difference from the callback subscriber.
     *
     * z_ring_channel_sample_new() creates a pair:
     *   closure  ← passed to z_declare_subscriber; Zenoh writes into it
     *   handler  ← the application reads from it via z_try_recv()
     *
     * The ring holds up to SIZE (3) samples.  If full, the oldest is
     * dropped (no back-pressure on the publisher).
     */
    printf("Declaring Subscriber on '%s'...\n", KEYEXPR);
    z_owned_closure_sample_t   closure;
    z_owned_ring_handler_sample_t handler;
    z_ring_channel_sample_new(&closure, &handler, SIZE);
    z_owned_subscriber_t sub;
    z_view_keyexpr_t     ke;
    z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);
    if (z_declare_subscriber(z_loan(s), &sub, z_loan(ke), z_move(closure),
                             NULL) < 0)
    {
        printf("Unable to declare subscriber.\n");
        exit(-1);
    }

    // ========================================================================
    // Step 6 — Pull loop: drain the ring at our own pace
    // ========================================================================
    /*
     * Every INTERVAL milliseconds, we drain all pending samples from
     * the ring channel with z_try_recv().  If nothing is waiting, we
     * print "Nothing to pull" and go back to sleep.
     *
     * z_try_recv() is non-blocking — it returns immediately with
     * either a sample (Z_OK) or Z_CHANNEL_NODATA (empty).
     */
    printf("Pulling data every %zu ms... Ring size: %zd\n", INTERVAL, SIZE);
    z_owned_sample_t sample;
    while (true)
    {
        z_result_t res;
        /*
         * Drain the ring: keep calling z_try_recv() until it returns
         * Z_CHANNEL_NODATA or an error.
         */
        for (res = z_try_recv(z_loan(handler), &sample); res == Z_OK;
             res = z_try_recv(z_loan(handler), &sample))
        {
            /* Extract key expression and payload for printing */
            z_view_string_t keystr;
            z_keyexpr_as_view_string(z_sample_keyexpr(z_loan(sample)),
                                     &keystr);
            z_owned_string_t value;
            z_bytes_to_string(z_sample_payload(z_loan(sample)), &value);
            printf(">> [Subscriber] Pulled ('%.*s': '%.*s')\n",
                   (int)z_string_len(z_loan(keystr)),
                   z_string_data(z_loan(keystr)),
                   (int)z_string_len(z_loan(value)),
                   z_string_data(z_loan(value)));

            /* Free the owned strings and the sample */
            z_drop(z_move(value));
            z_drop(z_move(sample));
        }

        if (res == Z_CHANNEL_NODATA)
        {
            /* Ring is empty — nothing to do, sleep and try again */
            printf(">> [Subscriber] Nothing to pull... sleep for %zu ms\n",
                   INTERVAL);
            z_sleep_ms(INTERVAL);
        }
        else
        {
            /* Unexpected error — break out of the loop */
            break;
        }
    }

    // ========================================================================
    // Step 7 — Cleanup
    // ========================================================================
    z_drop(z_move(sub));
    z_drop(z_move(handler));

    z_drop(z_move(s));
    printf("OK!\n");
}

// ============================================================================
// Fallback  —  Z_FEATURE_SUBSCRIPTION not enabled
// ============================================================================
#else
void app_main()
{
    printf("ERROR: Zenoh pico was compiled without Z_FEATURE_SUBSCRIPTION but "
           "this example requires it.\n");
}
#endif /* Z_FEATURE_SUBSCRIPTION */
