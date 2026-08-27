#ifndef EXPERIMENT_ENERGY_H_
#define EXPERIMENT_ENERGY_H_

#include "contiki.h"
#include "sys/energest.h"
#include "sys/rtimer.h"

#include <stdbool.h>
#include <stdint.h>
#include <inttypes.h>
#include <stdio.h>

/*
 * Marqueur utilisé par client.c et server.c pour détecter immédiatement
 * qu'un ancien/mauvais experiment-energy.h est inclus.
 */
#define EXPERIMENT_ENERGY_API_VERSION 2

#if !ENERGEST_CONF_ON
#error "ENERGEST_CONF_ON doit etre defini a 1 dans project-conf.h"
#endif

/*
 * Contiki-NG définit normalement ENERGEST_SECOND dans sys/energest.h.
 * Ce fallback rend le fichier robuste si une configuration de plateforme
 * personnalisée ne le définit pas explicitement.
 */
#ifndef ENERGEST_SECOND
#define ENERGEST_SECOND RTIMER_SECOND
#endif

#ifndef APP_CONF_ENERGY_MODEL_NAME
#define APP_CONF_ENERGY_MODEL_NAME "Z1_CC2420_APPROX"
#endif

#ifndef APP_CONF_ENERGY_VOLTAGE_MV
#define APP_CONF_ENERGY_VOLTAGE_MV 3000ULL
#endif

#ifndef APP_CONF_ENERGY_CPU_UA
#define APP_CONF_ENERGY_CPU_UA 4300ULL
#endif

#ifndef APP_CONF_ENERGY_LPM_UA
#define APP_CONF_ENERGY_LPM_UA 1ULL
#endif

#ifndef APP_CONF_ENERGY_DEEP_LPM_UA
#define APP_CONF_ENERGY_DEEP_LPM_UA 1ULL
#endif

#ifndef APP_CONF_ENERGY_RADIO_TX_UA
#define APP_CONF_ENERGY_RADIO_TX_UA 17400ULL
#endif

#ifndef APP_CONF_ENERGY_RADIO_RX_UA
#define APP_CONF_ENERGY_RADIO_RX_UA 18800ULL
#endif

typedef struct {
  uint64_t total;
  uint64_t cpu;
  uint64_t lpm;
  uint64_t deep_lpm;
  uint64_t tx;
  uint64_t rx;
} energy_snapshot_t;

typedef struct {
  energy_snapshot_t start;
  bool running;
} energy_meter_t;

