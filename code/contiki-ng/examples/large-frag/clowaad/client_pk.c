#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "sys/log.h"

#include <stdint.h>

#define LOG_MODULE "MLKEM-CLIENT"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

/*
 * Delais de l'experience.
 *
 * START_DELAY:
 *   laisse le temps a RPL de construire la route.
 *
 * CT_TIMEOUT:
 *   temps maximal attendu pour recevoir le ciphertext associe.
 *
 * INTER_CASE_DELAY:
 *   pause entre deux niveaux ML-KEM.
 *
 * ROUTE_RETRY_DELAY:
 *   nouvelle verification lorsque le root n'est pas encore joignable.
 */
#define START_DELAY       (15 * CLOCK_SECOND)
#define CT_TIMEOUT        (30 * CLOCK_SECOND)
#define INTER_CASE_DELAY  (10 * CLOCK_SECOND)
#define ROUTE_RETRY_DELAY (5 * CLOCK_SECOND)

/*
 * Par defaut, aucune retransmission applicative.
 * Cela permet de mesurer le comportement brut de 6LoWPAN + MAC + Gilbert.
 *
 * Mettre a 1 ou plus uniquement dans une experience consacree aux
 * retransmissions applicatives.
 */
#define MAX_APP_RETRIES 0

#define MAX_PAYLOAD_LEN 1568

/*
 * Header applicatif inclus DANS les tailles ML-KEM annoncees :
 *
 *   octets 0..1 : sequence
 *   octet  2    : indice du niveau ML-KEM
 *   octet  3    : type du message (PK ou CT)
 *   octets 4..5 : longueur totale annoncee
 *   octets 6..9 : horodatage du premier envoi PK, cote client
 *
 * HDR_LEN = 10 octets.
 *
 * Important :
 * un PK ML-KEM-512 est transmis avec datalen = 800 octets au total.
 * Le header remplace donc 10 octets du contenu factice ; il ne s'ajoute
 * pas aux 800 octets. Cette convention reproduit exactement la taille
 * reseau de l'objet ML-KEM etudie.
 */
#define HDR_LEN 10

#define MSG_TYPE_PK 1
#define MSG_TYPE_CT 2

typedef struct {
  const char *label;
  uint16_t pk_size;
  uint16_t ct_size;
} mlkem_case_t;

static const mlkem_case_t cases[] = {
  { "MLKEM512",   800,  768 },
  { "MLKEM768",  1184, 1088 },
  { "MLKEM1024", 1568, 1568 }
};

#define NUM_CASES ((uint8_t)(sizeof(cases) / sizeof(cases[0])))

static struct simple_udp_connection udp_conn;
static uint8_t tx_buf[MAX_PAYLOAD_LEN];

/* Etat applicatif du client. */
static uint8_t current_case_idx;
static uint16_t current_seq;
static uint8_t waiting_for_ct;
static uint8_t ct_received;
static uint8_t retries_done;
static uint8_t experiment_finished;

PROCESS(udp_client_process,
        "ML-KEM client: send public key and receive ciphertext");
AUTOSTART_PROCESSES(&udp_client_process);

/*---------------------------------------------------------------------------*/
static void
write_u16_be(uint8_t *dst, uint16_t value)
{
  dst[0] = (uint8_t)(value >> 8);
  dst[1] = (uint8_t)(value & 0xff);
}
/*---------------------------------------------------------------------------*/
static uint16_t
read_u16_be(const uint8_t *src)
{
  return ((uint16_t)src[0] << 8) | (uint16_t)src[1];
}
/*---------------------------------------------------------------------------*/
static void
write_u32_be(uint8_t *dst, uint32_t value)
{
  dst[0] = (uint8_t)(value >> 24);
  dst[1] = (uint8_t)(value >> 16);
  dst[2] = (uint8_t)(value >> 8);
  dst[3] = (uint8_t)(value & 0xff);
}
/*---------------------------------------------------------------------------*/
static uint32_t
read_u32_be(const uint8_t *src)
{
  return ((uint32_t)src[0] << 24) |
         ((uint32_t)src[1] << 16) |
         ((uint32_t)src[2] << 8)  |
         (uint32_t)src[3];
}
/*---------------------------------------------------------------------------*/
/*
 * Construit un payload de taille totale pk_len.
 * Le contenu n'est pas une vraie cle publique ML-KEM : seule la taille
 * reseau est reproduite pour etudier la fragmentation 6LoWPAN.
 */
