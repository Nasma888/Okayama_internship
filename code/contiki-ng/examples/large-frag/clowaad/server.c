// modèle 1--2 tout le temps
// #include "contiki.h"
// #include "net/routing/routing.h"
// #include "net/netstack.h"
// #include "net/ipv6/simple-udp.h"
// #include "sys/log.h"

// #define LOG_MODULE "App"
// #define LOG_LEVEL LOG_LEVEL_INFO

// static struct simple_udp_connection udp_conn;
// static uint16_t last_seq = UINT16_MAX;

// PROCESS(udp_server_process, " UDP server - etude fragmentation ML-KEM");
// AUTOSTART_PROCESSES(&udp_server_process);

// /*---------------------------------------------------------------------------*/
// static void
// udp_rx_callback(struct simple_udp_connection *c,
//                 const uip_ipaddr_t *sender_addr,
//                 uint16_t sender_port,
//                 const uip_ipaddr_t *receiver_addr,
//                 uint16_t receiver_port,
//                 const uint8_t *data,
//                 uint16_t datalen)
// {
//   uint16_t seq;
//   uint32_t tx_ts, now, latency;

//   if(datalen < 6) {
//     LOG_WARN("RX paquet trop court, len=%u\n", datalen);
//     return;
//   }

//   seq = ((uint16_t)data[0] << 8) | data[1];

//   tx_ts = ((uint32_t)data[2] << 24) |
//           ((uint32_t)data[3] << 16) |
//           ((uint32_t)data[4] << 8)  |
//           (uint32_t)data[5];

//   now = (uint32_t)clock_time();
//   latency = now - tx_ts;

//   if(seq == last_seq) {
//     LOG_WARN("RX DUPLICATE seq=%u size=%u\n", seq, datalen);
//   } else {
//     last_seq = seq;

//     LOG_INFO("RX NEW seq=%u size=%u latency_ticks=%lu (%lu ms) from ",
//              seq,
//              datalen,
//              (unsigned long)latency,
//              (unsigned long)((latency * 1000) / CLOCK_SECOND));

//     LOG_INFO_6ADDR(sender_addr);
//     LOG_INFO_("\n");
//   }
// }
// /*---------------------------------------------------------------------------*/
// PROCESS_THREAD(udp_server_process, ev, data)
// {
//   PROCESS_BEGIN();

//   NETSTACK_ROUTING.root_start();

//   simple_udp_register(&udp_conn,
//                       UDP_SERVER_PORT,
//                       NULL,
//                       UDP_CLIENT_PORT,
//                       udp_rx_callback);

//   PROCESS_END();
// }

#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "sys/log.h"

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define MAX_PAYLOAD_LEN 1568

/* Doit rester identique au client : [2o seq][1o case_idx][4o horodatage
 * TX original] = 7 octets. Le serveur RECOPIE ce header tel quel dans sa
 * reponse ct (seq/case_idx/ts_orig inchanges), ce qui permet au client de
 * calculer une latence aller-retour exacte sans horloge partagee. */
#define HDR_LEN 7

static struct simple_udp_connection udp_conn;
static uint16_t last_rx_seq = UINT16_MAX;

/* Doit rester synchronise avec le tableau "cases" du client (memes
 * tailles ek/ct, meme ordre). Utilise ici uniquement pour connaitre la
 * taille du ciphertext a renvoyer et pour l'etiquette de log. */
typedef struct {
  const char *label;
  uint16_t    ek_size;
  uint16_t    ct_size;
} mlkem_case_t;

static const mlkem_case_t cases[] = {
  { "MLKEM512",  800,  768 },
  { "MLKEM768",  1184, 1088 },
  { "MLKEM1024", 1568, 1568 },
};
#define NUM_CASES (sizeof(cases) / sizeof(cases[0]))

static uint8_t ct_buf[MAX_PAYLOAD_LEN];

PROCESS(udp_server_process, "UDP server - etude fragmentation ML-KEM (ek/ct)");
AUTOSTART_PROCESSES(&udp_server_process);
/*---------------------------------------------------------------------------*/
/* Construit le payload ct de reponse : recopie le header recu (seq,
 * case_idx, ts_orig) tel quel, puis remplit jusqu'a ct_size. Le
 * remplissage n'est pas un vrai ciphertext ML-KEM : on etudie ici
 * uniquement le comportement de fragmentation 6LoWPAN, pas la crypto. */
static void
build_ct_payload(const uint8_t *rx_header, uint16_t ct_len)
{
  uint16_t i;

  for(i = 0; i < HDR_LEN; i++) {
    ct_buf[i] = rx_header[i];
  }
  for(i = HDR_LEN; i < ct_len; i++) {
    ct_buf[i] = (uint8_t)(i & 0xff);
  }
}
/*---------------------------------------------------------------------------*/
static void
udp_rx_callback(struct simple_udp_connection *c,
                 const uip_ipaddr_t *sender_addr,
                 uint16_t sender_port,
                 const uip_ipaddr_t *receiver_addr,
                 uint16_t receiver_port,
                 const uint8_t *data,
                 uint16_t datalen)
{
  uint16_t seq;
  uint8_t case_idx;
  uint32_t tx_ts, now, latency;
  const char *label = "UNKNOWN";
  uint16_t ct_len;

  if(datalen < HDR_LEN) {
    LOG_WARN("RX ek trop court, len=%u\n", datalen);
    return;
  }

  seq      = ((uint16_t)data[0] << 8) | data[1];
  case_idx = data[2];
  tx_ts    = ((uint32_t)data[3] << 24) |
             ((uint32_t)data[4] << 16) |
             ((uint32_t)data[5] << 8)  |
             (uint32_t)data[6];

  now = (uint32_t)clock_time();
  latency = now - tx_ts;

  if(case_idx >= NUM_CASES) {
    LOG_WARN("RX EK case_idx invalide (%u), paquet ignore\n", case_idx);
    return;
  }
  label = cases[case_idx].label;

  if(seq == last_rx_seq) {
    LOG_WARN("RX EK DUPLICATE case=%s seq=%u size=%u\n", label, seq, datalen);
    /* On repond quand meme : la reponse precedente a pu se perdre. */
  } else {
    last_rx_seq = seq;
    LOG_INFO("RX EK NEW case=%s seq=%u size=%u latency_ticks=%lu (%lu ms) from ",
             label, seq, datalen,
             (unsigned long)latency,
             (unsigned long)((latency * 1000) / CLOCK_SECOND));
    LOG_INFO_6ADDR(sender_addr);
    LOG_INFO_("\n");
  }

  /* Encapsulation simulee : on renvoie immediatement le ct correspondant,
   * de la taille reelle FIPS 203 pour ce niveau ML-KEM. */
  ct_len = cases[case_idx].ct_size;
  build_ct_payload(data, ct_len);

  LOG_INFO("TX CT case=%s size=%u seq=%u to ", label, ct_len, seq);
  LOG_INFO_6ADDR(sender_addr);
  LOG_INFO_("\n");

  simple_udp_sendto(&udp_conn, ct_buf, ct_len, sender_addr);
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_server_process, ev, data)
{
  PROCESS_BEGIN();

  NETSTACK_ROUTING.root_start();

  simple_udp_register(&udp_conn,
                       UDP_SERVER_PORT,
                       NULL,
                       UDP_CLIENT_PORT,
                       udp_rx_callback);

  PROCESS_END();
}