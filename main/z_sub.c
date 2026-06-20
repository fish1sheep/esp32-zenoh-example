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

#if Z_FEATURE_SUBSCRIPTION == 1
#define ESP_WIFI_SSID "SSID"
#define ESP_WIFI_PASS "PASS"
#define ESP_MAXIMUM_RETRY 5
#define WIFI_CONNECTED_BIT BIT0

static bool               s_is_wifi_connected = false;
static EventGroupHandle_t s_event_group_handler;
static int                s_retry_count = 0;

#define CLIENT_OR_PEER 0 // 0: Client mode; 1: Peer mode
#if CLIENT_OR_PEER == 0
#define MODE "client"
#define LOCATOR "" // If empty, it will scout
#elif CLIENT_OR_PEER == 1
#define MODE "peer"
#define LOCATOR "udp/224.0.0.225:7447#iface=en0"
#else
#error "Unknown Zenoh operation mode. Check CLIENT_OR_PEER value."
#endif

#define KEYEXPR "demo/example/**"

/*
 * WiFi event handler
 * ------------------
 * ESP-IDF fires events during the WiFi lifecycle. We care about three:
 *
 *   WIFI_EVENT_STA_START        →  WiFi driver initialised; begin association.
 *   WIFI_EVENT_STA_DISCONNECTED →  Link dropped; retry up to a max count.
 *   IP_EVENT_STA_GOT_IP         →  DHCP succeeded; signal the waiting loop.
 *
 * The GOT_IP event is the real "we're online" signal — the WiFi link may
 * come up before DHCP hands out an address, so waiting on CONNECTED (the
 * lower-level association event) would be too early.
 */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        if (s_retry_count < ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_count++;
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        xEventGroupSetBits(s_event_group_handler, WIFI_CONNECTED_BIT);
        s_retry_count = 0;
    }
}

/*
 * WiFi initialisation (blocking)
 * --------------------------------
 * This function will NOT return until the ESP32 has a valid IP address.
 *
 * The sequence is:
 *   1. Create a FreeRTOS event group — a lightweight flag that the event
 *      handler sets and we wait on.
 *   2. Call esp_netif_init() + esp_event_loop_create_default() to set up
 *      ESP-IDF's network and event subsystems.
 *   3. Create the default STA netif (the logical network interface).
 *   4. Register our event_handler for WiFi and IP events.
 *   5. Apply SSID/PSK and call esp_wifi_start().
 *   6. Block on xEventGroupWaitBits() until GOT_IP fires.
 *   7. Unregister handlers and clean up the event group.
 *
 * After this function, s_is_wifi_connected == true and we can use TCP/UDP.
 */
void wifi_init_sta(void)
{
    /*
     * Create a FreeRTOS event group so the WiFi event handler can signal
     * the waiting task (us) when DHCP succeeds.
     */
    s_event_group_handler = xEventGroupCreate();

    /*
     * ESP-IDF networking initialisation.
     * - esp_netif_init()      creates the TCP/IP stack (lwIP).
     * - esp_event_loop_create_default()  starts the system event loop.
     * - esp_netif_create_default_wifi_sta()  creates the STA network
     *   interface object with default config.
     */
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /*
     * Initialise the WiFi driver with default configuration, then register
     * our event handler for all WiFi events and the IP-layer GOT_IP event.
     * Note the two separate registrations: one catches WiFi link events
     * (start / disconnect), the other catches DHCP success.
     */
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));

    esp_event_handler_instance_t handler_any_id;
    esp_event_handler_instance_t handler_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &handler_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &handler_got_ip));

    /*
     * Configure SSID and password from compile-time macros.
     * (In production you'd read these from NVS or a config file.)
     */
    wifi_config_t wifi_config = {.sta = {
                                     .ssid     = ESP_WIFI_SSID,
                                     .password = ESP_WIFI_PASS,
                                 }};

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /*
     * Block here until the event handler sets WIFI_CONNECTED_BIT (which
     * only happens after DHCP succeeds).  portMAX_DELAY means "wait
     * forever" — in a real product you'd add a timeout and a fallback.
     */
    EventBits_t bits =
        xEventGroupWaitBits(s_event_group_handler, WIFI_CONNECTED_BIT, pdFALSE,
                            pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        s_is_wifi_connected = true;
    }

    /*
     * Cleanup: unregister the handlers we no longer need and delete the
     * event group.  The WiFi connection itself remains alive.
     */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        IP_EVENT, IP_EVENT_STA_GOT_IP, handler_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        WIFI_EVENT, ESP_EVENT_ANY_ID, handler_any_id));
    vEventGroupDelete(s_event_group_handler);
}

