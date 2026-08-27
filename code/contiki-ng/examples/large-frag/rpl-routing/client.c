#include "contiki.h"
#include "net/routing/routing.h"
#include "net/ipv6/simple-udp.h"
#include "sys/log.h"
#include "project-conf.h"

#include <stdint.h>

#define LOG_MODULE "RPL-CLIENT"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

static struct simple_udp_connection udp_conn;
static uint8_t payload[APP_PAYLOAD_SIZE];

PROCESS(client_process, "RPL one-shot UDP client");
AUTOSTART_PROCESSES(&client_process);

static void
fill_payload(uint32_t seq)
{
  uint16_t i;

  for(i = 0; i < APP_PAYLOAD_SIZE; i++) {
    payload[i] = (uint8_t)(i % 256);
  }

  /* Magic marker: MLKEMFRG */
  payload[0] = 'M';
  payload[1] = 'L';
  payload[2] = 'K';
  payload[3] = 'E';
  payload[4] = 'M';
  payload[5] = 'F';
  payload[6] = 'R';
  payload[7] = 'G';

  /* Sequence number */
  payload[8]  = (seq >> 24) & 0xff;
  payload[9]  = (seq >> 16) & 0xff;
  payload[10] = (seq >> 8) & 0xff;
  payload[11] = seq & 0xff;

  /* End marker */
  payload[APP_PAYLOAD_SIZE - 1] = 0xAA;
}

PROCESS_THREAD(client_process, ev, data)
{
  static struct etimer wait_timer;
  static uip_ipaddr_t dest_ipaddr;
  static uint8_t sent = 0;
  static uint32_t seq = 1;

  PROCESS_BEGIN();

  simple_udp_register(&udp_conn,
                      UDP_CLIENT_PORT,
                      NULL,
                      UDP_SERVER_PORT,
                      NULL);

  LOG_INFO("==================================================\n");
  LOG_INFO("CLIENT_BOOT\n");
  LOG_INFO("Mode: RPL unicast one-shot client\n");
  LOG_INFO("payload_size=%u bytes\n", APP_PAYLOAD_SIZE);
  LOG_INFO("Waiting for RPL route to root...\n");
  LOG_INFO("==================================================\n");

  etimer_set(&wait_timer, 2 * CLOCK_SECOND);

  while(!sent) {
    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&wait_timer));

    if(NETSTACK_ROUTING.node_is_reachable() &&
       NETSTACK_ROUTING.get_root_ipaddr(&dest_ipaddr)) {

      fill_payload(seq);

      LOG_INFO("==================================================\n");
      LOG_INFO("APP_TX_BEGIN\n");
      LOG_INFO("seq=%lu\n", (unsigned long)seq);
      LOG_INFO("payload_size=%u bytes\n", APP_PAYLOAD_SIZE);
      LOG_INFO("magic=MLKEMFRG\n");
      LOG_INFO("destination=");
      LOG_INFO_6ADDR(&dest_ipaddr);
      LOG_INFO_("\n");
      LOG_INFO("Meaning: application gives ONE UDP message to the stack.\n");
      LOG_INFO("==================================================\n");

      LOG_INFO("APP_SEND_CALL_BEGIN seq=%lu\n", (unsigned long)seq);

      simple_udp_sendto(&udp_conn,
                        payload,
                        APP_PAYLOAD_SIZE,
                        &dest_ipaddr);

      LOG_INFO("APP_SEND_CALL_END seq=%lu\n", (unsigned long)seq);

      LOG_INFO("==================================================\n");
      LOG_INFO("APP_TX_REQUEST_DONE\n");
      LOG_INFO("seq=%lu\n", (unsigned long)seq);
      LOG_INFO("Client will NOT send another application packet.\n");
      LOG_INFO("Now count 6LoWPAN/CSMA/Radio frames for this single packet.\n");
      LOG_INFO("==================================================\n");

      sent = 1;

    } else {
      LOG_INFO("RPL_NOT_REACHABLE_YET\n");
      etimer_reset(&wait_timer);
    }
  }

  while(1) {
    PROCESS_YIELD();
  }

  PROCESS_END();
}