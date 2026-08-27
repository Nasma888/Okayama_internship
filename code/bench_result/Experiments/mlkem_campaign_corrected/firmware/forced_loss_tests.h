#ifndef FORCED_LOSS_TESTS_H_
#define FORCED_LOSS_TESTS_H_

/*
 * Controlled one-shot injection for targeted study.
 *
 * Only two experimental forced-loss scenarios:
 *
 *   1 -> drop first PK fragment
 *   2 -> drop first AND last PK fragments
 *
 * Loss is applied on the receiver side AFTER the radio channel,
 * so Gilbert / Fixed radio losses remain active normally.
 */

#define FORCED_LOSS_NONE                     0
#define FORCED_LOSS_FIRST_FRAGMENT           1
#define FORCED_LOSS_FIRST_AND_LAST_FRAGMENT  2


#ifndef FORCED_LOSS_MODE
#define FORCED_LOSS_MODE FORCED_LOSS_NONE
#endif


/*
 * Forced loss must occur after the configured radio channel.
 */
#ifndef FORCED_LOSS_AFTER_RADIO_CHANNEL
#define FORCED_LOSS_AFTER_RADIO_CHANNEL 1
#endif


/*
 * Sequence number of the last expected PK fragment.
 */
#ifndef FORCED_LOSS_LAST_SEQUENCE
#define FORCED_LOSS_LAST_SEQUENCE \
  (EXPERIMENT_PK_FRAGS_EXPECTED - 1)
#endif


/* ------------------------------------------------------------------------- */
/* Forced-loss scenario                                                      */
/* ------------------------------------------------------------------------- */

#if FORCED_LOSS_MODE == FORCED_LOSS_NONE

#define EXPERIMENT_FORCED_LOSS_NAME "none"

#elif FORCED_LOSS_MODE == FORCED_LOSS_FIRST_FRAGMENT

#define EXPERIMENT_FORCED_LOSS_NAME "first_fragment"

/*
 * Fragment sequence 0.
 */
#define SICSLOWPAN_CONF_RFRAG_TEST_DROP_DATA_BITMAP \
  0x80000000UL

#elif FORCED_LOSS_MODE == FORCED_LOSS_FIRST_AND_LAST_FRAGMENT

#define EXPERIMENT_FORCED_LOSS_NAME "first_and_last_fragment"

/*
 * Sequence 0:
 *     0x80000000
 *
 * Last sequence:
 *     0x80000000 >> FORCED_LOSS_LAST_SEQUENCE
 *
 * Both fragments are therefore dropped exactly once.
 */
#define SICSLOWPAN_CONF_RFRAG_TEST_DROP_DATA_BITMAP \
  (0x80000000UL | \
   (0x80000000UL >> FORCED_LOSS_LAST_SEQUENCE))

#else

#error "FORCED_LOSS_MODE must be 0, 1, or 2"

#endif


/* ------------------------------------------------------------------------- */
/* Injection layer                                                           */
/* ------------------------------------------------------------------------- */

#if FORCED_LOSS_MODE == FORCED_LOSS_NONE

#define EXPERIMENT_FORCED_LOSS_LAYER "none"
#define SICSLOWPAN_CONF_RFRAG_TEST_ENABLE 0

#else

#if !FORCED_LOSS_AFTER_RADIO_CHANNEL
#error "Forced loss must be applied after the configured radio channel"
#endif

#define EXPERIMENT_FORCED_LOSS_LAYER \
  "receiver_after_channel"

#define SICSLOWPAN_CONF_RFRAG_TEST_ENABLE 1

#endif


/* ------------------------------------------------------------------------- */
/* Allowed variants                                                          */
/* ------------------------------------------------------------------------- */

#if FORCED_LOSS_MODE != FORCED_LOSS_NONE && \
    EXPERIMENT_VARIANT != EXPERIMENT_VARIANT_V1 && \
    EXPERIMENT_VARIANT != EXPERIMENT_VARIANT_V2

#error "Forced PK losses are only valid for V1 and V2, B2"

#endif


/* ------------------------------------------------------------------------- */
/* Runtime target                                                            */
/* ------------------------------------------------------------------------- */

#if FORCED_LOSS_MODE != FORCED_LOSS_NONE

#ifndef FORCED_LOSS_SOURCE_ID
#define FORCED_LOSS_SOURCE_ID 2
#endif

#ifndef FORCED_LOSS_TARGET_TAG
#define FORCED_LOSS_TARGET_TAG 0
#endif

#define SICSLOWPAN_CONF_RFRAG_TEST_DATA_TARGET_NODE_ID \
  FORCED_LOSS_SOURCE_ID

#define SICSLOWPAN_CONF_RFRAG_TEST_DATA_TARGET_TAG \
  FORCED_LOSS_TARGET_TAG


/*
 * Each selected fragment sequence can be suppressed only once
 * during the whole simulation.
 */
#define SICSLOWPAN_CONF_RFRAG_TEST_DROP_DATA_MAX_PER_SEQ 1

#define SICSLOWPAN_CONF_RFRAG_TEST_EXPECTED_FRAGMENT_COUNT \
  EXPERIMENT_PK_FRAGS_EXPECTED


/*
 * We inject DATA fragment losses only.
 * ACKs are never artificially removed.
 */
#define SICSLOWPAN_CONF_RFRAG_TEST_DROP_ACK_TYPE 0
#define SICSLOWPAN_CONF_RFRAG_TEST_DROP_ACK_COUNT 0

#define SICSLOWPAN_CONF_RFRAG_TEST_REPORT_MAC_NOACK 0

#endif


/*
 * No forced RFC4944 loss mechanism.
 */
#define SICSLOWPAN_CONF_RFC4944_TEST_ENABLE 0


#endif /* FORCED_LOSS_TESTS_H_ */