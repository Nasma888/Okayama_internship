#include "contiki.h"
#include "net/routing/routing.h"
#include "net/ipv6/simple-udp.h"
#include "sys/log.h"

#include <stdint.h>

#define LOG_MODULE "RPL-SERVER"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

static struct simple_udp_connection udp_conn;

PROCESS(server_process, "RPL UDP server");
AUTOSTART_PROCESSES(&server_process);

static uint32_t
read_seq_from_payload(const uint8_t *data, uint16_t datalen)
{
  if(datalen < 12) {
    return 0;
  }

  return ((uint32_t)data[8] << 24) |
         ((uint32_t)data[9] << 16) |
         ((uint32_t)data[10] << 8) |
         ((uint32_t)data[11]);
}

static int
check_magic(const uint8_t *data, uint16_t datalen)
{
  if(datalen < 8) {
    return 0;
  }

  return data[0] == 'M' &&
         data[1] == 'L' &&
         data[2] == 'K' &&
         data[3] == 'E' &&
         data[4] == 'M' &&
         data[5] == 'F' &&
         data[6] == 'R' &&
         data[7] == 'G';
}

static void
udp_rx_callback(struct simple_udp_connection *c,
                const uip_ipaddr_t *sender_addr,
                uint16_t sender_port,
                const uip_ipaddr_t *receiver_addr,
                uint16_t receiver_port,
                const uint8_t *data,
                uint16_t datalen)
{
  uint32_t seq = read_seq_from_payload(data, datalen);
  int magic_ok = check_magic(data, datalen);

  LOG_INFO("==================================================\n");
  LOG_INFO("APP_RX_BEGIN\n");
  LOG_INFO("seq=%lu\n", (unsigned long)seq);
  LOG_INFO("received_payload_size=%u bytes\n", datalen);
  LOG_INFO("magic_check=%s\n", magic_ok ? "OK_MLKEMFRG" : "FAILED");
  LOG_INFO("sender=");
  LOG_INFO_6ADDR(sender_addr);
  LOG_INFO_("\n");
  LOG_INFO("receiver=");
  LOG_INFO_6ADDR(receiver_addr);
  LOG_INFO_("\n");
  LOG_INFO("Meaning: full UDP packet received after 6LoWPAN reassembly.\n");
  LOG_INFO("SERVER DOES NOT SEND ANY APPLICATION REPLY.\n");
  LOG_INFO("==================================================\n");

  LOG_INFO("APP_RX_END seq=%lu size=%u\n",
           (unsigned long)seq,
           datalen);
}

PROCESS_THREAD(server_process, ev, data)
{
  PROCESS_BEGIN();

  NETSTACK_ROUTING.root_start();

  simple_udp_register(&udp_conn,
                      UDP_SERVER_PORT,
                      NULL,
                      UDP_CLIENT_PORT,
                      udp_rx_callback);

  LOG_INFO("==================================================\n");
  LOG_INFO("SERVER_BOOT\n");
  LOG_INFO("Mode: RPL root UDP server\n");
  LOG_INFO("Application behavior: receive only, no reply\n");
  LOG_INFO("==================================================\n");

  while(1) {
    PROCESS_YIELD();
  }

  PROCESS_END();
}