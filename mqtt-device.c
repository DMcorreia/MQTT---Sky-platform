/*
/**
 * \file
 *         Light and temperatur sensor web demo
 * \author
 *         Niclas Finne    <nfi@sics.se>
 *         Joakim Eriksson <joakime@sics.se>
 *         Joel Hoglund    <joel@sics.se>
 */

#include "contiki-conf.h"
#include "contiki.h"
#include "net/ipv6/sicslowpan.h"
#include "dev/sht11/sht11-sensor.h"
#include "dev/light-sensor.h"
#include "dev/leds.h"
#include <stdio.h>

/*
 * MQTT includes
 */
#include "mqtt.h"
#include "button-sensor.h"
#include "sys/etimer.h"
#include "sys/ctimer.h"
#include <string.h>

/*
 * Init
 */
PROCESS(web_sense_process, "Sense Web Demo");
PROCESS_NAME(mqtt_demo_process);
PROCESS(mqtt_demo_process, "MQTT Demo");

AUTOSTART_PROCESSES(&web_sense_process,&mqtt_demo_process);

#define HISTORY 16
static int temperature[HISTORY];
static int light1[HISTORY];
static int sensors_pos;

/*---------------------------------------------------------------------------*/
static int
get_light(void)
{
  return 10 * light_sensor.value(LIGHT_SENSOR_PHOTOSYNTHETIC) / 7;
}
/*---------------------------------------------------------------------------*/
static int
get_temp(void)
{
  return ((sht11_sensor.value(SHT11_SENSOR_TEMP) / 10) - 396) / 10;
}

static int get_temperature(void) {
    return (light_sensor.value(LIGHT_SENSOR_PHOTOSYNTHETIC) + 6420) / 300;
}

/*---------------------------------------------------------------------------*/
PROCESS_THREAD(web_sense_process, ev, data)
{
  static struct etimer timer;
  PROCESS_BEGIN();

  sensors_pos = 0;

  etimer_set(&timer, CLOCK_SECOND * 2);
  SENSORS_ACTIVATE(light_sensor);
  SENSORS_ACTIVATE(sht11_sensor);

  while(1) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&timer));
    etimer_reset(&timer);

    light1[sensors_pos] = get_light();;
    temperature[sensors_pos] = get_temp();
    sensors_pos = (sensors_pos + 1) % HISTORY;
  }

  PROCESS_END();
}

/*---------------------------------------------------------------------------*/
/*
 * IBM server: quickstart.messaging.internetofthings.ibmcloud.com
 * (184.172.124.189) mapped in an NAT64 (prefix 64:ff9b::/96) IPv6 address
 * Note: If not able to connect; lookup the IP address again as it may change.
 *    
 * Alternatively, publish to a local MQTT broker (e.g. mosquitto) running on
 * the node that hosts your border router
 */
#ifdef MQTT_DEMO_BROKER_IP_ADDR
static const char *broker_ip = MQTT_DEMO_BROKER_IP_ADDR;
#define DEFAULT_ORG_ID              "mqtt-demo"
#else
static const char *broker_ip = "k29znw.messaging.internetofthings.ibmcloud.com";
#define DEFAULT_ORG_ID              "k29znw"
#endif
/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
/*
 * A timeout used when waiting for something to happen (e.g. to connect or to
 * disconnect)
 */
#define STATE_MACHINE_PERIODIC     (CLOCK_SECOND >> 1)
/*---------------------------------------------------------------------------*/
/* Provide visible feedback via LEDS during various states */
/* When connecting to broker */
#define CONNECTING_LED_DURATION    (CLOCK_SECOND >> 2)

/* Each time we try to publish */
#define PUBLISH_LED_ON_DURATION    (CLOCK_SECOND)
/*---------------------------------------------------------------------------*/
/* Connections and reconnections */
#define RETRY_FOREVER              0xFF
#define RECONNECT_INTERVAL         (CLOCK_SECOND * 2)

/*
 * Number of times to try reconnecting to the broker.
 * Can be a limited number (e.g. 3, 10 etc) or can be set to RETRY_FOREVER
 */