// Zenoh subscriber callback
// --------------------------
// Whenever someone publishes on a key matching "demo/example/**", Zenoh
// calls this function with a "loaned sample" — a borrowed view into the
// received data that we must NOT free.
//
// Our job:
//   1. Extract the key expression string (for printing).
//   2. Extract the payload bytes as a C string.
//   3. Print both to the serial console.
//   4. Drop the owned string we created (NOT the sample itself — Zenoh
//      manages that lifecycle).
void data_handler(z_loaned_sample_t *sample, void *arg)
{
    /*
     * z_loaned_sample_t is a "borrowed" (non-owning) reference — Zenoh
     * owns the memory.  We can read from it but must not free it.
     *
     * Extract the key expression as a view string, then loan it for
     * printf's "%.*s" (length + pointer) format.
     */
    z_view_string_t keystr;
    z_keyexpr_as_view_string(z_sample_keyexpr(sample), &keystr);

    /*
     * Convert the binary payload into an owned C string.
     * z_bytes_to_string allocates memory; we own the result and must
     * drop it afterwards.
     */
    z_owned_string_t value;
    z_bytes_to_string(z_sample_payload(sample), &value);

    printf(" >> [Subscriber handler] Received ('%.*s': '%.*s')\n",
           (int)z_string_len(z_view_string_loan(&keystr)),
           z_string_data(z_view_string_loan(&keystr)),
           (int)z_string_len(z_string_loan(&value)),
           z_string_data(z_string_loan(&value)));

    /*
     * Free the owned string.  If we skip this, we leak memory on every
     * received message.
     */
    z_string_drop(z_string_move(&value));
}

/*
 * Application entry point (FreeRTOS task)
 * ========================================
 * The ESP32 boots, runs through its ROM bootloader, then ESP-IDF's startup
 * code eventually calls app_main() — this function.  It runs as a FreeRTOS
 * task with a reasonably large stack.
 *
 * Execution flow (7 steps):
 *   1. NVS init       — initialise the flash storage (needed by WiFi).
 *   2. WiFi connect   — block until we have an IP address.
 *   3. Zenoh config   — build a session config (client or peer mode).
 *   4. Open session   — connect to the Zenoh router / network.
 *   5. Declare sub    — subscribe to a key expression.
 *   6. Idle loop      — sleep forever; data_handler prints messages.
 *   7. Cleanup        — drop resources (unreachable here, but illustrative).
 */
