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

#include <stdint.h>
#include <string.h>

#define LOG_MODULE "MLKEM-SERVER"
#define LOG_LEVEL LOG_LEVEL_INFO

#define UDP_CLIENT_PORT 8765
#define UDP_SERVER_PORT 5678

#define MAX_PAYLOAD_LEN 1568
#define HDR_LEN 10
#define MSG_TYPE_PK 1
#define MSG_TYPE_CT 2

/*
 * Instant global auquel le client commence son premier envoi PK et auquel
 * le serveur démarre Energest. Cette valeur DOIT être identique côté client.
 *
 * Choisir une valeur suffisamment tardive pour laisser RPL converger.
 */
#ifndef APP_CONF_EXCHANGE_T0
#define APP_CONF_EXCHANGE_T0 (20 * CLOCK_SECOND)
#endif

#ifndef APP_CONF_CT_TIMEOUT
#define APP_CONF_CT_TIMEOUT (30 * CLOCK_SECOND)
#endif

/*
 * Délai maximal de garde après l'envoi du CT.
 * Ce n'est pas une queue ajoutée aux résultats : la mesure s'arrête dès que
 * la vraie condition de fin est observée.
 */
#ifndef APP_CONF_SERVER_PROTOCOL_END_TIMEOUT
#define APP_CONF_SERVER_PROTOCOL_END_TIMEOUT (10 * CLOCK_SECOND)
#endif

/*
 * Produit un résultat même si aucun PK complet n'arrive.
 * Pour une seule tentative sans reprise applicative :
 * T0 + timeout client + petite marge.
 */
#ifndef APP_CONF_SERVER_RESULT_FALLBACK_DELAY
#define APP_CONF_SERVER_RESULT_FALLBACK_DELAY \
  (APP_CONF_EXCHANGE_T0 + \
   ((EXPERIMENT_MAX_APP_RETRIES + 1) * APP_CONF_CT_TIMEOUT) + \
   (2 * CLOCK_SECOND))
#endif

/* Tolérance d'alignement entre le T0 serveur et le timestamp du client. */
#ifndef APP_CONF_ENERGY_ALIGNMENT_TOLERANCE
#define APP_CONF_ENERGY_ALIGNMENT_TOLERANCE 2
#endif

experiment_net_stats_t experiment_net_stats;

typedef struct {
  uint32_t pk_valid_rx;
  uint32_t pk_duplicate_rx;
  uint32_t pk_invalid_rx;
  uint32_t ct_tx;
  uint32_t last_seq;
  uint32_t last_one_way_ticks;
} server_app_stats_t;

static server_app_stats_t app_stats;
static struct simple_udp_connection udp_conn;
static uint8_t ct_buf[MAX_PAYLOAD_LEN];

static energy_meter_t exchange_energy;

static uint8_t energy_started;
static uint8_t experiment_finished;
static uint8_t waiting_for_protocol_end;
static uint8_t result_success;
static uint8_t energy_window_aligned;

static clock_time_t energy_start_clock;
static clock_time_t protocol_end_deadline;

static uint32_t rfrag_full_ack_rx_before_ct;
static uint32_t rfc4944_final_tx_done_before_ct;
static uint32_t rfc4944_final_tx_ok_before_ct;

static uint16_t last_rx_seq = UINT16_MAX;
static uint8_t last_rx_case_idx = 0xff;
static const char *result_end_reason = "not_finished";

PROCESS(udp_server_process,
        "ML-KEM server: exchange-only energy measurement");