#define RECONNECT_ATTEMPTS         RETRY_FOREVER
#define CONNECTION_STABLE_TIME     (CLOCK_SECOND * 5)
static struct timer connection_life;
static uint8_t connect_attempt;
/*---------------------------------------------------------------------------*/
/* Various states */
static uint8_t state;
#define STATE_INIT            0
#define STATE_REGISTERED      1
#define STATE_CONNECTING      2
#define STATE_CONNECTED       3
#define STATE_PUBLISHING      4
#define STATE_DISCONNECTED    5
#define STATE_NEWCONFIG       6
#define STATE_CONFIG_ERROR 0xFE
#define STATE_ERROR        0xFF
/*---------------------------------------------------------------------------*/
#define CONFIG_ORG_ID_LEN        32
#define CONFIG_TYPE_ID_LEN       32
#define CONFIG_AUTH_TOKEN_LEN    32
#define CONFIG_EVENT_TYPE_ID_LEN 32
#define CONFIG_CMD_TYPE_LEN       8
#define CONFIG_IP_ADDR_STR_LEN   64
/*---------------------------------------------------------------------------*/
#define RSSI_MEASURE_INTERVAL_MAX 86400 /* secs: 1 day */
#define RSSI_MEASURE_INTERVAL_MIN     5 /* secs */
#define PUBLISH_INTERVAL_MAX      86400 /* secs: 1 day */
#define PUBLISH_INTERVAL_MIN          5 /* secs */
/*---------------------------------------------------------------------------*/
/* A timeout used when waiting to connect to a network */
#define NET_CONNECT_PERIODIC        CLOCK_SECOND
#define NO_NET_LED_DURATION         (NET_CONNECT_PERIODIC >> 1)
/*---------------------------------------------------------------------------*/
/* Default configuration values */
#define DEFAULT_TYPE_ID             "GateWayType"
#define DEFAULT_AUTH_TOKEN          "123456789"
#define DEFAULT_EVENT_TYPE_ID       "status"
#define DEFAULT_SUBSCRIBE_CMD_TYPE  "+"
#define DEFAULT_BROKER_PORT         1883
#define DEFAULT_PUBLISH_INTERVAL    (30 * CLOCK_SECOND)
#define DEFAULT_KEEP_ALIVE_TIMER    60
#define DEFAULT_RSSI_MEAS_INTERVAL  (CLOCK_SECOND * 30)
/*---------------------------------------------------------------------------*/
/* Take a sensor reading on button press */
#define PUBLISH_TRIGGER (&button_sensor)

/* Payload length of ICMPv6 echo requests used to measure RSSI with def rt */
#define ECHO_REQ_PAYLOAD_LEN   20
/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
/**
 * \brief Data structure declaration for the MQTT client configuration
 */
typedef struct mqtt_client_config {
  char org_id[CONFIG_ORG_ID_LEN];
  char type_id[CONFIG_TYPE_ID_LEN];
  char auth_token[CONFIG_AUTH_TOKEN_LEN];
  char event_type_id[CONFIG_EVENT_TYPE_ID_LEN];
  char broker_ip[CONFIG_IP_ADDR_STR_LEN];
  char cmd_type[CONFIG_CMD_TYPE_LEN];
  clock_time_t pub_interval;
  int def_rt_ping_interval;
  uint16_t broker_port;
} mqtt_client_config_t;
/*---------------------------------------------------------------------------*/
/* Maximum TCP segment size for outgoing segments of our socket */
#define MAX_TCP_SEGMENT_SIZE    32
/*---------------------------------------------------------------------------*/
#define STATUS_LED LEDS_GREEN
/*---------------------------------------------------------------------------*/
#define BUFFER_SIZE 64
static char client_id[BUFFER_SIZE];
static char pub_topic[BUFFER_SIZE];
static char sub_topic[BUFFER_SIZE];
/*---------------------------------------------------------------------------*/
/*
 * The main MQTT buffers.
 * We will need to increase if we start publishing more data.
 */
#define APP_BUFFER_SIZE 128
static struct mqtt_connection conn;
static char app_buffer[APP_BUFFER_SIZE];
/*---------------------------------------------------------------------------*/
#define QUICKSTART "quickstart"
/*---------------------------------------------------------------------------*/
static struct mqtt_message *msg_ptr = 0;
static struct etimer publish_periodic_timer;
static struct ctimer ct;
static char *buf_ptr;
static uint16_t seq_nr_value = 0;
/*---------------------------------------------------------------------------*/
/* Parent RSSI functionality */
static struct uip_icmp6_echo_reply_notification echo_reply_notification;
static struct etimer echo_request_timer;
static int def_rt_rssi = 0;
/*---------------------------------------------------------------------------*/
static mqtt_client_config_t conf;
/*---------------------------------------------------------------------------*/

