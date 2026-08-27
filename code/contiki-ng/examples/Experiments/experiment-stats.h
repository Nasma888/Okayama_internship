#ifndef EXPERIMENT_STATS_H_
#define EXPERIMENT_STATS_H_

#include <stdint.h>
#include <string.h>

#define EXPERIMENT_STATS_API_VERSION 2

/*
 * Compteurs locaux à un mote. Le client et le serveur définissent chacun
 * leur instance, car ils sont compilés dans deux firmwares distincts.
 */
typedef struct {
  uint32_t datagram_tx_start;
  uint32_t datagram_rx_complete;

  uint32_t data_frag_tx;
  uint32_t data_frag_rx;
  uint32_t lowpan_data_bytes_tx;
  uint32_t lowpan_data_bytes_rx;

  uint32_t rfrag_retx_tx;
  uint32_t rfrag_ack_tx;
  uint32_t rfrag_ack_rx;
  uint32_t rfrag_ack_bytes_tx;
  uint32_t rfrag_ack_bytes_rx;

  /* Fin MAC des ACK RFRAG. */
  uint32_t rfrag_ack_tx_done;
  uint32_t rfrag_ack_tx_ok;
  uint32_t rfrag_full_ack_tx_done;
  uint32_t rfrag_full_ack_tx_ok;

  /* Fin MAC du dernier fragment RFC 4944. */
  uint32_t rfc4944_final_frag_tx_done;
  uint32_t rfc4944_final_frag_tx_ok;

  uint32_t rfrag_idle_ack_tx;
  uint32_t rfrag_rto_expired;
  uint32_t rfrag_null_ack_rx;
  uint32_t rfrag_partial_ack_rx;
  uint32_t rfrag_full_ack_rx;

  uint32_t context_create;
  uint32_t context_complete;
  uint32_t context_timeout_incomplete;
  uint32_t context_timeout_complete;
  uint32_t context_alloc_fail;
  uint32_t context_active;
  uint32_t context_active_peak;
  uint32_t context_lifetime_ticks_closed;
} experiment_net_stats_t;

extern experiment_net_stats_t experiment_net_stats;

/*---------------------------------------------------------------------------*/
static inline void
experiment_net_stats_reset(void)
{
  memset(&experiment_net_stats, 0, sizeof(experiment_net_stats));
}
/*---------------------------------------------------------------------------*/
static inline void
experiment_context_opened(void)
{
  experiment_net_stats.context_create++;
  experiment_net_stats.context_active++;

  if(experiment_net_stats.context_active >
     experiment_net_stats.context_active_peak) {
    experiment_net_stats.context_active_peak =
      experiment_net_stats.context_active;
  }
}
/*---------------------------------------------------------------------------*/
static inline void
experiment_context_closed(uint32_t lifetime_ticks)
{
  if(experiment_net_stats.context_active > 0) {
    experiment_net_stats.context_active--;
  }

  experiment_net_stats.context_lifetime_ticks_closed += lifetime_ticks;
}

#endif /* EXPERIMENT_STATS_H_ */