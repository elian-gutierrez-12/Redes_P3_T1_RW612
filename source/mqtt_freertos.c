/*
 * mqtt_freertos.c
 * Integrated MQTT functionality for RW612
 * Publishes oxygen level and fill state (INCREASE/DECREASE/STABLE); subscribes to requests and alarms.
 * Uses lwIP sys_timeout for timing.
 *
 * SPDX-License-Identifier: BSD-3-Clause
 */

/*******************************************************************************
 * Includes
 ******************************************************************************/
#include "mqtt_freertos.h"
#include "board.h"
#include "fsl_silicon_id.h"
#include "lwip/opt.h"
#include "lwip/api.h"
#include "lwip/apps/mqtt.h"
#include "lwip/tcpip.h"
#include "lwip/sys.h"
#include <string.h>
#include <stdio.h>
#include <stdbool.h>

/*******************************************************************************
 * Definitions
 ******************************************************************************/
#ifndef EXAMPLE_MQTT_SERVER_HOST
#define EXAMPLE_MQTT_SERVER_HOST "broker.hivemq.com"
#endif
#ifndef EXAMPLE_MQTT_SERVER_PORT
#define EXAMPLE_MQTT_SERVER_PORT 1883
#endif
#define APP_THREAD_STACKSIZE 1024
#define APP_THREAD_PRIO      DEFAULT_THREAD_PRIO
#define STEP_DELAY_MS         10    /* ms between each level step */
#define OFF_DELAY_MS        5000    /* ms before starting increase */

/*******************************************************************************
 * Prototypes
 ******************************************************************************/
static void connect_to_mqtt(void *ctx);
static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status);
static void mqtt_topic_subscribed_cb(void *arg, err_t err);
static void mqtt_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len);
static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags);
static void mqtt_message_published_cb(void *arg, err_t err);
static void publish_availability(void *ctx);
static void generate_client_id(void);
static void app_thread(void *arg);
static void oxygen_decrease_step(void *ctx);
static void oxygen_increase_step(void *ctx);
static void publish_change(mqtt_client_t *client);

/*******************************************************************************
 * Variables
 ******************************************************************************/
static mqtt_client_t *mqtt_client;
static char client_id[(SILICONID_MAX_LENGTH * 2) + 5];

/* oxygen level and tracking */
static int oxygen_level      = 100;
static int prev_oxygen_level = 100;

/* tank state */
static bool alarm_active = false;
static bool fill_request = false;
typedef enum { SUB_NONE=0, SUB_REQUEST, SUB_ALARM } sub_t;
static sub_t current_sub = SUB_NONE;

static const struct mqtt_connect_client_info_t mqtt_client_info = {
    .client_id   = (const char *)&client_id[0],
    .client_user = NULL,
    .client_pass = NULL,
    .keep_alive  = 100,
    .will_topic  = "tank/availability",
    .will_msg    = "OFFLINE",
    .will_qos    = 1,
    .will_retain = 1,
#if LWIP_ALTCP && LWIP_ALTCP_TLS
    .tls_config  = NULL,
#endif
};
static ip_addr_t mqtt_addr;

/*******************************************************************************
 * Code
 ******************************************************************************/

/* Publish ONLINE and initial STABLE on connect */
static void publish_availability(void *ctx)
{
    mqtt_client_t *client = (mqtt_client_t*)ctx;
    PRINTF("DBG: tank/availability=ONLINE\r\n");
    mqtt_publish(client,
                 "tank/availability",
                 "ONLINE", strlen("ONLINE"),
                 1, 1,
                 mqtt_message_published_cb,
                 (void*)"tank/availability");
    /* Immediately publish initial STABLE state */
    publish_change(client);
}

/* Publish level and state (INCREASE/DECREASE/STABLE) if changed or first invocation */
static void publish_change(mqtt_client_t *client)
{
    /* level */
    if (oxygen_level != prev_oxygen_level) {
        char pl[4];
        int l = snprintf(pl, sizeof(pl), "%d", oxygen_level);
        PRINTF("DBG: tank/oxygen_level=%d%%\r\n", oxygen_level);
        mqtt_publish(client,
                     "tank/oxygen_level",
                     pl, l,
                     1, 1,
                     mqtt_message_published_cb,
                     (void*)"tank/oxygen_level");
    }

    /* state */
    const char *state;
    if      (oxygen_level < prev_oxygen_level) state = "DECREASE";
    else if (oxygen_level > prev_oxygen_level) state = "INCREASE";
    else                                       state = "STABLE";

    static const char *prev_state = NULL;
    if (!prev_state || strcmp(state, prev_state) != 0) {
        PRINTF("DBG: tank/fill_state=%s\r\n", state);
        mqtt_publish(client,
                     "tank/fill_state",
                     state, strlen(state),
                     1, 1,
                     mqtt_message_published_cb,
                     (void*)"tank/fill_state");
        prev_state = state;
    }

    prev_oxygen_level = oxygen_level;
}