AUTOSTART_PROCESSES(&udp_server_process);

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
static uint32_t
absolute_tick_difference(uint32_t a, uint32_t b)
{
  int32_t signed_difference = (int32_t)(a - b);
  return signed_difference < 0 ?
         (uint32_t)(-signed_difference) :
         (uint32_t)signed_difference;
}
/*---------------------------------------------------------------------------*/
static void
start_energy_measurement(const char *boundary)
{
  if(energy_started) {
    return;
  }

  energy_meter_start(&exchange_energy);
  energy_started = 1;
  energy_start_clock = clock_time();

  LOG_INFO("ENERGY_START,role=server,variant=%u,kem=%u,seed=%lu,"
           "boundary=%s,start_clock=%lu,energest_second=%lu\n",
           (unsigned)EXPERIMENT_VARIANT,
           (unsigned)EXPERIMENT_KEM_LEVEL,
           (unsigned long)EXPERIMENT_SEED,
           boundary,
           (unsigned long)energy_start_clock,
           (unsigned long)ENERGEST_SECOND);
}
/*---------------------------------------------------------------------------*/
static void
build_ct_payload(uint16_t seq, uint32_t original_client_timestamp)
{
  uint16_t i;

  write_u16_be(&ct_buf[0], seq);
  ct_buf[2] = EXPERIMENT_CASE_INDEX;
  ct_buf[3] = MSG_TYPE_CT;
  write_u16_be(&ct_buf[4], EXPERIMENT_CT_SIZE);
  write_u32_be(&ct_buf[6], original_client_timestamp);

  for(i = HDR_LEN; i < EXPERIMENT_CT_SIZE; i++) {
    ct_buf[i] =
      (uint8_t)((0xa5 + i + seq + EXPERIMENT_CASE_INDEX) & 0xff);
  }
}
/*---------------------------------------------------------------------------*/
static void
print_server_result(void)
{
  LOG_INFO("RESULT_SERVER,role=server,variant=%u,variant_name=%s,"
           "kem=%u,kem_name=%s,seed=%lu,success=%u,end_reason=%s,"
           "energy_window_aligned=%u,pk_valid_rx=%lu,pk_duplicate_rx=%lu,"
           "pk_invalid_rx=%lu,ct_tx=%lu,last_seq=%lu,"
           "last_one_way_ticks=%lu,last_one_way_ms=%lu\n",
           (unsigned)EXPERIMENT_VARIANT,
           EXPERIMENT_VARIANT_NAME,
           (unsigned)EXPERIMENT_KEM_LEVEL,
           EXPERIMENT_KEM_NAME,
           (unsigned long)EXPERIMENT_SEED,
           (unsigned)result_success,
           result_end_reason,
           (unsigned)energy_window_aligned,
           (unsigned long)app_stats.pk_valid_rx,
           (unsigned long)app_stats.pk_duplicate_rx,
           (unsigned long)app_stats.pk_invalid_rx,
           (unsigned long)app_stats.ct_tx,
           (unsigned long)app_stats.last_seq,
           (unsigned long)app_stats.last_one_way_ticks,
           (unsigned long)ticks_to_ms(app_stats.last_one_way_ticks));

  LOG_INFO("RESULT_NET_SERVER,role=server,variant=%u,kem=%u,seed=%lu,"
           "datagram_tx=%lu,datagram_rx_complete=%lu,data_frag_tx=%lu,"
           "data_frag_rx=%lu,data_bytes_tx=%lu,data_bytes_rx=%lu,"
           "rfrag_retx_tx=%lu,rfrag_ack_tx=%lu,rfrag_ack_rx=%lu,"
           "rfrag_ack_tx_done=%lu,rfrag_ack_tx_ok=%lu,"
           "rfrag_full_ack_tx_done=%lu,rfrag_full_ack_tx_ok=%lu,"
           "rfrag_full_ack_rx=%lu,rfc4944_final_tx_done=%lu,"
           "rfc4944_final_tx_ok=%lu,idle_ack_tx=%lu,rto_expired=%lu,"
           "null_ack_rx=%lu,partial_ack_rx=%lu,ctx_create=%lu,"
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
           (unsigned long)experiment_net_stats.rfrag_ack_tx_done,
           (unsigned long)experiment_net_stats.rfrag_ack_tx_ok,
           (unsigned long)experiment_net_stats.rfrag_full_ack_tx_done,
           (unsigned long)experiment_net_stats.rfrag_full_ack_tx_ok,
           (unsigned long)experiment_net_stats.rfrag_full_ack_rx,
           (unsigned long)experiment_net_stats.rfc4944_final_frag_tx_done,
           (unsigned long)experiment_net_stats.rfc4944_final_frag_tx_ok,
           (unsigned long)experiment_net_stats.rfrag_idle_ack_tx,
           (unsigned long)experiment_net_stats.rfrag_rto_expired,
           (unsigned long)experiment_net_stats.rfrag_null_ack_rx,
           (unsigned long)experiment_net_stats.rfrag_partial_ack_rx,
           (unsigned long)experiment_net_stats.context_create,
           (unsigned long)experiment_net_stats.context_complete,
           (unsigned long)experiment_net_stats.context_timeout_incomplete,
           (unsigned long)experiment_net_stats.context_timeout_complete,
           (unsigned long)experiment_net_stats.context_alloc_fail,
           (unsigned long)experiment_net_stats.context_active,
           (unsigned long)experiment_net_stats.context_active_peak,
           (unsigned long)experiment_net_stats.context_lifetime_ticks_closed);

  LOG_INFO("EXPERIMENT COMPLETE: role=server variant=%u kem=%u success=%u\n",
           (unsigned)EXPERIMENT_VARIANT,
           (unsigned)EXPERIMENT_KEM_LEVEL,
           (unsigned)result_success);
}
/*---------------------------------------------------------------------------*/
static void
complete_server_experiment(uint8_t success, const char *reason)
{
  if(experiment_finished) {
    return;
  }

  result_success = success;
  result_end_reason = reason;
  waiting_for_protocol_end = 0;
  experiment_finished = 1;

  if(energy_started && exchange_energy.running) {
    energy_meter_stop_and_report(&exchange_energy,
                                 "server",
                                 success ? "success" : "failure",
                                 reason,
                                 (unsigned)EXPERIMENT_VARIANT,
                                 (unsigned)EXPERIMENT_KEM_LEVEL,
                                 (unsigned long)EXPERIMENT_SEED);
  } else {
    LOG_WARN("ENERGY_RESULT_SKIPPED,role=server,started=%u,running=%u\n",
             (unsigned)energy_started,
             (unsigned)exchange_energy.running);
  }

  print_server_result();
}
/*---------------------------------------------------------------------------*/
static void
udp_server_rx_callback(struct simple_udp_connection *c,
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
  uint32_t original_client_timestamp;
  uint32_t now;
  uint8_t is_duplicate;
  uint32_t alignment_error;

  (void)c;
  (void)sender_port;
  (void)receiver_addr;
  (void)receiver_port;

  if(experiment_finished) {
    return;
  }

  if(datalen < HDR_LEN) {
    app_stats.pk_invalid_rx++;
    LOG_WARN("RX paquet trop court: datalen=%u\n", datalen);
    return;
  }

  seq = read_u16_be(&data[0]);
  case_idx = data[2];
  msg_type = data[3];
  declared_len = read_u16_be(&data[4]);
  original_client_timestamp = read_u32_be(&data[6]);

  if(msg_type != MSG_TYPE_PK ||
     case_idx != EXPERIMENT_CASE_INDEX ||
     declared_len != datalen ||
     datalen != EXPERIMENT_PK_SIZE) {
    app_stats.pk_invalid_rx++;
    LOG_WARN("RX PK invalid msg_type=%u case_idx=%u declared=%u "
             "datalen=%u expected_case=%u expected_len=%u\n",
             msg_type,
             case_idx,
             declared_len,
             datalen,
             (unsigned)EXPERIMENT_CASE_INDEX,
             (unsigned)EXPERIMENT_PK_SIZE);
    return;
  }

  /*
   * Un démarrage ici est seulement un secours. Une expérience crédible doit
   * avoir démarré Energest au T0 synchronisé avant la réception du PK.
   */
  if(!energy_started) {
    start_energy_measurement("late_fallback_on_pk_complete");
    energy_window_aligned = 0;
  }

  alignment_error =
    absolute_tick_difference(original_client_timestamp,
                             (uint32_t)energy_start_clock);

  if(alignment_error > APP_CONF_ENERGY_ALIGNMENT_TOLERANCE) {
    energy_window_aligned = 0;
    LOG_WARN("ENERGY_ALIGNMENT_WARNING,role=server,"
             "client_tx_ticks=%lu,server_start_ticks=%lu,error_ticks=%lu\n",
             (unsigned long)original_client_timestamp,
             (unsigned long)energy_start_clock,
             (unsigned long)alignment_error);
  }

  is_duplicate = seq == last_rx_seq && case_idx == last_rx_case_idx;
  now = (uint32_t)clock_time();

  app_stats.last_seq = seq;
  app_stats.last_one_way_ticks = now - original_client_timestamp;

  if(is_duplicate) {
    app_stats.pk_duplicate_rx++;

    LOG_WARN("RX PK DUPLICATE case=%s case_idx=%u seq=%u datalen=%u "
             "approx_one_way_ms=%lu from ",
             EXPERIMENT_KEM_NAME,
             (unsigned)EXPERIMENT_CASE_INDEX,
             seq,
             datalen,
             (unsigned long)ticks_to_ms(app_stats.last_one_way_ticks));
  } else {
    last_rx_seq = seq;
    last_rx_case_idx = case_idx;
    app_stats.pk_valid_rx++;

    LOG_INFO("RX PK VALID case=%s case_idx=%u seq=%u datalen=%u "
             "approx_one_way_ticks=%lu approx_one_way_ms=%lu from ",
             EXPERIMENT_KEM_NAME,
             (unsigned)EXPERIMENT_CASE_INDEX,
             seq,
             datalen,
             (unsigned long)app_stats.last_one_way_ticks,
             (unsigned long)ticks_to_ms(app_stats.last_one_way_ticks));
  }

  LOG_INFO_6ADDR(sender_addr);
  LOG_INFO_("\n");

  build_ct_payload(seq, original_client_timestamp);
  app_stats.ct_tx++;

  /*
   * Capturer les compteurs avant l'envoi pour distinguer la fin de CE CT
   * d'une éventuelle transmission précédente.
   */
  rfrag_full_ack_rx_before_ct =
    experiment_net_stats.rfrag_full_ack_rx;

  rfc4944_final_tx_done_before_ct =
    experiment_net_stats.rfc4944_final_frag_tx_done;

  rfc4944_final_tx_ok_before_ct =
    experiment_net_stats.rfc4944_final_frag_tx_ok;

  LOG_INFO("TX CT case=%s case_idx=%u seq=%u declared=%u datalen=%u "
           "ct_attempt=%lu to ",
           EXPERIMENT_KEM_NAME,
           (unsigned)EXPERIMENT_CASE_INDEX,
           seq,
           (unsigned)EXPERIMENT_CT_SIZE,
           (unsigned)EXPERIMENT_CT_SIZE,
           (unsigned long)app_stats.ct_tx);
  LOG_INFO_6ADDR(sender_addr);
  LOG_INFO_("\n");

  simple_udp_sendto(&udp_conn,
                    ct_buf,
                    EXPERIMENT_CT_SIZE,
                    sender_addr);

  waiting_for_protocol_end = 1;
  protocol_end_deadline =
    clock_time() + APP_CONF_SERVER_PROTOCOL_END_TIMEOUT;

  process_poll(&udp_server_process);
}
/*---------------------------------------------------------------------------*/
PROCESS_THREAD(udp_server_process, ev, data)
{
  static struct etimer energy_start_timer;
  static struct etimer protocol_poll_timer;
  static struct etimer fallback_timer;

  PROCESS_BEGIN();

  memset(&app_stats, 0, sizeof(app_stats));
  memset(&exchange_energy, 0, sizeof(exchange_energy));

  energy_started = 0;
  experiment_finished = 0;
  waiting_for_protocol_end = 0;
  result_success = 0;
  energy_window_aligned = 1;
  energy_start_clock = 0;
  protocol_end_deadline = 0;

  rfrag_full_ack_rx_before_ct = 0;
  rfc4944_final_tx_done_before_ct = 0;
  rfc4944_final_tx_ok_before_ct = 0;

  last_rx_seq = UINT16_MAX;
  last_rx_case_idx = 0xff;
  result_end_reason = "not_finished";

  experiment_net_stats_reset();

  NETSTACK_ROUTING.root_start();

  simple_udp_register(&udp_conn,
                      UDP_SERVER_PORT,
                      NULL,
                      UDP_CLIENT_PORT,
                      udp_server_rx_callback);

  LOG_INFO("EXPERIMENT_CONFIG,role=server,variant=%u,variant_name=%s,"
           "kem=%u,kem_name=%s,seed=%lu,rfrag=%u,ack_first=%u,"
           "idle_ack=%u,app_retry=%u,energest=%u,"
           "energy_boundary=sync_T0,exchange_T0_ticks=%lu\n",
           (unsigned)EXPERIMENT_VARIANT,
           EXPERIMENT_VARIANT_NAME,
           (unsigned)EXPERIMENT_KEM_LEVEL,
           EXPERIMENT_KEM_NAME,
           (unsigned long)EXPERIMENT_SEED,
           (unsigned)EXPERIMENT_RFRAG_ENABLED,
           (unsigned)EXPERIMENT_RFRAG_ACK_FIRST,
           (unsigned)EXPERIMENT_RFRAG_ACK_IDLE_ENABLED,
           (unsigned)EXPERIMENT_APP_RETRY_ENABLED,
           (unsigned)ENERGEST_CONF_ON,
           (unsigned long)APP_CONF_EXCHANGE_T0);

  LOG_INFO("Serveur/root initialise: case=%s case_idx=%u PK=%u CT=%u\n",
           EXPERIMENT_KEM_NAME,
           (unsigned)EXPERIMENT_CASE_INDEX,
           (unsigned)EXPERIMENT_PK_SIZE,
           (unsigned)EXPERIMENT_CT_SIZE);

  etimer_set(&energy_start_timer, APP_CONF_EXCHANGE_T0);
  etimer_set(&fallback_timer, APP_CONF_SERVER_RESULT_FALLBACK_DELAY);

  while(1) {
    PROCESS_WAIT_EVENT();

    if(ev == PROCESS_EVENT_TIMER &&
       data == &energy_start_timer &&
       !energy_started) {
      start_energy_measurement("sync_T0_before_PK");
    }

    if(ev == PROCESS_EVENT_POLL &&
       waiting_for_protocol_end &&
       !experiment_finished) {
      etimer_set(&protocol_poll_timer, 1);
    }

    if(ev == PROCESS_EVENT_TIMER &&
       data == &protocol_poll_timer &&
       waiting_for_protocol_end &&
       !experiment_finished) {

#if EXPERIMENT_RFRAG_ENABLED
      /*
       * Fin sémantique exacte : le serveur a reçu le FULL ACK du CT.
       */
      if(experiment_net_stats.rfrag_full_ack_rx >
         rfrag_full_ack_rx_before_ct) {
        complete_server_experiment(1, "rfrag_full_ack_received");
      } else
#else
      /*
       * B1 : fin radio exacte du dernier fragment RFC 4944 du CT.
       */
      if(experiment_net_stats.rfc4944_final_frag_tx_done >
         rfc4944_final_tx_done_before_ct) {
        uint8_t mac_ok =
          experiment_net_stats.rfc4944_final_frag_tx_ok >
          rfc4944_final_tx_ok_before_ct;

        complete_server_experiment(mac_ok,
          mac_ok ?
          "rfc4944_final_fragment_mac_ok" :
          "rfc4944_final_fragment_mac_failed");
      } else
#endif
      if(!CLOCK_LT(clock_time(), protocol_end_deadline)) {
        complete_server_experiment(0, "protocol_end_timeout");
      } else {
        etimer_set(&protocol_poll_timer, 1);
      }
    }

    if(ev == PROCESS_EVENT_TIMER &&
       data == &fallback_timer &&
       !experiment_finished) {
      complete_server_experiment(0, "server_fallback_no_completion");
    }
  }

  PROCESS_END();
}