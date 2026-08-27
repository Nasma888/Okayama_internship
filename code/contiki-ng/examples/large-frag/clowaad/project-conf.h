#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

/* ---------------------------------------------------------------------
 * 6LoWPAN / IPv6 buffer configuration
 * Config validée : UIP_CONF_BUFFER_SIZE 1750 couvre les 5 payloads
 * ML-KEM (max 1568 o) avec marge. QUEUEBUF_CONF_NUM 32 couvre le nombre
 * de fragments attendu (n_max ~17-30 selon inclusion sécurité MAC).
 * --------------------------------------------------------------------- */
#define UIP_CONF_BUFFER_SIZE          1750
#define QUEUEBUF_CONF_NUM             32
#define SICSLOWPAN_CONF_FRAG          1

/* Pas de retransmission MAC pour cette baseline : on veut compter le
 * nombre de fragments "bruts" produits par sicslowpan_output(), sans
 * bruit introduit par CSMA qui réémettrait certaines trames. */
#define CSMA_CONF_MAX_FRAME_RETRIES   0

/* ---------------------------------------------------------------------
 * Application
 * --------------------------------------------------------------------- */
#define UDP_CLIENT_PORT  8765
#define UDP_SERVER_PORT  5678

/* ---------------------------------------------------------------------
 * Logging : on remonte le niveau de sicslowpan pour voir chaque
 * fragment émis/reçu dans la console Cooja, en plus du Radio Logger.
 * --------------------------------------------------------------------- */
#undef  LOG_CONF_LEVEL_6LOWPAN
#define LOG_CONF_LEVEL_6LOWPAN        LOG_LEVEL_DBG

#undef  LOG_CONF_LEVEL_IPV6
#define LOG_CONF_LEVEL_IPV6           LOG_LEVEL_INFO

#undef  LOG_CONF_LEVEL_RPL
#define LOG_CONF_LEVEL_RPL            LOG_LEVEL_INFO

#endif /* PROJECT_CONF_H_ */