/* Fast decrease until 1% */
static void oxygen_decrease_step(void *ctx)
{
    mqtt_client_t *client = (mqtt_client_t*)ctx;
    if (oxygen_level > 1)
    {
        oxygen_level--;
        publish_change(client);
        sys_timeout(STEP_DELAY_MS, oxygen_decrease_step, client);
    }
    else
    {
        /* reached 1% */
        publish_change(client);
    }
}

/* Fast increase until 100% */
static void oxygen_increase_step(void *ctx)
{
    mqtt_client_t *client = (mqtt_client_t*)ctx;
    if (oxygen_level < 100)
    {
        oxygen_level++;
        publish_change(client);
        sys_timeout(STEP_DELAY_MS, oxygen_increase_step, client);
    }
    else
    {
        /* reached 100% */
        publish_change(client);
    }
}

/* Incoming topic notification */
static void mqtt_incoming_publish_cb(void *arg, const char *topic, u32_t tot_len)
{
    LWIP_UNUSED_ARG(arg);
    LWIP_UNUSED_ARG(tot_len);
    PRINTF("DBG: Incoming publish for '%s'\r\n", topic);
    if      (strcmp(topic, "tank/oxygen_request") == 0) current_sub = SUB_REQUEST;
    else if (strcmp(topic, "tank/alarm")          == 0) current_sub = SUB_ALARM;
    else                                               current_sub = SUB_NONE;
}

/* Handle full payload */
static void mqtt_incoming_data_cb(void *arg, const u8_t *data, u16_t len, u8_t flags)
{
    mqtt_client_t *client = (mqtt_client_t*)arg;
    if (!(flags & MQTT_DATA_FLAG_LAST)) return;

    char buf[16] = {0};
    memcpy(buf, data, len>15?15:len);
    const char *topic = (current_sub==SUB_REQUEST) ? "tank/oxygen_request" : "tank/alarm";
    PRINTF("DBG: Received on '%s': '%s'\r\n", topic, buf);

    if (current_sub == SUB_REQUEST)
    {
        bool req_on = (buf[0]=='O' && buf[1]=='N');
        fill_request = req_on;
        if (fill_request)
        {
            /* ON: cancel increase, start fast decrease */
            sys_untimeout(oxygen_increase_step, client);
            sys_untimeout(oxygen_decrease_step, client);
            oxygen_decrease_step(client);
        }
        else
        {
            /* OFF: publish STABLE, then after delay start increase */
            PRINTF("DBG: tank/fill_state=STABLE\r\n");
            mqtt_publish(client,
                         "tank/fill_state",
                         "STABLE", strlen("STABLE"),
                         1, 1,
                         mqtt_message_published_cb,
                         (void*)"tank/fill_state");
            sys_untimeout(oxygen_decrease_step, client);
            sys_timeout(OFF_DELAY_MS, oxygen_increase_step, client);
        }
    }
    else if (current_sub == SUB_ALARM)
    {
        bool alarm_on = (buf[0]=='O' && buf[1]=='N');
        alarm_active = alarm_on;
        if (alarm_active)
        {
            sys_untimeout(oxygen_decrease_step, client);
            sys_untimeout(oxygen_increase_step, client);
            PRINTF("DBG: tank/fill_state=STABLE\r\n");
            mqtt_publish(client,
                         "tank/fill_state",
                         "STABLE", strlen("STABLE"),
                         1, 1,
                         mqtt_message_published_cb,
                         (void*)"tank/fill_state");
        }
        else if (fill_request)
        {
            sys_untimeout(oxygen_decrease_step, client);
            oxygen_decrease_step(client);
        }
    }
    current_sub = SUB_NONE;
}

/* Subscription callback */
static void mqtt_topic_subscribed_cb(void *arg, err_t err)
{
    const char *topic = (const char*)arg;
    PRINTF(err==ERR_OK
           ? "Subscribed to '%s'\r\n"
           : "Subscribe failed '%s': %d\r\n",
           topic, err);
}

