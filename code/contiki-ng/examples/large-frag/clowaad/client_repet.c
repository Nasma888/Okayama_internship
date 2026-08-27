#include "contiki.h"
#include "net/routing/routing.h"
#include "net/ipv6/simple-udp.h"
#include "sys/log.h"

#include <stdint.h>
#include <string.h>

#define LOG_MODULE "App"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

#define NUM_MLKEM_CASES       5
#define REPETITIONS_PER_CASE 10

#define MAX_PAYLOAD_SIZE      1568
#define INITIAL_DELAY         (30 * CLOCK_SECOND)
#define ROUTE_RETRY_DELAY     (5 * CLOCK_SECOND)
#define SEND_INTERVAL         (15 * CLOCK_SECOND)

struct mlkem_case {
  const char *name;
  uint16_t payload_size;
};

static const struct mlkem_case mlkem_cases[NUM_MLKEM_CASES] = {
  { "MLKEM512_EK",     800  },
  { "MLKEM512_CT",     768  },
  { "MLKEM768_EK",     1184 },
  { "MLKEM768_CT",     1088 },
  { "MLKEM1024_EKCT",  1568 }
};

static struct simple_udp_connection udp_conn;
static uint8_t payload[MAX_PAYLOAD_SIZE];

PROCESS(udp_client_process,
        "UDP client - repetitions ML-KEM");

AUTOSTART_PROCESSES(&udp_client_process);

/*---------------------------------------------------------------------------*/
/*
 * Structure du payload :
 *
 * octets 0-1 : numéro de séquence
 * octets 2-5 : instant d'envoi en ticks
 * octets 6-N : données artificielles
 */
static void
fill_payload(uint16_t seq, uint16_t payload_size)
{
  uint16_t i;
  uint32_t tx_timestamp;

  tx_timestamp = (uint32_t)clock_time();

  /* Numéro de séquence en big-endian */
  payload[0] = (uint8_t)(seq >> 8);
  payload[1] = (uint8_t)(seq & 0xff);

  /* Timestamp en big-endian */
  payload[2] = (uint8_t)(tx_timestamp >> 24);
  payload[3] = (uint8_t)(tx_timestamp >> 16);
  payload[4] = (uint8_t)(tx_timestamp >> 8);
  payload[5] = (uint8_t)(tx_timestamp & 0xff);

  for(i = 6; i < payload_size; i++) {
    payload[i] = (uint8_t)(i & 0xff);
  }
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer send_timer;
  static uip_ipaddr_t root_ipaddr;

  static uint8_t case_index;
  static uint8_t repetition;
  static uint16_t seq;

  PROCESS_BEGIN();

  simple_udp_register(&udp_conn,
                      UDP_CLIENT_PORT,
                      NULL,
                      UDP_SERVER_PORT,
                      NULL);

  case_index = 0;
  repetition = 0;
  seq = 0;

  LOG_INFO("Experience ML-KEM demarree\n");
  LOG_INFO("Nombre de cas=%u, repetitions par cas=%u\n",
           NUM_MLKEM_CASES,
           REPETITIONS_PER_CASE);

  /*
   * Laisser le temps à RPL de construire la route vers le root.
   */
  etimer_set(&send_timer, INITIAL_DELAY);

  while(case_index < NUM_MLKEM_CASES) {

    PROCESS_WAIT_EVENT_UNTIL(etimer_expired(&send_timer));

    /*
     * Vérifier que le client possède une route vers le root RPL.
     */
    if(!NETSTACK_ROUTING.node_is_reachable() ||
       !NETSTACK_ROUTING.get_root_ipaddr(&root_ipaddr)) {

      LOG_INFO("Pas encore de route vers le root : "
               "case=%s repetition=%u/%u\n",
               mlkem_cases[case_index].name,
               repetition + 1,
               REPETITIONS_PER_CASE);

      etimer_set(&send_timer, ROUTE_RETRY_DELAY);
      continue;
    }

    /*
     * Construire le payload correspondant au cas ML-KEM courant.
     */
    fill_payload(seq,
                 mlkem_cases[case_index].payload_size);

    LOG_INFO("TX case=%s size=%u repetition=%u/%u seq=%u\n",
             mlkem_cases[case_index].name,
             mlkem_cases[case_index].payload_size,
             repetition + 1,
             REPETITIONS_PER_CASE,
             seq);

    /*
     * Envoi du datagramme UDP complet.
     * 6LoWPAN effectuera automatiquement la fragmentation.
     */
    simple_udp_sendto(&udp_conn,
                      payload,
                      mlkem_cases[case_index].payload_size,
                      &root_ipaddr);

    seq++;
    repetition++;

    /*
     * Après 10 répétitions, passer au cas ML-KEM suivant.
     */
    if(repetition >= REPETITIONS_PER_CASE) {
      LOG_INFO("Case terminee : %s, %u transmissions envoyees\n",
               mlkem_cases[case_index].name,
               REPETITIONS_PER_CASE);

      repetition = 0;
      case_index++;
    }

    /*
     * Espacement entre deux datagrammes afin d'éviter de saturer
     * les files d'attente et le buffer de réassemblage.
     */
    etimer_set(&send_timer, SEND_INTERVAL);
  }

  LOG_INFO("Experience terminee : %u transmissions envoyees\n",
           NUM_MLKEM_CASES * REPETITIONS_PER_CASE);

  while(1) {
    PROCESS_WAIT_EVENT();
  }

  PROCESS_END();
}
/*---------------------------------------------------------------------------*/