/*---------------------------------------------------------------------------*/
int
ipaddr_sprintf(char *buf, uint8_t buf_len, const uip_ipaddr_t *addr)
{
  uint16_t a;
  uint8_t len = 0;
  int i, f;
  for(i = 0, f = 0; i < sizeof(uip_ipaddr_t); i += 2) {
    a = (addr->u8[i] << 8) + addr->u8[i + 1];
    if(a == 0 && f >= 0) {
      if(f++ == 0) {
        len += snprintf(&buf[len], buf_len - len, "::");
      }
    } else {
      if(f > 0) {
        f = -1;
      } else if(i > 0) {
        len += snprintf(&buf[len], buf_len - len, ":");
      }
      len += snprintf(&buf[len], buf_len - len, "%x", a);
    }
  }

  return len;
}
/*---------------------------------------------------------------------------*/
static void
echo_reply_handler(uip_ipaddr_t *source, uint8_t ttl, uint8_t *data,
                   uint16_t datalen)
{
  if(uip_ip6addr_cmp(source, uip_ds6_defrt_choose())) {
    //printf("sicslowpan_get_last_rssi()\n");
    def_rt_rssi = sicslowpan_get_last_rssi();
  }
}
/*---------------------------------------------------------------------------*/
static void
publish_led_off(void *d)
{
  leds_off(STATUS_LED);
}
/*---------------------------------------------------------------------------*/
static void
pub_handler(const char *topic, uint16_t topic_len, const uint8_t *chunk,
            uint16_t chunk_len)
{
  DBG("Pub Handler: topic='%s' (len=%u), chunk_len=%u\n", topic, topic_len,
      chunk_len);

    printf("Received command\n");                   
    if(chunk[0] == '1') {
      leds_on(LEDS_RED);
    } else if(chunk[0] == '0') {
      leds_off(LEDS_RED);
    }
    return;

  /* If we don't like the length, ignore */
  if(topic_len != 23 || chunk_len != 1) {
    printf("Incorrect topic or chunk len. Ignored\n");
    return;
  }

  /* If the format != json, ignore */
  if(strncmp(&topic[topic_len - 4], "json", 4) != 0) {
    printf("Incorrect format\n");
  }
 
  if(strncmp(&topic[10], "leds", 4) == 0) {
    if(chunk[0] == '1') {
      leds_on(LEDS_RED);
    } else if(chunk[0] == '0') {
      leds_off(LEDS_RED);
    }
    return;
  }
}
/*---------------------------------------------------------------------------*/
static void
mqtt_event(struct mqtt_connection *m, mqtt_event_t event, void *data)
{
  switch(event) {
  case MQTT_EVENT_CONNECTED: {
    DBG("APP - Application has a MQTT connection\n");
    timer_set(&connection_life, CONNECTION_STABLE_TIME);
    state = STATE_CONNECTED;
    break;
  }
  case MQTT_EVENT_DISCONNECTED: {
    DBG("APP - MQTT Disconnect. Reason %u\n", *((mqtt_event_t *)data));

    state = STATE_DISCONNECTED;
    process_poll(&mqtt_demo_process);
    break;
  }
  case MQTT_EVENT_PUBLISH: {
    msg_ptr = data;

    /* Implement first_flag in publish message? */
    if(msg_ptr->first_chunk) {
      msg_ptr->first_chunk = 0;
      DBG("APP - Application received a publish on topic '%s'. Payload "
          "size is %i bytes. Content:\n\n",
          msg_ptr->topic, msg_ptr->payload_length);
    }

    pub_handler(msg_ptr->topic, strlen(msg_ptr->topic), msg_ptr->payload_chunk,
                msg_ptr->payload_length);
    break;
  }
  case MQTT_EVENT_SUBACK: {
    DBG("APP - Application is subscribed to topic successfully\n");
    break;
  }
  case MQTT_EVENT_UNSUBACK: {
    DBG("APP - Application is unsubscribed to topic successfully\n");
    break;
  }
  case MQTT_EVENT_PUBACK: {
    DBG("APP - Publishing complete\n");
    break;
  }
  default:
    DBG("APP - Application got a unhandled MQTT event: %i\n", event);
    break;
  }
}
/*---------------------------------------------------------------------------*/
static int
construct_pub_topic(void)
{
  //int len = snprintf(pub_topic, BUFFER_SIZE, "iot-2/evt/%s/fmt/json", conf.event_type_id);
  int len = snprintf(pub_topic, BUFFER_SIZE, "sensor/data");

  /* len < 0: Error. Len >= BUFFER_SIZE: Buffer too small */
  if(len < 0 || len >= BUFFER_SIZE) {
    printf("Pub Topic: %d, Buffer %d\n", len, BUFFER_SIZE);
    return 0;
  }

  return 1;
}
/*---------------------------------------------------------------------------*/
static int
construct_sub_topic(void)
{
  //int len = snprintf(sub_topic, BUFFER_SIZE, "iot-2/cmd/%s/fmt/json", conf.cmd_type);
  int len = snprintf(sub_topic, BUFFER_SIZE, "sensor/command");

  /* len < 0: Error. Len >= BUFFER_SIZE: Buffer too small */
  if(len < 0 || len >= BUFFER_SIZE) {
    printf("Sub Topic: %d, Buffer %d\n", len, BUFFER_SIZE);
    return 0;
  }

  return 1;
}
/*---------------------------------------------------------------------------*/
static int
construct_client_id(void)
{

  // ClientID is differentiated by final segment of IPv6 address
  int len = snprintf(client_id, BUFFER_SIZE, "sensor%d", linkaddr_node_addr.u8[7]);

  /* len < 0: Error. Len >= BUFFER_SIZE: Buffer too small */
  if(len < 0 || len >= BUFFER_SIZE) {
    printf("Client ID: %d, Buffer %d\n", len, BUFFER_SIZE);
    return 0;
  }

  return 1;
}
/*---------------------------------------------------------------------------*/
static void
update_config(void)
{
  if(construct_client_id() == 0) {
    /* Fatal error. Client ID larger than the buffer */
    state = STATE_CONFIG_ERROR;
    return;
  }

  if(construct_sub_topic() == 0) {
    /* Fatal error. Topic larger than the buffer */
    state = STATE_CONFIG_ERROR;
    return;
  }

  if(construct_pub_topic() == 0) {
    /* Fatal error. Topic larger than the buffer */
    state = STATE_CONFIG_ERROR;
    return;
  }

  /* Reset the counter */
  seq_nr_value = 0;

  state = STATE_INIT;

  /*
   * Schedule next timer event ASAP
   *
   * If we entered an error state then we won't do anything when it fires.
   *
   * Since the error at this stage is a config error, we will only exit this
   * error state if we get a new config.
   */
  etimer_set(&publish_periodic_timer, 0);

  return;
}
/*---------------------------------------------------------------------------*/
static int
init_config()
{
  /* Populate configuration with default values */
  memset(&conf, 0, sizeof(mqtt_client_config_t));

  memcpy(conf.org_id, DEFAULT_ORG_ID, strlen(DEFAULT_ORG_ID));
  memcpy(conf.type_id, DEFAULT_TYPE_ID, strlen(DEFAULT_TYPE_ID));
  memcpy(conf.auth_token, DEFAULT_AUTH_TOKEN, strlen(DEFAULT_AUTH_TOKEN));
  memcpy(conf.event_type_id, DEFAULT_EVENT_TYPE_ID, strlen(DEFAULT_EVENT_TYPE_ID));
  memcpy(conf.broker_ip, broker_ip, strlen(broker_ip));
  memcpy(conf.cmd_type, DEFAULT_SUBSCRIBE_CMD_TYPE, 1);

  conf.broker_port = DEFAULT_BROKER_PORT;
  conf.pub_interval = DEFAULT_PUBLISH_INTERVAL;
  conf.def_rt_ping_interval = DEFAULT_RSSI_MEAS_INTERVAL;

  return 1;
}
/*---------------------------------------------------------------------------*/
static void
subscribe(void)
{
  /* Publish MQTT topic in IBM quickstart format */
  mqtt_status_t status;

  printf("Subscribing\n");
  status = mqtt_subscribe(&conn, NULL, sub_topic, MQTT_QOS_LEVEL_0);

  DBG("APP - Subscribing!\n");
  if(status == MQTT_STATUS_OUT_QUEUE_FULL) {
    DBG("APP - Tried to subscribe but command queue was full!\n");
  }
}
/*---------------------------------------------------------------------------*/
static void
publish(void)
{
  //printf("Publish()\n");
  /* Publish MQTT topic in IBM quickstart format */
  int len;
  int remaining = APP_BUFFER_SIZE;

  seq_nr_value++;

  buf_ptr = app_buffer;

  len = snprintf(buf_ptr, remaining,
                 "{\"%s\":{\"Sequence\":%d,\"Clock\":%lu,\"Temp\":%d,\"Light\":%d}}",
                 client_id, seq_nr_value, clock_seconds(), get_temperature(), get_light());

  if(len < 0 || len >= remaining) {
    printf("Buffer too short. Have %d, need %d + \\0\n", remaining, len);
    return;
  }

  remaining -= len;
  buf_ptr += len;

  mqtt_publish(&conn, NULL, pub_topic, (uint8_t *)app_buffer, strlen(app_buffer), MQTT_QOS_LEVEL_0, MQTT_RETAIN_OFF);

  DBG("APP - Publish!\n");
}
/*---------------------------------------------------------------------------*/
static void
connect_to_broker(void)
{
  /* Connect to MQTT server */
  mqtt_connect(&conn, conf.broker_ip, conf.broker_port, conf.pub_interval * 3);

  state = STATE_CONNECTING;
}
/*---------------------------------------------------------------------------*/
static void
ping_parent(void)
{
  if(uip_ds6_get_global(ADDR_PREFERRED) == NULL) {
    return;
  }

  uip_icmp6_send(uip_ds6_defrt_choose(), ICMP6_ECHO_REQUEST, 0, ECHO_REQ_PAYLOAD_LEN);
}
/*---------------------------------------------------------------------------*/
static void
state_machine(void)
{
  switch(state) {
  case STATE_INIT:
    printf("INITIALIZING\n");
    /* If we have just been configured register MQTT connection */
    mqtt_register(&conn, &mqtt_demo_process, client_id, mqtt_event, MAX_TCP_SEGMENT_SIZE);

    /*
     * If we are not using the quickstart service (thus we are an IBM
     * registered device), we need to provide user name and password
     */
    if(strncasecmp(conf.org_id, QUICKSTART, strlen(conf.org_id)) != 0) {
      if(strlen(conf.auth_token) == 0) {
        printf("User name set, but empty auth token\n");
        state = STATE_ERROR;
        break;
      } else {
        mqtt_set_username_password(&conn, "use-token-auth", conf.auth_token);
      }
    }

    /* _register() will set auto_reconnect. We don't want that. */
    conn.auto_reconnect = 0;
    connect_attempt = 1;

    state = STATE_REGISTERED;
    DBG("Init\n");
    /* Continue */
  case STATE_REGISTERED:
    printf("REGISTERED\n");
    if(uip_ds6_get_global(ADDR_PREFERRED) != NULL) {
      /* Registered and with a public IP. Connect */
      DBG("Registered. Connect attempt %u\n", connect_attempt);
      ping_parent();
      connect_to_broker();
    } else {
      leds_on(STATUS_LED);
      ctimer_set(&ct, NO_NET_LED_DURATION, publish_led_off, NULL);
    }
    etimer_set(&publish_periodic_timer, NET_CONNECT_PERIODIC);
    return;
    break;
  case STATE_CONNECTING:
    printf("CONNECTING\n");
    leds_on(STATUS_LED);
    ctimer_set(&ct, CONNECTING_LED_DURATION, publish_led_off, NULL);
    /* Not connected yet. Wait */
    DBG("Connecting (%u)\n", connect_attempt);
    break;
  case STATE_CONNECTED:
    printf("CONNECTED\n");
    /* Don't subscribe unless we are a registered device */
    if(strncasecmp(conf.org_id, QUICKSTART, strlen(conf.org_id)) == 0) {
      DBG("Using 'quickstart': Skipping subscribe\n");
      state = STATE_PUBLISHING;
    }
    /* Continue */
  case STATE_PUBLISHING:
    printf("PUBLISHING\n");
    /* If the timer expired, the connection is stable. */
    if(timer_expired(&connection_life)) {
      /*
       * Intentionally using 0 here instead of 1: We want RECONNECT_ATTEMPTS
       * attempts if we disconnect after a successful connect
       */
      connect_attempt = 0;
    }

    if(mqtt_ready(&conn) && conn.out_buffer_sent) {
      /* Connected. Publish */
      if(state == STATE_CONNECTED) {
        subscribe();
        state = STATE_PUBLISHING;
      } else {
        leds_on(STATUS_LED);
        ctimer_set(&ct, PUBLISH_LED_ON_DURATION, publish_led_off, NULL);
        publish();
      }
      etimer_set(&publish_periodic_timer, conf.pub_interval);

      DBG("Publishing\n");
      /* Return here so we don't end up rescheduling the timer */
      return;
    } else {
      /*
       * Our publish timer fired, but some MQTT packet is already in flight
       * (either not sent at all, or sent but not fully ACKd).
       *
       * This can mean that we have lost connectivity to our broker or that
       * simply there is some network delay. In both cases, we refuse to
       * trigger a new message and we wait for TCP to either ACK the entire
       * packet after retries, or to timeout and notify us.
       */
      printf("WAITING for network\n");
      DBG("Publishing... (MQTT state=%d, q=%u)\n", conn.state,
          conn.out_queue_full);
    }
    break;
  case STATE_DISCONNECTED:
    printf("DISCONNECTED\n");
    DBG("Disconnected\n");
    if(connect_attempt < RECONNECT_ATTEMPTS ||
       RECONNECT_ATTEMPTS == RETRY_FOREVER) {
      /* Disconnect and backoff */
      clock_time_t interval;
      mqtt_disconnect(&conn);
      connect_attempt++;

      interval = connect_attempt < 3 ? RECONNECT_INTERVAL << connect_attempt :
        RECONNECT_INTERVAL << 3;

      DBG("Disconnected. Attempt %u in %lu ticks\n", connect_attempt, interval);

      etimer_set(&publish_periodic_timer, interval);

      state = STATE_REGISTERED;
      return;
    } else {
      /* Max reconnect attempts reached. Enter error state */
      state = STATE_ERROR;
      DBG("Aborting connection after %u attempts\n", connect_attempt - 1);
    }
    break;
  case STATE_CONFIG_ERROR:
    printf("CONFIG_ERROR\n");
    /* Idle away. The only way out is a new config */
    printf("Bad configuration\n");
    return;
  case STATE_ERROR:
    printf("STATE_ERROR\n");
  default:
    printf("DEFAULT_ERROR\n");
    leds_on(STATUS_LED);
    /*
     * 'default' should never happen.
     *
     * If we enter here it's because of some error. Stop timers. The only thing
     * that can bring us out is a new config event
     */
    printf("Default case: State=0x%02x\n", state);
    return;
  }

  /* If we didn't return so far, reschedule ourselves */
  etimer_set(&publish_periodic_timer, STATE_MACHINE_PERIODIC);
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(mqtt_demo_process, ev, data)
{

  PROCESS_BEGIN();

  printf("MQTT Demo Process\n");

  if(init_config() != 1) {
    PROCESS_EXIT();
  }
  //printf("MQTT Demo Process 2\n");

  update_config();
  //printf("MQTT Demo Process 2\n");

  //def_rt_rssi = 0x8000000;
  uip_icmp6_echo_reply_callback_add(&echo_reply_notification, echo_reply_handler);
  //printf("MQTT Demo Process 3\n");

  etimer_set(&echo_request_timer, conf.def_rt_ping_interval);
  //printf("MQTT Demo Process 4\n");

  PUBLISH_TRIGGER->configure(SENSORS_ACTIVE, 1);
  //printf("MQTT Demo Process 5\n");

  /* Main loop */
  while(1) {

    PROCESS_YIELD();

    if(ev == sensors_event && data == PUBLISH_TRIGGER) {
      if(state == STATE_ERROR) {
        connect_attempt = 1;
        state = STATE_REGISTERED;
      }
    }

    if((ev == PROCESS_EVENT_TIMER && data == &publish_periodic_timer) ||
       ev == PROCESS_EVENT_POLL ) {
       // IGNORE BUTTON || (ev == sensors_event && data == PUBLISH_TRIGGER && PUBLISH_TRIGGER->value(BUTTON_SENSOR_VALUE_STATE) == 0)) {
      state_machine();
    }

    if(ev == PROCESS_EVENT_TIMER && data == &echo_request_timer) {
      ping_parent();
      etimer_set(&echo_request_timer, conf.def_rt_ping_interval);
    }
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/
/**
 * @}
 * @}
 */