void app_main()
{
    /* =============================================================
     * Step 1 — NVS (Non-Volatile Storage) initialisation
     *
     * ESP-IDF stores WiFi calibration data and the DHCP client config
     * in a dedicated NVS partition.  We must mount it before using
     * esp_netif or esp_wifi.
     *
     * If the NVS partition is corrupt (wrong version / out of pages),
     * erase it and retry.  This is ESP-IDF's recommended pattern.
     * ============================================================= */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* =============================================================
     * Step 2 — WiFi connection (blocking)
     *
     * wifi_init_sta() registers event handlers, starts the WiFi driver,
     * and WAITS until DHCP hands out an IP address.  After this call
     * succeeds, the ESP32 is on the network and ready for TCP/UDP.
     * ============================================================= */
    printf("Connecting to WiFi...");
    wifi_init_sta();
    while (!s_is_wifi_connected)
    {
        printf(".");
        sleep(1);
    }
    printf("OK!\n");

    /* =============================================================
     * Step 3 — Zenoh session configuration
     *
     * We start with the default config, then override the session mode
     * ("client" or "peer") based on the CLIENT_OR_PEER compile-time flag.
     *
     * Client mode: connect to a known Zenoh router endpoint.
     * Peer mode:   listen on a UDP multicast address for peer discovery.
     *
     * If LOCATOR is empty (client mode with scouting), Zenoh will
     * discover a router via UDP multicast — no hardcoded address needed.
     * ============================================================= */
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

    /* =============================================================
     * Step 4 — Open Zenoh session
     *
     * z_open() takes the config and returns an owned session handle.
     * Under the hood it sets up a transport (TCP or UDP), negotiates
     * capabilities with the router/peer, and establishes the Zenoh
     * protocol session.
     *
     * If this fails (router unreachable, network down, etc.) we print
     * an error and abort — the ESP32 resets and retries.
     * ============================================================= */
    printf("Opening Zenoh Session...");
    z_owned_session_t s;
    if (z_open(&s, z_move(config), NULL) < 0)
    {
        printf("Unable to open session!\n");
        exit(-1);
    }
    printf("OK\n");

    // =============================================================
    // Step 5 — Declare subscriber
    //
    // A "subscriber" is Zenoh's term for a persistent subscription.
    // We provide:
    //   - the session handle  (z_loan(s))
    //   - the key expression  ("demo/example/**")
    //   - a closure (callback + optional context + drop function)
    //
    // The "**" wildcard matches any number of path segments, so
    // publishing on "demo/example/foo", "demo/example/bar/baz", etc.
    // will all trigger our callback.
    //
    // The closure is "moved" (z_move(callback)) — after this call the
    // subscriber owns it and we must not use it again.
    // =============================================================
    printf("Declaring Subscriber on '%s'...", KEYEXPR);
    z_owned_closure_sample_t callback;
    z_closure(&callback, data_handler, NULL, NULL);
    z_owned_subscriber_t sub;
    z_view_keyexpr_t     ke;
    z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);
    if (z_declare_subscriber(z_loan(s), &sub, z_loan(ke), z_move(callback),
                             NULL) < 0)
    {
        printf("Unable to declare subscriber.\n");
        exit(-1);
    }
    printf("OK!\n");

    /* =============================================================
     * Step 6 — Main loop (idle)
     *
     * The subscriber runs in the background — Zenoh's transport layer
     * receives incoming publications and invokes our data_handler
     * callback from its own context.
     *
     * We just sleep forever.  The FreeRTOS idle task handles low-power
     * housekeeping; the WiFi and Zenoh stacks handle network I/O.
     * ============================================================= */
    while (1)
    {
        sleep(1);
    }

    /* =============================================================
     * Step 7 — Cleanup (unreachable under current infinite loop)
     *
     * If we ever break out of the loop, we must clean up Zenoh resources
     * in reverse order of creation:
     *   1. Undeclare the subscriber (stops receiving).
     *   2. Close the session (tears down the transport).
     *
     * This is shown for completeness — in a real production subscriber
     * you might react to a shutdown signal instead of looping forever.
     * ============================================================= */
    printf("Closing Zenoh Session...");
    z_drop(z_move(sub));

    z_drop(z_move(s));
    printf("OK!\n");
}
/*
 * Compile-time fallback
 * ----------------------
 * zenoh-pico can be compiled without subscription support (to save code
 * space on memory-constrained targets).  If Z_FEATURE_SUBSCRIPTION is 0,
 * the library doesn't include subscriber primitives and this example
 * would fail at link time — so we guard the entire application body
 * behind the #if at line 36.
 *
 * When SUBSCRIPTION is disabled, app_main() simply prints an error
 * message and returns, letting FreeRTOS idle.
 */
#else
void app_main()
{
    printf("ERROR: Zenoh pico was compiled without Z_FEATURE_SUBSCRIPTION but "
           "this example requires it.\n");
}
#endif /* Z_FEATURE_SUBSCRIPTION */
