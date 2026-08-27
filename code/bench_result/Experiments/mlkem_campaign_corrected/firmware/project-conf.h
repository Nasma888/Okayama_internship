#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

#include "run-config.h"
#include "experiment-variants.h"
#include "forced_loss_tests.h"

/* ------------------------------------------------------------------------- */
/* Contiki / 6LoWPAN configuration                                           */
/* ------------------------------------------------------------------------- */

#define UIP_CONF_BUFFER_SIZE 1800
#define SICSLOWPAN_CONF_FRAG 1
#define QUEUEBUF_CONF_NUM 24

/*
 * Disable automatic MAC retransmissions.
 * Loss recovery is evaluated at the RFRAG/application layers.
 */
#ifndef CSMA_CONF_MAX_FRAME_RETRIES
#define CSMA_CONF_MAX_FRAME_RETRIES 0
#endif

/* ------------------------------------------------------------------------- */
/* RFRAG configuration                                                       */
/* ------------------------------------------------------------------------- */

#ifndef RFRAG_FRAGMENT_SIZE
#define RFRAG_FRAGMENT_SIZE 96
#endif

#define SICSLOWPAN_CONF_RFRAG_FRAGMENT_SIZE RFRAG_FRAGMENT_SIZE

#define SICSLOWPAN_CONF_RFRAG_REASS_CONTEXTS 1

/* Maximum number of retries of an individual RFRAG fragment. */
#define SICSLOWPAN_CONF_RFRAG_MAX_RETRIES 3

/*
 * Kept for compatibility with the RFRAG implementation.
 * B2/V2 do not use full datagram restart on NULL bitmap in the
 * targeted experimental implementation.
 */
#define SICSLOWPAN_CONF_RFRAG_MAX_RESTARTS 3

/* Delay between consecutive RFRAG frames. */
#define SICSLOWPAN_CONF_RFRAG_INTER_FRAME_GAP 1

/*
 * Sender-side RFRAG ACK timeout.
 *
 * This allows B2, V1 and V2 to retransmit an X-bearing fragment when
 * the final RFRAG ACK is not received.
 */
#define SICSLOWPAN_CONF_RFRAG_ARQ_TIMEOUT CLOCK_SECOND

#define SICSLOWPAN_CONF_RFRAG_MAX_ARQ_TIMEOUT \
  (8 * CLOCK_SECOND)

/*
 * Currently unused because the four-strategy study does not enable
 * receiver-side inactivity ACKs.
 * Kept because sicslowpan.c provides the corresponding configuration.
 */
#define SICSLOWPAN_CONF_RFRAG_ACK_IDLE_TIMEOUT \
  (CLOCK_SECOND / 2)

/*
 * Lifetime of an incomplete receiver reassembly context.
 */
#define SICSLOWPAN_CONF_RFRAG_REASS_TIMEOUT \
  (20 * CLOCK_SECOND)

/*
 * Keep a completed context temporarily so that the receiver can answer
 * again if the final FULL RFRAG ACK was lost and X=1 is retransmitted.
 */
#define SICSLOWPAN_CONF_RFRAG_COMPLETED_CONTEXT_TIMEOUT \
  (10 * CLOCK_SECOND)

/* ------------------------------------------------------------------------- */
/* Application configuration                                                 */
/* ------------------------------------------------------------------------- */

/*
 * Wait for routing convergence before starting the PK/CT exchange.
 */
#define APP_CONF_START_DELAY \
  (30 * CLOCK_SECOND)

#define APP_CONF_EXCHANGE_T0 APP_CONF_START_DELAY

/*
 * Maximum time to wait for the CT corresponding to one PK attempt.
 *
 * The same timeout is used for B1, B2, V1 and V2 so that the
 * comparison is not biased by different application deadlines.
 */
#ifndef APP_CONF_CT_TIMEOUT
#define APP_CONF_CT_TIMEOUT \
  (120 * CLOCK_SECOND)
#endif

/* Delay before checking route availability again. */
#define APP_CONF_ROUTE_RETRY_DELAY \
  (5 * CLOCK_SECOND)

/*
 * Maximum time allowed for route acquisition.
 */
#define APP_CONF_ROUTE_ACQUISITION_TIMEOUT \
  (60 * CLOCK_SECOND)

/*
 * Additional time after successful application delivery to let the
 * RFRAG protocol finish its final ACK exchange and update statistics.
 */
#define APP_CONF_PROTOCOL_END_TIMEOUT \
  (10 * CLOCK_SECOND)

/* ------------------------------------------------------------------------- */
/* Global result fallbacks                                                   */
/* ------------------------------------------------------------------------- */

/*
 * V2 may perform one additional complete application-level attempt.
 * EXPERIMENT_MAX_APP_RETRIES is 0 for B1/B2/V1 and 1 for V2.
 */
#define APP_CONF_CLIENT_RESULT_FALLBACK_DELAY \
  (APP_CONF_ROUTE_ACQUISITION_TIMEOUT + \
   ((EXPERIMENT_MAX_APP_RETRIES + 1) * APP_CONF_CT_TIMEOUT) + \
   APP_CONF_PROTOCOL_END_TIMEOUT)

/*
 * Give the server slightly more time than the client before producing
 * its final fallback result.
 */
#define APP_CONF_SERVER_RESULT_FALLBACK_DELAY \
  (APP_CONF_CLIENT_RESULT_FALLBACK_DELAY + (2 * CLOCK_SECOND))

/* ------------------------------------------------------------------------- */
/* Logging                                                                   */
/* ------------------------------------------------------------------------- */

#define LOG_CONF_LEVEL_6LOWPAN LOG_LEVEL_INFO

#endif /* PROJECT_CONF_H_ */