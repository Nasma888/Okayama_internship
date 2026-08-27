#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

#define APP_PAYLOAD_SIZE 1568
#define UIP_CONF_BUFFER_SIZE 1750
#define SICSLOWPAN_CONF_FRAG 1
#define QUEUEBUF_CONF_NUM 32

#define LOG_CONF_LEVEL_IPV6 LOG_LEVEL_INFO
#define LOG_CONF_LEVEL_6LOWPAN LOG_LEVEL_INFO
#define LOG_CONF_LEVEL_MAC LOG_LEVEL_INFO
#define LOG_CONF_LEVEL_RPL LOG_LEVEL_INFO

/*
 * Try to disable MAC retransmissions.
 * Check the exact macro name in your Contiki-NG version with grep.
 */
#define CSMA_CONF_MAX_FRAME_RETRIES 0

#endif /* PROJECT_CONF_H_ */