static uint32_t
build_pk_payload(uint16_t seq, uint8_t case_idx, uint16_t pk_len)
{
  uint16_t i;
  uint32_t tx_timestamp = (uint32_t)clock_time();

  write_u16_be(&tx_buf[0], seq);
  tx_buf[2] = case_idx;
  tx_buf[3] = MSG_TYPE_PK;
  write_u16_be(&tx_buf[4], pk_len);
  write_u32_be(&tx_buf[6], tx_timestamp);

  for(i = HDR_LEN; i < pk_len; i++) {
    tx_buf[i] = (uint8_t)((i + seq + case_idx) & 0xff);
  }

  return tx_timestamp;
}
/*---------------------------------------------------------------------------*/
static void
send_current_pk(const uip_ipaddr_t *dest_ipaddr)
{
  uint16_t pk_len;
  uint32_t tx_timestamp;

  pk_len = cases[current_case_idx].pk_size;
  tx_timestamp = build_pk_payload(current_seq, current_case_idx, pk_len);

  LOG_INFO("TX PK case=%s case_idx=%u seq=%u declared=%u datalen=%u "
           "attempt=%u tx_ticks=%lu to ",
           cases[current_case_idx].label,
           current_case_idx,
           current_seq,
           pk_len,
           pk_len,
           (unsigned)(retries_done + 1),
           (unsigned long)tx_timestamp);
  LOG_INFO_6ADDR(dest_ipaddr);
  LOG_INFO_("\n");

  simple_udp_sendto(&udp_conn, tx_buf, pk_len, dest_ipaddr);
  waiting_for_ct = 1;
}
/*---------------------------------------------------------------------------*/
static void
udp_client_rx_callback(struct simple_udp_connection *c,
                       const uip_ipaddr_t *sender_addr,
                       uint16_t sender_port,
                       const uip_ipaddr_t *receiver_addr,
                       uint16_t receiver_port,
                       const uint8_t *data,
                       uint16_t datalen)
{
  uint16_t seq;
  uint8_t case_idx;
  uint8_t msg_type;
  uint16_t declared_len;
  uint32_t original_tx_timestamp;
  uint32_t now;
  uint32_t rtt_ticks;
  uint16_t expected_ct_len;

  if(datalen < HDR_LEN) {
    LOG_WARN("RX paquet trop court: datalen=%u\n", datalen);
    return;
  }

  seq = read_u16_be(&data[0]);
  case_idx = data[2];
  msg_type = data[3];
  declared_len = read_u16_be(&data[4]);
  original_tx_timestamp = read_u32_be(&data[6]);

  if(msg_type != MSG_TYPE_CT) {
    LOG_WARN("RX type inattendu=%u seq=%u case_idx=%u datalen=%u\n",
             msg_type, seq, case_idx, datalen);
    return;
  }

  if(case_idx >= NUM_CASES) {
    LOG_WARN("RX CT case_idx invalide=%u seq=%u datalen=%u\n",
             case_idx, seq, datalen);
    return;
  }

  expected_ct_len = cases[case_idx].ct_size;

  if(declared_len != datalen) {
    LOG_WARN("RX CT longueur incoherente case=%s seq=%u "
             "declared=%u datalen=%u\n",
             cases[case_idx].label, seq, declared_len, datalen);
    return;
  }

  if(datalen != expected_ct_len) {
    LOG_WARN("RX CT mauvaise taille case=%s seq=%u "
             "expected=%u received=%u\n",
             cases[case_idx].label, seq, expected_ct_len, datalen);
    return;
  }

  /*
   * Un CT ancien ou duplique ne doit pas faire avancer l'automate
   * applicatif du client.
   */
  if(!waiting_for_ct ||
     case_idx != current_case_idx ||
     seq != current_seq) {
    LOG_WARN("RX CT stale/duplicate case=%s case_idx=%u seq=%u size=%u; "
             "attendu case_idx=%u seq=%u waiting=%u\n",
             cases[case_idx].label,
             case_idx,
             seq,
             datalen,
             current_case_idx,
             current_seq,
             waiting_for_ct);
    return;
  }

  now = (uint32_t)clock_time();
  rtt_ticks = now - original_tx_timestamp;

  LOG_INFO("RX CT VALID case=%s case_idx=%u seq=%u "
           "declared=%u datalen=%u rtt_ticks=%lu rtt_ms=%lu from ",
           cases[case_idx].label,
           case_idx,
           seq,
           declared_len,
           datalen,
           (unsigned long)rtt_ticks,
           (unsigned long)((rtt_ticks * 1000UL) / CLOCK_SECOND));
  LOG_INFO_6ADDR(sender_addr);
  LOG_INFO_("\n");

  ct_received = 1;

  /*
   * Reveille le processus afin qu'il passe au niveau ML-KEM suivant
   * sans attendre l'expiration du timer de timeout.
   */
  process_poll(&udp_client_process);
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer app_timer;
  static uip_ipaddr_t root_ipaddr;

  PROCESS_BEGIN();

  current_case_idx = 0;
  current_seq = 0;
  waiting_for_ct = 0;
  ct_received = 0;
  retries_done = 0;
  experiment_finished = 0;

  simple_udp_register(&udp_conn,
                      UDP_CLIENT_PORT,
                      NULL,
                      UDP_SERVER_PORT,
                      udp_client_rx_callback);

  LOG_INFO("Client initialise: %u niveaux ML-KEM, PK sizes="
           "800/1184/1568, CT sizes=768/1088/1568\n",
           NUM_CASES);

  etimer_set(&app_timer, START_DELAY);

  while(1) {
    PROCESS_WAIT_EVENT();

    /*
     * Reception valide d'un CT : succes de l'echange courant.
     */
    if(ev == PROCESS_EVENT_POLL && ct_received) {
      ct_received = 0;
      waiting_for_ct = 0;
      retries_done = 0;

      LOG_INFO("EXCHANGE SUCCESS case=%s case_idx=%u seq=%u "
               "pk_size=%u ct_size=%u\n",
               cases[current_case_idx].label,
               current_case_idx,
               current_seq,
               cases[current_case_idx].pk_size,
               cases[current_case_idx].ct_size);

      current_case_idx++;
      current_seq++;

      if(current_case_idx >= NUM_CASES) {
        experiment_finished = 1;
        LOG_INFO("EXPERIMENT COMPLETE: %u echanges ML-KEM traites\n",
                 NUM_CASES);
      } else {
        etimer_set(&app_timer, INTER_CASE_DELAY);
      }
    }

    if(ev == PROCESS_EVENT_TIMER && data == &app_timer) {
      if(experiment_finished) {
        /*
         * On garde le processus actif pour que les callbacks UDP restent
         * valides et pour journaliser d'eventuels paquets retardes.
         */
        continue;
      }

      if(waiting_for_ct) {
        LOG_WARN("CT TIMEOUT case=%s case_idx=%u seq=%u "
                 "pk_size=%u expected_ct_size=%u retries_done=%u\n",
                 cases[current_case_idx].label,
                 current_case_idx,
                 current_seq,
                 cases[current_case_idx].pk_size,
                 cases[current_case_idx].ct_size,
                 retries_done);

        if(retries_done < MAX_APP_RETRIES) {
          retries_done++;

          if(NETSTACK_ROUTING.node_is_reachable() &&
             NETSTACK_ROUTING.get_root_ipaddr(&root_ipaddr)) {
            send_current_pk(&root_ipaddr);
            etimer_set(&app_timer, CT_TIMEOUT);
          } else {
            waiting_for_ct = 0;
            LOG_WARN("Route perdue avant retransmission du cas %s\n",
                     cases[current_case_idx].label);
            etimer_set(&app_timer, ROUTE_RETRY_DELAY);
          }
        } else {
          LOG_WARN("EXCHANGE FAILED case=%s case_idx=%u seq=%u "
                   "pk_size=%u ct_size=%u\n",
                   cases[current_case_idx].label,
                   current_case_idx,
                   current_seq,
                   cases[current_case_idx].pk_size,
                   cases[current_case_idx].ct_size);

          waiting_for_ct = 0;
          retries_done = 0;
          current_case_idx++;
          current_seq++;

          if(current_case_idx >= NUM_CASES) {
            experiment_finished = 1;
            LOG_INFO("EXPERIMENT COMPLETE: tous les cas ont ete testes\n");
          } else {
            etimer_set(&app_timer, INTER_CASE_DELAY);
          }
        }
      } else {
        /*
         * Aucun echange en cours : essayer d'envoyer le PK du cas courant.
         */
        if(NETSTACK_ROUTING.node_is_reachable() &&
           NETSTACK_ROUTING.get_root_ipaddr(&root_ipaddr)) {
          send_current_pk(&root_ipaddr);
          etimer_set(&app_timer, CT_TIMEOUT);
        } else {
          LOG_INFO("Root non joignable; cas en attente=%s case_idx=%u\n",
                   cases[current_case_idx].label,
                   current_case_idx);
          etimer_set(&app_timer, ROUTE_RETRY_DELAY);
        }
      }
    }
  }

  PROCESS_END();
}