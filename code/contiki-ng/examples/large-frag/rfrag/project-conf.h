/* RFC 8931 selective fragment recovery prototype */
#define SICSLOWPAN_CONF_FRAG 1
#define SICSLOWPAN_CONF_RFRAG 1

/* 96 compressed-datagram bytes + 6-byte RFRAG header = 102 bytes. */
#define SICSLOWPAN_CONF_RFRAG_FRAGMENT_SIZE 96

/* RFC 8931 default-style fragment retry budget. */
#define SICSLOWPAN_CONF_RFRAG_MAX_RETRIES 3

/* Initial RTO and exponential-backoff ceiling. */
#define SICSLOWPAN_CONF_RFRAG_ARQ_TIMEOUT CLOCK_SECOND
#define SICSLOWPAN_CONF_RFRAG_MAX_ARQ_TIMEOUT (8 * CLOCK_SECOND)

/* One clock tick between consecutive data fragments. */
#define SICSLOWPAN_CONF_RFRAG_INTER_FRAME_GAP 1
#define LOG_CONF_LEVEL_6LOWPAN LOG_LEVEL_INFO
/* Single-hop prototype: one simultaneous receiver context. */
#define SICSLOWPAN_CONF_RFRAG_REASS_CONTEXTS 1

/* Keep or adapt this existing project setting for the largest ML-KEM case. */
#ifndef UIP_CONF_BUFFER_SIZE
#define UIP_CONF_BUFFER_SIZE 1800
#endif