/* Subscribe topics */
static void mqtt_subscribe_topics(mqtt_client_t *client)
{
    mqtt_set_inpub_callback(client,
                            mqtt_incoming_publish_cb,
                            mqtt_incoming_data_cb,
                            client);
    const char *topics[] = { "tank/oxygen_request", "tank/alarm" };
    int qos[] = { 1, 1 };
    for (int i = 0; i < 2; i++) {
        mqtt_subscribe(client,
                       topics[i], qos[i],
                       mqtt_topic_subscribed_cb,
                       (void*)topics[i]);
    }
}

/* Connection callback */
static void mqtt_connection_cb(mqtt_client_t *client, void *arg, mqtt_connection_status_t status)
{
    const struct mqtt_connect_client_info_t *ci = arg;
    if (status == MQTT_CONNECT_ACCEPTED)
    {
        PRINTF("MQTT '%s' connected\r\n", ci->client_id);
        mqtt_subscribe_topics(client);
        tcpip_callback(publish_availability, client);
    }
    else if (status == MQTT_CONNECT_DISCONNECTED)
    {
        PRINTF("MQTT disconnected\r\n");
        sys_timeout(1000, connect_to_mqtt, NULL);
    }
    else
    {
        sys_timeout(10000, connect_to_mqtt, NULL);
    }
}

/* Initiate connection */
static void connect_to_mqtt(void *ctx)
{
    LWIP_UNUSED_ARG(ctx);
    PRINTF("Connecting to %s...\r\n", ipaddr_ntoa(&mqtt_addr));
    mqtt_client_connect(mqtt_client,
                        &mqtt_addr,
                        EXAMPLE_MQTT_SERVER_PORT,
                        mqtt_connection_cb,
                        (void*)&mqtt_client_info,
                        &mqtt_client_info);
}

/* Publish callback */
static void mqtt_message_published_cb(void *arg, err_t err)
{
    const char *topic = (const char*)arg;
    PRINTF(err==ERR_OK
           ? "Publicado '%s'\r\n"
           : "Error publicando '%s': %d\r\n",
           topic, err);
}

/* Generate client ID */
static void generate_client_id(void)
{
    uint8_t silicon_id[SILICONID_MAX_LENGTH];
    uint32_t id_len = sizeof(silicon_id);
    assert(SILICONID_GetID(silicon_id, &id_len) == kStatus_Success);
    const char *hex = "0123456789abcdef";
    int idx = 0;
    client_id[idx++] = 'n'; client_id[idx++] = 'x';
    client_id[idx++] = 'p'; client_id[idx++] = '_';
    for (int i = id_len - 1; i >= 0; i--)
    {
        uint8_t v = silicon_id[i];
        client_id[idx++] = hex[v >> 4];
        client_id[idx++] = hex[v & 0xF];
    }
    client_id[idx] = '\0';
}

/* App thread */
static void app_thread(void *arg)
{
    struct netif *netif = (struct netif*)arg;
    PRINTF("IP=%s mask=%s gw=%s\r\n",
           ipaddr_ntoa(&netif->ip_addr),
           ipaddr_ntoa(&netif->netmask),
           ipaddr_ntoa(&netif->gw));

    err_t err;
    if (ipaddr_aton(EXAMPLE_MQTT_SERVER_HOST, &mqtt_addr) && IP_IS_V4(&mqtt_addr))
        err = ERR_OK;
    else
        err = netconn_gethostbyname(EXAMPLE_MQTT_SERVER_HOST, &mqtt_addr);

    if (err == ERR_OK)
        tcpip_callback(connect_to_mqtt, NULL);
    else
        PRINTF("Resolve error: %d\r\n", err);

    vTaskDelete(NULL);
}

/* Entry point */
void mqtt_freertos_run_thread(struct netif *netif)
{
    LOCK_TCPIP_CORE();
    mqtt_client = mqtt_client_new();
    UNLOCK_TCPIP_CORE();
    if (!mqtt_client) {
        PRINTF("mqtt_client_new failed\r\n");
        while (1) { /* fatal */ }
    }
    generate_client_id();
    sys_thread_new("app_task",
                   app_thread,
                   netif,
                   APP_THREAD_STACKSIZE,
                   APP_THREAD_PRIO);
}
