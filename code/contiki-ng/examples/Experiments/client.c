#include "contiki.h"
#include "net/routing/routing.h"
#include "net/netstack.h"
#include "net/ipv6/simple-udp.h"
#include "sys/log.h"

#include "experiment-variants.h"
#include "experiment-stats.h"
#include "experiment-energy.h"

#ifndef EXPERIMENT_ENERGY_API_VERSION
#error "Mauvais experiment-energy.h: le fichier API v2 n'est pas inclus"
#endif
#if EXPERIMENT_ENERGY_API_VERSION != 2
#error "Version incompatible de experiment-energy.h"
#endif

#ifndef EXPERIMENT_STATS_API_VERSION
#error "Mauvais experiment-stats.h: le fichier API v2 n'est pas inclus"
#endif
#if EXPERIMENT_STATS_API_VERSION != 2
#error "Version incompatible de experiment-stats.h"
#endif

#include <inttypes.h>
#include <stdint.h>
#include <string.h>

#define LOG_MODULE "MLKEM-CLIENT"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

#ifndef APP_CONF_START_DELAY
#define APP_CONF_START_DELAY (15 * CLOCK_SECOND)
#endif
#ifndef APP_CONF_CT_TIMEOUT
#define APP_CONF_CT_TIMEOUT (30 * CLOCK_SECOND)
#endif
#ifndef APP_CONF_ROUTE_RETRY_DELAY
#define APP_CONF_ROUTE_RETRY_DELAY (5 * CLOCK_SECOND)
#endif

/*
 * Durée maximale pendant laquelle le client attend la confirmation MAC du
 * FULL ACK RFRAG après la livraison du CT. Ce délai ne s'applique qu'aux
 * variantes RFRAG et uniquement si le callback MAC n'a pas encore confirmé
 * l'ACK. En fonctionnement normal, la mesure s'arrête après quelques ticks.
 */
#ifndef APP_CONF_ENERGY_PROTOCOL_END_TIMEOUT
#define APP_CONF_ENERGY_PROTOCOL_END_TIMEOUT (10 * CLOCK_SECOND)
#endif

#define START_DELAY APP_CONF_START_DELAY
#define CT_TIMEOUT APP_CONF_CT_TIMEOUT
#define ROUTE_RETRY_DELAY APP_CONF_ROUTE_RETRY_DELAY
#define ENERGY_PROTOCOL_END_TIMEOUT APP_CONF_ENERGY_PROTOCOL_END_TIMEOUT

#ifndef APP_CONF_ENERGY_MODEL_NAME
#define APP_CONF_ENERGY_MODEL_NAME "Z1_CC2420_APPROX"
#endif

#define MAX_PAYLOAD_LEN 1568
#define HDR_LEN 10
#define MSG_TYPE_PK 1
#define MSG_TYPE_CT 2

/* Définition locale requise par experiment-stats.h. */
experiment_net_stats_t experiment_net_stats;

typedef struct {
  uint32_t pk_attempts;
  uint32_t app_retries;
  uint32_t app_timeouts;
  uint32_t ct_valid_rx;
  uint32_t ct_invalid_rx;
  uint32_t ct_stale_rx;
  uint32_t route_waits;
  uint32_t first_tx_ticks;
  uint32_t ct_rx_ticks;
  uint32_t rtt_ticks;
  uint32_t decision_ticks;
} client_app_stats_t;

static client_app_stats_t app_stats;
static struct simple_udp_connection udp_conn;
static uint8_t tx_buf[MAX_PAYLOAD_LEN];

static uint16_t current_seq;
static uint8_t waiting_for_ct;
static uint8_t ct_received;
static uint8_t result_success;
static uint8_t experiment_finished;
static uint8_t waiting_for_protocol_end;
static clock_time_t protocol_end_deadline;

static energy_meter_t exchange_energy;
static uint8_t energy_started;
static uint8_t energy_finished;
static const char *energy_end_reason;

