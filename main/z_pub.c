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

#include <esp_event.h>      /* ESP event loop */
#include <esp_log.h>        /* ESP logging */
#include <esp_system.h>     /* ESP system calls */
#include <esp_wifi.h>       /* WiFi STA/AP */

#include <stdio.h>          /* printf */
#include <stdlib.h>         /* exit */
#include <string.h>         /* strcmp */

#include <nvs_flash.h>      /* Non-volatile storage */
#include <unistd.h>         /* sleep */
#include <zenoh-pico.h>     /* Zenoh pub/sub */

#include <freertos/FreeRTOS.h>        /* FreeRTOS */
#include <freertos/event_groups.h>    /* Event groups for WiFi sync */
#include <freertos/task.h>            /* Tasks */

#if Z_FEATURE_PUBLICATION == 1
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

#define KEYEXPR "demo/example/zenoh-pico-pub"    /* Zenoh key expression (topic) */
#define VALUE  "[ESPIDF]{ESP32} Publication from Zenoh-Pico!"  /* Message template */

/* WiFi event handler: start connection, retry on disconnect, signal on IP assigned */
static void event_handler(void *arg, esp_event_base_t event_base,
                          int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START)
    {
        /* WiFi stack ready — begin connection */
        esp_wifi_connect();
    }
    else if (event_base == WIFI_EVENT &&
             event_id == WIFI_EVENT_STA_DISCONNECTED)
    {
        /* Disconnected — retry up to ESP_MAXIMUM_RETRY times */
        if (s_retry_count < ESP_MAXIMUM_RETRY)
        {
            esp_wifi_connect();
            s_retry_count++;
        }
    }
    else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP)
    {
        /* DHCP lease obtained — wake up app_main */
        xEventGroupSetBits(s_event_group_handler, WIFI_CONNECTED_BIT);
        s_retry_count = 0;
    }
}

/* Initialise WiFi in station (STA) mode, block until connected */
void wifi_init_sta(void)
{
    s_event_group_handler = xEventGroupCreate();

    /* --- ESP-IDF network stack initialisation --- */
    ESP_ERROR_CHECK(esp_netif_init());

    ESP_ERROR_CHECK(esp_event_loop_create_default());
    esp_netif_create_default_wifi_sta();

    /* --- WiFi driver init --- */
    wifi_init_config_t config = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&config));

    /* Register event handlers before starting WiFi */
    esp_event_handler_instance_t handler_any_id;
    esp_event_handler_instance_t handler_got_ip;
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        WIFI_EVENT, ESP_EVENT_ANY_ID, &event_handler, NULL, &handler_any_id));
    ESP_ERROR_CHECK(esp_event_handler_instance_register(
        IP_EVENT, IP_EVENT_STA_GOT_IP, &event_handler, NULL, &handler_got_ip));

    /* Configure SSID / password from compile-time defines */
    wifi_config_t wifi_config = {.sta = {
                                     .ssid     = ESP_WIFI_SSID,
                                     .password = ESP_WIFI_PASS,
                                 }};

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    /* Block until GOT_IP event is received (via the event group) */
    EventBits_t bits =
        xEventGroupWaitBits(s_event_group_handler, WIFI_CONNECTED_BIT, pdFALSE,
                            pdFALSE, portMAX_DELAY);

    if (bits & WIFI_CONNECTED_BIT)
    {
        s_is_wifi_connected = true;
    }

    /* Clean up event handlers and event group */
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        IP_EVENT, IP_EVENT_STA_GOT_IP, handler_got_ip));
    ESP_ERROR_CHECK(esp_event_handler_instance_unregister(
        WIFI_EVENT, ESP_EVENT_ANY_ID, handler_any_id));
    vEventGroupDelete(s_event_group_handler);
}

/* Entry point: init NVS → WiFi → Zenoh session → publish forever */
void app_main()
{
    /* --- Initialise NVS (non-volatile storage) ---
       Needed by ESP-IDF's WiFi stack internally */
    esp_err_t ret = nvs_flash_init();
    if (ret == ESP_ERR_NVS_NO_FREE_PAGES ||
        ret == ESP_ERR_NVS_NEW_VERSION_FOUND)
    {
        ESP_ERROR_CHECK(nvs_flash_erase());
        ret = nvs_flash_init();
    }
    ESP_ERROR_CHECK(ret);

    /* --- Connect to WiFi (blocking) --- */
    printf("Connecting to WiFi...");
    wifi_init_sta();
    while (!s_is_wifi_connected)
    {
        printf(".");
        sleep(1);
    }
    printf("OK!\n");

    /* --- Build Zenoh config from compile-time defines --- */
    z_owned_config_t config;
    z_config_default(&config);
    zp_config_insert(z_loan_mut(config), Z_CONFIG_MODE_KEY, MODE);
    if (strcmp(LOCATOR, "") != 0)
    {
        /* Non-empty locator: use it as connect (client) or listen (peer) endpoint */
        if (strcmp(MODE, "client") == 0)
        {
            zp_config_insert(z_loan_mut(config), Z_CONFIG_CONNECT_KEY, LOCATOR);
        }
        else
        {
            zp_config_insert(z_loan_mut(config), Z_CONFIG_LISTEN_KEY, LOCATOR);
        }
    }

    /* --- Open Zenoh session --- */
    printf("Opening Zenoh Session...");
    z_owned_session_t s;
    if (z_open(&s, z_move(config), NULL) < 0)
    {
        printf("Unable to open session!\n");
        exit(-1);
    }
    printf("OK\n");

    /* --- Declare a publisher on the configured key expression --- */
    printf("Declaring publisher for '%s'...", KEYEXPR);
    z_owned_publisher_t pub;
    z_view_keyexpr_t    ke;
    z_view_keyexpr_from_str_unchecked(&ke, KEYEXPR);
    if (z_declare_publisher(z_loan(s), &pub, z_loan(ke), NULL) < 0)
    {
        printf("Unable to declare publisher for key expression!\n");
        exit(-1);
    }
    printf("OK\n");

    /* --- Publish an incrementing counter every second, forever --- */
    char buf[256];
    for (int idx = 0; 1; ++idx)
    {
        sleep(1);
        sprintf(buf, "[%4d] %s", idx, VALUE);
        printf("Putting Data ('%s': '%s')...\n", KEYEXPR, buf);

        /* Build payload and publish */
        z_owned_bytes_t payload;
        z_bytes_copy_from_str(&payload, buf);

        z_publisher_put(z_loan(pub), z_move(payload), NULL);
    }

    /* Unreachable — kept for completeness */
    printf("Closing Zenoh Session...");
    z_drop(z_move(pub));
    z_drop(z_move(s));
    printf("OK!\n");
}
#else
/* Fallback: zenoh-pico was built without publication feature */
void app_main()
{
    printf("ERROR: Zenoh pico was compiled without Z_FEATURE_PUBLICATION but "
           "this example requires it.\n");
}
#endif