/*---------------------------------------------------------------------------*/
static inline void
energy_take_snapshot(energy_snapshot_t *snapshot)
{
  energest_flush();

  snapshot->total = ENERGEST_GET_TOTAL_TIME();
  snapshot->cpu = energest_type_time(ENERGEST_TYPE_CPU);
  snapshot->lpm = energest_type_time(ENERGEST_TYPE_LPM);
  snapshot->deep_lpm = energest_type_time(ENERGEST_TYPE_DEEP_LPM);
  snapshot->tx = energest_type_time(ENERGEST_TYPE_TRANSMIT);
  snapshot->rx = energest_type_time(ENERGEST_TYPE_LISTEN);
}
/*---------------------------------------------------------------------------*/
static inline void
energy_meter_start(energy_meter_t *meter)
{
  energy_take_snapshot(&meter->start);
  meter->running = true;
}
/*---------------------------------------------------------------------------*/
static inline uint64_t
energy_delta_u64(uint64_t end, uint64_t start)
{
  return end >= start ? end - start : 0;
}
/*---------------------------------------------------------------------------*/
static inline uint64_t
energy_ticks_to_uj(uint64_t ticks, uint64_t current_ua)
{
  uint64_t whole_seconds;
  uint64_t remaining_ticks;
  uint64_t whole_energy_uj;
  uint64_t remaining_energy_uj;

  whole_seconds = ticks / ENERGEST_SECOND;
  remaining_ticks = ticks % ENERGEST_SECOND;

  whole_energy_uj =
    (whole_seconds * APP_CONF_ENERGY_VOLTAGE_MV * current_ua) / 1000ULL;

  remaining_energy_uj =
    (remaining_ticks * APP_CONF_ENERGY_VOLTAGE_MV * current_ua) /
    (1000ULL * ENERGEST_SECOND);

  return whole_energy_uj + remaining_energy_uj;
}
/*---------------------------------------------------------------------------*/
static inline uint64_t
energy_abs_diff_u64(uint64_t a, uint64_t b)
{
  return a >= b ? a - b : b - a;
}
/*---------------------------------------------------------------------------*/
static inline void
energy_meter_stop_and_report(energy_meter_t *meter,
                             const char *role,
                             const char *outcome,
                             const char *end_reason,
                             unsigned variant,
                             unsigned kem,
                             unsigned long seed)
{
  energy_snapshot_t end;
  uint64_t window_ticks;
  uint64_t cpu_ticks;
  uint64_t lpm_ticks;
  uint64_t deep_lpm_ticks;
  uint64_t tx_ticks;
  uint64_t rx_ticks;

  uint64_t cpu_energy_uj;
  uint64_t lpm_energy_uj;
  uint64_t deep_lpm_energy_uj;
  uint64_t tx_energy_uj;
  uint64_t rx_energy_uj;
  uint64_t total_energy_uj;

  uint64_t cpu_partition_ticks;
  unsigned cpu_partition_valid;
  unsigned radio_partition_valid;
  unsigned absolute_node_energy_valid;

  if(meter == NULL || !meter->running) {
    return;
  }

  energy_take_snapshot(&end);
  meter->running = false;

  window_ticks =
    energy_delta_u64(end.total, meter->start.total);
  cpu_ticks =
    energy_delta_u64(end.cpu, meter->start.cpu);
  lpm_ticks =
    energy_delta_u64(end.lpm, meter->start.lpm);
  deep_lpm_ticks =
    energy_delta_u64(end.deep_lpm, meter->start.deep_lpm);
  tx_ticks =
    energy_delta_u64(end.tx, meter->start.tx);
  rx_ticks =
    energy_delta_u64(end.rx, meter->start.rx);

  cpu_energy_uj =
    energy_ticks_to_uj(cpu_ticks, APP_CONF_ENERGY_CPU_UA);
  lpm_energy_uj =
    energy_ticks_to_uj(lpm_ticks, APP_CONF_ENERGY_LPM_UA);
  deep_lpm_energy_uj =
    energy_ticks_to_uj(deep_lpm_ticks, APP_CONF_ENERGY_DEEP_LPM_UA);
  tx_energy_uj =
    energy_ticks_to_uj(tx_ticks, APP_CONF_ENERGY_RADIO_TX_UA);
  rx_energy_uj =
    energy_ticks_to_uj(rx_ticks, APP_CONF_ENERGY_RADIO_RX_UA);

  total_energy_uj =
    cpu_energy_uj +
    lpm_energy_uj +
    deep_lpm_energy_uj +
    tx_energy_uj +
    rx_energy_uj;

  cpu_partition_ticks = cpu_ticks + lpm_ticks + deep_lpm_ticks;

  cpu_partition_valid =
    energy_abs_diff_u64(cpu_partition_ticks, window_ticks) <= 2ULL;

  radio_partition_valid =
    tx_ticks + rx_ticks <= window_ticks;

  /*
   * Sur un Cooja mote natif, CPU=fenêtre et LPM=0 est fréquent :
   * l'énergie radio reste comparable, mais l'énergie absolue complète
   * ne doit pas être présentée comme une mesure physique Z1.
   */
  absolute_node_energy_valid =
    cpu_partition_valid &&
    !(lpm_ticks == 0 &&
      deep_lpm_ticks == 0 &&
      cpu_ticks >= window_ticks);

  printf(
    "RESULT_ENERGY_EXCHANGE,"
    "role=%s,outcome=%s,end_reason=%s,"
    "variant=%u,kem=%u,seed=%lu,"
    "model=%s,voltage_mV=%llu,energest_second=%lu,"
    "window_ticks=%" PRIu64 ","
    "cpu_ticks=%" PRIu64 ",lpm_ticks=%" PRIu64 ","
    "deep_lpm_ticks=%" PRIu64 ",tx_ticks=%" PRIu64 ","
    "rx_ticks=%" PRIu64 ","
    "cpu_energy_uJ=%" PRIu64 ",lpm_energy_uJ=%" PRIu64 ","
    "deep_lpm_energy_uJ=%" PRIu64 ",tx_energy_uJ=%" PRIu64 ","
    "rx_energy_uJ=%" PRIu64 ",total_energy_uJ=%" PRIu64 ","
    "cpu_partition_valid=%u,radio_partition_valid=%u,"
    "absolute_node_energy_valid=%u\n",
    role,
    outcome,
    end_reason != NULL ? end_reason : "unspecified",
    variant,
    kem,
    seed,
    APP_CONF_ENERGY_MODEL_NAME,
    (unsigned long long)APP_CONF_ENERGY_VOLTAGE_MV,
    (unsigned long)ENERGEST_SECOND,
    window_ticks,
    cpu_ticks,
    lpm_ticks,
    deep_lpm_ticks,
    tx_ticks,
    rx_ticks,
    cpu_energy_uj,
    lpm_energy_uj,
    deep_lpm_energy_uj,
    tx_energy_uj,
    rx_energy_uj,
    total_energy_uj,
    cpu_partition_valid,
    radio_partition_valid,
    absolute_node_energy_valid
  );

  if(!radio_partition_valid) {
    printf(
      "ENERGY_ERROR,role=%s,radio_ticks_exceed_window=1\n",
      role
    );
  }

  if(!absolute_node_energy_valid) {
    printf(
      "ENERGY_WARNING,role=%s,cpu_sleep_not_observed=1,"
      "absolute_node_energy_valid=0,"
      "radio_energy_still_comparable=1\n",
      role
    );
  }
}

#endif /* EXPERIMENT_ENERGY_H_ */