PROCESS(udp_client_process,
        "ML-KEM client: one selected PK/CT exchange");
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
static uint32_t
ticks_to_ms(uint32_t ticks)
{
  return (ticks * 1000UL) / CLOCK_SECOND;
}
/*---------------------------------------------------------------------------*/
static void
start_energy_measurement(void)
{
  if(energy_started) {
    return;
  }

  energy_meter_start(&exchange_energy);
  energy_started = 1;

  LOG_INFO("ENERGY_START,role=client,variant=%u,kem=%u,seed=%lu,"
           "model=%s,boundary=before_first_pk,energest_second=%lu\n",
           (unsigned)EXPERIMENT_VARIANT,
           (unsigned)EXPERIMENT_KEM_LEVEL,
           (unsigned long)EXPERIMENT_SEED,
           APP_CONF_ENERGY_MODEL_NAME,
           (unsigned long)ENERGEST_SECOND);
}
/*---------------------------------------------------------------------------*/
static void
finish_energy_measurement(const char *reason)
{
  if(!energy_started || energy_finished) {
    return;
  }

  energy_end_reason = reason;

  /*
   * Cette fonction prend elle-même le snapshot final, calcule le delta
   * Energest et imprime RESULT_ENERGY_EXCHANGE.
   */
  energy_meter_stop_and_report(&exchange_energy,
                               "client",
                               result_success ? "success" : "failure",
                               reason,
                               (unsigned)EXPERIMENT_VARIANT,
                               (unsigned)EXPERIMENT_KEM_LEVEL,
                               (unsigned long)EXPERIMENT_SEED);

  energy_finished = 1;
}
/*---------------------------------------------------------------------------*/
static void
log_experiment_configuration(void)
{
  LOG_INFO("EXPERIMENT_CONFIG,role=client,variant=%u,variant_name=%s,"
           "kem=%u,kem_name=%s,seed=%lu,rfrag=%u,ack_first=%u,"
           "idle_ack=%u,app_retry=%u,max_app_retries=%u,energest=%u,"
           "energy_model=%s\n",
           (unsigned)EXPERIMENT_VARIANT,
           EXPERIMENT_VARIANT_NAME,
           (unsigned)EXPERIMENT_KEM_LEVEL,
           EXPERIMENT_KEM_NAME,
           (unsigned long)EXPERIMENT_SEED,
           (unsigned)EXPERIMENT_RFRAG_ENABLED,
           (unsigned)EXPERIMENT_RFRAG_ACK_FIRST,
           (unsigned)EXPERIMENT_RFRAG_ACK_IDLE_ENABLED,
           (unsigned)EXPERIMENT_APP_RETRY_ENABLED,
           (unsigned)EXPERIMENT_MAX_APP_RETRIES,
           (unsigned)ENERGEST_CONF_ON,
           APP_CONF_ENERGY_MODEL_NAME);
}
/*---------------------------------------------------------------------------*/
static void
build_pk_payload(uint16_t seq, uint32_t original_tx_timestamp)
{
  uint16_t i;

  write_u16_be(&tx_buf[0], seq);
  tx_buf[2] = EXPERIMENT_CASE_INDEX;
  tx_buf[3] = MSG_TYPE_PK;
  write_u16_be(&tx_buf[4], EXPERIMENT_PK_SIZE);
  write_u32_be(&tx_buf[6], original_tx_timestamp);

  for(i = HDR_LEN; i < EXPERIMENT_PK_SIZE; i++) {
    tx_buf[i] = (uint8_t)((i + seq + EXPERIMENT_CASE_INDEX) & 0xff);
  }
}
/*---------------------------------------------------------------------------*/
static void
send_selected_pk(const uip_ipaddr_t *dest_ipaddr)
{
  uint32_t attempt_tx_ticks;

  /* La fenêtre commence exactement avant le premier PK, jamais avant. */
  if(app_stats.pk_attempts == 0) {
    start_energy_measurement();
    app_stats.first_tx_ticks = (uint32_t)clock_time();
  }

  attempt_tx_ticks = (uint32_t)clock_time();
  build_pk_payload(current_seq, app_stats.first_tx_ticks);

  app_stats.pk_attempts++;
  if(app_stats.pk_attempts > 1) {
    app_stats.app_retries++;
  }

  LOG_INFO("TX PK case=%s case_idx=%u seq=%u declared=%u datalen=%u "
           "attempt=%lu original_tx_ticks=%lu attempt_tx_ticks=%lu to ",
           EXPERIMENT_KEM_NAME,
           (unsigned)EXPERIMENT_CASE_INDEX,
           current_seq,
           (unsigned)EXPERIMENT_PK_SIZE,
           (unsigned)EXPERIMENT_PK_SIZE,
           (unsigned long)app_stats.pk_attempts,
           (unsigned long)app_stats.first_tx_ticks,
           (unsigned long)attempt_tx_ticks);
  LOG_INFO_6ADDR(dest_ipaddr);
  LOG_INFO_("\n");

  simple_udp_sendto(&udp_conn,
                    tx_buf,
                    EXPERIMENT_PK_SIZE,
                    dest_ipaddr);
  waiting_for_ct = 1;
}
/*---------------------------------------------------------------------------*/
static void
print_result(void)
{
  uint32_t elapsed_ticks;
  uint32_t elapsed_ms;
  uint32_t rtt_ms;

  elapsed_ticks = app_stats.first_tx_ticks == 0 ? 0 :
                  app_stats.decision_ticks - app_stats.first_tx_ticks;
  elapsed_ms = ticks_to_ms(elapsed_ticks);
  rtt_ms = ticks_to_ms(app_stats.rtt_ticks);

  LOG_INFO("RESULT,role=client,variant=%u,variant_name=%s,kem=%u,"
           "kem_name=%s,seed=%lu,success=%u,seq=%u,pk_size=%u,ct_size=%u,"
           "pk_frags_expected=%u,ct_frags_expected=%u,app_attempts=%lu,"
           "app_retries=%lu,app_timeouts=%lu,rtt_valid=%u,rtt_ticks=%lu,"
           "rtt_ms=%lu,elapsed_ticks=%lu,elapsed_ms=%lu,energy_end_reason=%s\n",
           (unsigned)EXPERIMENT_VARIANT,
           EXPERIMENT_VARIANT_NAME,
           (unsigned)EXPERIMENT_KEM_LEVEL,
           EXPERIMENT_KEM_NAME,
           (unsigned long)EXPERIMENT_SEED,
           (unsigned)result_success,
           current_seq,
           (unsigned)EXPERIMENT_PK_SIZE,
           (unsigned)EXPERIMENT_CT_SIZE,
           (unsigned)EXPERIMENT_PK_FRAGS_EXPECTED,
           (unsigned)EXPERIMENT_CT_FRAGS_EXPECTED,
           (unsigned long)app_stats.pk_attempts,
           (unsigned long)app_stats.app_retries,
           (unsigned long)app_stats.app_timeouts,
           (unsigned)(app_stats.ct_valid_rx > 0),
           (unsigned long)app_stats.rtt_ticks,
           (unsigned long)rtt_ms,
           (unsigned long)elapsed_ticks,
           (unsigned long)elapsed_ms,
           energy_end_reason != NULL ? energy_end_reason : "not_finished");

  LOG_INFO("RESULT_NET,role=client,variant=%u,kem=%u,seed=%lu,"
           "datagram_tx=%lu,datagram_rx_complete=%lu,data_frag_tx=%lu,"
           "data_frag_rx=%lu,data_bytes_tx=%lu,data_bytes_rx=%lu,"
           "rfrag_retx_tx=%lu,rfrag_ack_tx=%lu,rfrag_ack_rx=%lu,rfrag_ack_tx_ok=%lu,"
           "rfrag_full_ack_tx_done=%lu,rfrag_full_ack_tx_ok=%lu,"
           "idle_ack_tx=%lu,rto_expired=%lu,null_ack_rx=%lu,"
           "partial_ack_rx=%lu,full_ack_rx=%lu,ctx_create=%lu,"
           "ctx_complete=%lu,ctx_timeout_incomplete=%lu,"
           "ctx_timeout_complete=%lu,ctx_alloc_fail=%lu,"
           "ctx_active_now=%lu,ctx_active_peak=%lu,ctx_ticks_closed=%lu\n",
           (unsigned)EXPERIMENT_VARIANT,
           (unsigned)EXPERIMENT_KEM_LEVEL,
           (unsigned long)EXPERIMENT_SEED,
           (unsigned long)experiment_net_stats.datagram_tx_start,
           (unsigned long)experiment_net_stats.datagram_rx_complete,
           (unsigned long)experiment_net_stats.data_frag_tx,
           (unsigned long)experiment_net_stats.data_frag_rx,
           (unsigned long)experiment_net_stats.lowpan_data_bytes_tx,
           (unsigned long)experiment_net_stats.lowpan_data_bytes_rx,
           (unsigned long)experiment_net_stats.rfrag_retx_tx,
           (unsigned long)experiment_net_stats.rfrag_ack_tx,
           (unsigned long)experiment_net_stats.rfrag_ack_rx,
           (unsigned long)experiment_net_stats.rfrag_ack_tx_ok,
           (unsigned long)experiment_net_stats.rfrag_full_ack_tx_done,
           (unsigned long)experiment_net_stats.rfrag_full_ack_tx_ok,
           (unsigned long)experiment_net_stats.rfrag_idle_ack_tx,
           (unsigned long)experiment_net_stats.rfrag_rto_expired,
           (unsigned long)experiment_net_stats.rfrag_null_ack_rx,
           (unsigned long)experiment_net_stats.rfrag_partial_ack_rx,
           (unsigned long)experiment_net_stats.rfrag_full_ack_rx,
           (unsigned long)experiment_net_stats.context_create,
           (unsigned long)experiment_net_stats.context_complete,
           (unsigned long)experiment_net_stats.context_timeout_incomplete,
           (unsigned long)experiment_net_stats.context_timeout_complete,
           (unsigned long)experiment_net_stats.context_alloc_fail,
           (unsigned long)experiment_net_stats.context_active,
           (unsigned long)experiment_net_stats.context_active_peak,
           (unsigned long)experiment_net_stats.context_lifetime_ticks_closed);


  LOG_INFO("EXPERIMENT COMPLETE: variant=%u kem=%u success=%u\n",
           (unsigned)EXPERIMENT_VARIANT,
           (unsigned)EXPERIMENT_KEM_LEVEL,
           (unsigned)result_success);
}
/*---------------------------------------------------------------------------*/
static void
complete_experiment(const char *energy_reason)
{
  if(experiment_finished) {
    return;
  }

  finish_energy_measurement(energy_reason);
  experiment_finished = 1;
  waiting_for_protocol_end = 0;
  print_result();
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

  (void)c;
  (void)sender_port;
  (void)receiver_addr;
  (void)receiver_port;

  if(datalen < HDR_LEN) {
    app_stats.ct_invalid_rx++;
    LOG_WARN("RX paquet trop court: datalen=%u\n", datalen);
    return;
  }

  seq = read_u16_be(&data[0]);
  case_idx = data[2];
  msg_type = data[3];
  declared_len = read_u16_be(&data[4]);
  original_tx_timestamp = read_u32_be(&data[6]);

  if(msg_type != MSG_TYPE_CT ||
     case_idx != EXPERIMENT_CASE_INDEX ||
     declared_len != datalen ||
     datalen != EXPERIMENT_CT_SIZE) {
    app_stats.ct_invalid_rx++;
    LOG_WARN("RX CT invalid msg_type=%u case_idx=%u declared=%u "
             "datalen=%u expected_case=%u expected_len=%u\n",
             msg_type, case_idx, declared_len, datalen,
             (unsigned)EXPERIMENT_CASE_INDEX,
             (unsigned)EXPERIMENT_CT_SIZE);
    return;
  }

  if(!waiting_for_ct || seq != current_seq) {
    app_stats.ct_stale_rx++;
    LOG_WARN("RX CT stale/duplicate case=%s seq=%u expected_seq=%u "
             "waiting=%u\n",
             EXPERIMENT_KEM_NAME, seq, current_seq, waiting_for_ct);
    return;
  }

  if(original_tx_timestamp != app_stats.first_tx_ticks) {
    app_stats.ct_invalid_rx++;
    LOG_WARN("RX CT timestamp mismatch received=%lu expected=%lu\n",
             (unsigned long)original_tx_timestamp,
             (unsigned long)app_stats.first_tx_ticks);
    return;
  }

  now = (uint32_t)clock_time();
  app_stats.ct_rx_ticks = now;
  app_stats.rtt_ticks = now - app_stats.first_tx_ticks;
  app_stats.ct_valid_rx++;

  LOG_INFO("RX CT VALID case=%s case_idx=%u seq=%u declared=%u "
           "datalen=%u rtt_ticks=%lu rtt_ms=%lu from ",
           EXPERIMENT_KEM_NAME,
           (unsigned)EXPERIMENT_CASE_INDEX,
           seq,
           declared_len,
           datalen,
           (unsigned long)app_stats.rtt_ticks,
           (unsigned long)ticks_to_ms(app_stats.rtt_ticks));
  LOG_INFO_6ADDR(sender_addr);
  LOG_INFO_("\n");

  ct_received = 1;
  process_poll(&udp_client_process);
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_client_process, ev, data)
{
  static struct etimer app_timer;
  static struct etimer protocol_end_timer;
  static uip_ipaddr_t root_ipaddr;

  PROCESS_BEGIN();

  current_seq = 0;
  waiting_for_ct = 0;
  ct_received = 0;
  result_success = 0;
  experiment_finished = 0;
  waiting_for_protocol_end = 0;
  protocol_end_deadline = 0;
  energy_started = 0;
  energy_finished = 0;
  energy_end_reason = NULL;

  memset(&exchange_energy, 0, sizeof(exchange_energy));
  memset(&app_stats, 0, sizeof(app_stats));
  experiment_net_stats_reset();

  simple_udp_register(&udp_conn,
                      UDP_CLIENT_PORT,
                      NULL,
                      UDP_SERVER_PORT,
                      udp_client_rx_callback);

  log_experiment_configuration();
  LOG_INFO("Client initialise: one KEM level only, case=%s case_idx=%u "
           "PK=%u CT=%u\n",
           EXPERIMENT_KEM_NAME,
           (unsigned)EXPERIMENT_CASE_INDEX,
           (unsigned)EXPERIMENT_PK_SIZE,
           (unsigned)EXPERIMENT_CT_SIZE);

  etimer_set(&app_timer, START_DELAY);

  while(1) {
    PROCESS_WAIT_EVENT();

    if(ev == PROCESS_EVENT_POLL && ct_received && !experiment_finished) {
      ct_received = 0;
      waiting_for_ct = 0;
      result_success = 1;
      app_stats.decision_ticks = (uint32_t)clock_time();

      LOG_INFO("EXCHANGE SUCCESS case=%s case_idx=%u seq=%u "
               "pk_size=%u ct_size=%u attempts=%lu\n",
               EXPERIMENT_KEM_NAME,
               (unsigned)EXPERIMENT_CASE_INDEX,
               current_seq,
               (unsigned)EXPERIMENT_PK_SIZE,
               (unsigned)EXPERIMENT_CT_SIZE,
               (unsigned long)app_stats.pk_attempts);

#if EXPERIMENT_RFRAG_ENABLED
      /*
       * Le CT est livré avant que le FULL ACK RFRAG soit forcément terminé au
       * MAC. On attend le compteur alimenté par le callback MAC de sicslowpan.
       */
      if(experiment_net_stats.rfrag_full_ack_tx_ok > 0) {
        complete_experiment("rfrag_full_ack_mac_ok");
      } else {
        waiting_for_protocol_end = 1;
        protocol_end_deadline = clock_time() + ENERGY_PROTOCOL_END_TIMEOUT;
        etimer_set(&protocol_end_timer, 1);
      }
#else
      complete_experiment("ct_delivered_rfc4944");
#endif
    }

    if(ev == PROCESS_EVENT_TIMER && data == &protocol_end_timer &&
       waiting_for_protocol_end && !experiment_finished) {
#if EXPERIMENT_RFRAG_ENABLED
      if(experiment_net_stats.rfrag_full_ack_tx_ok > 0) {
        complete_experiment("rfrag_full_ack_mac_ok");
      } else if(!CLOCK_LT(clock_time(), protocol_end_deadline)) {
        /* Deadline atteinte avec la comparaison wrap-safe de Contiki. */
        complete_experiment("rfrag_protocol_end_timeout");
      } else {
        etimer_set(&protocol_end_timer, 1);
      }
#else
      complete_experiment("ct_delivered_rfc4944");
#endif
    }

    if(ev == PROCESS_EVENT_TIMER && data == &app_timer) {
      if(experiment_finished) {
        continue;
      }

      if(waiting_for_ct) {
        app_stats.app_timeouts++;

        LOG_WARN("CT TIMEOUT case=%s case_idx=%u seq=%u pk_size=%u "
                 "expected_ct_size=%u attempts=%lu app_retries=%lu\n",
                 EXPERIMENT_KEM_NAME,
                 (unsigned)EXPERIMENT_CASE_INDEX,
                 current_seq,
                 (unsigned)EXPERIMENT_PK_SIZE,
                 (unsigned)EXPERIMENT_CT_SIZE,
                 (unsigned long)app_stats.pk_attempts,
                 (unsigned long)app_stats.app_retries);

#if EXPERIMENT_APP_RETRY_ENABLED
        if(app_stats.app_retries < EXPERIMENT_MAX_APP_RETRIES) {
          if(NETSTACK_ROUTING.node_is_reachable() &&
             NETSTACK_ROUTING.get_root_ipaddr(&root_ipaddr)) {
            send_selected_pk(&root_ipaddr);
            etimer_set(&app_timer, CT_TIMEOUT);
          } else {
            waiting_for_ct = 0;
            app_stats.route_waits++;
            LOG_WARN("Route perdue avant retransmission du cas %s\n",
                     EXPERIMENT_KEM_NAME);
            etimer_set(&app_timer, ROUTE_RETRY_DELAY);
          }
        } else
#endif
        {
          waiting_for_ct = 0;
          result_success = 0;
          app_stats.decision_ticks = (uint32_t)clock_time();

          LOG_WARN("EXCHANGE FAILED case=%s case_idx=%u seq=%u "
                   "pk_size=%u ct_size=%u attempts=%lu\n",
                   EXPERIMENT_KEM_NAME,
                   (unsigned)EXPERIMENT_CASE_INDEX,
                   current_seq,
                   (unsigned)EXPERIMENT_PK_SIZE,
                   (unsigned)EXPERIMENT_CT_SIZE,
                   (unsigned long)app_stats.pk_attempts);

          complete_experiment("final_application_timeout");
        }
      } else {
        if(NETSTACK_ROUTING.node_is_reachable() &&
           NETSTACK_ROUTING.get_root_ipaddr(&root_ipaddr)) {
          send_selected_pk(&root_ipaddr);
          etimer_set(&app_timer, CT_TIMEOUT);
        } else {
          app_stats.route_waits++;
          LOG_INFO("Root non joignable; selected case=%s\n",
                   EXPERIMENT_KEM_NAME);
          etimer_set(&app_timer, ROUTE_RETRY_DELAY);
        }
      }
    }
  }

  PROCESS_END();
}