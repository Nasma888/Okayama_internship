#ifndef PROJECT_CONF_H_
#define PROJECT_CONF_H_

/* Changer uniquement ces trois valeurs pour une cellule experimentale. */
#define EXPERIMENT_VARIANT  5 /* 1=B1, 2=B2, 3=V3, 4=V4, 5=V5 */
#define EXPERIMENT_KEM_LEVEL 1024    /* 512, 768 ou 1024 */
#define EXPERIMENT_SEED 123456         /* remplace par la seed du run */

/* V5 seulement. Ignore par B1/B2/V3/V4. */
#define APP_CONF_MAX_RETRIES 3

#include "experiment-variants.h"

#define UIP_CONF_BUFFER_SIZE 1800
#define SICSLOWPAN_CONF_FRAG 1
#define QUEUEBUF_CONF_NUM 24

/* Campagne principale : isoler la recuperation 6LoWPAN/RFRAG. */
#define CSMA_CONF_MAX_FRAME_RETRIES 0

/* Parametres communs B2/V3/V4. */
#define SICSLOWPAN_CONF_RFRAG_FRAGMENT_SIZE 96
#define SICSLOWPAN_CONF_RFRAG_REASS_CONTEXTS 1
#define SICSLOWPAN_CONF_RFRAG_MAX_RETRIES 3
#define SICSLOWPAN_CONF_RFRAG_MAX_RESTARTS 3
#define SICSLOWPAN_CONF_RFRAG_INTER_FRAME_GAP 1
#define SICSLOWPAN_CONF_RFRAG_ARQ_TIMEOUT CLOCK_SECOND
#define SICSLOWPAN_CONF_RFRAG_MAX_ARQ_TIMEOUT (8 * CLOCK_SECOND)
#define SICSLOWPAN_CONF_RFRAG_ACK_IDLE_TIMEOUT (CLOCK_SECOND / 2)
#define SICSLOWPAN_CONF_RFRAG_REASS_TIMEOUT (20 * CLOCK_SECOND)
#define SICSLOWPAN_CONF_RFRAG_COMPLETED_CONTEXT_TIMEOUT (10 * CLOCK_SECOND)

/* Les pertes forcees restent desactivees pendant la campagne statistique. */
#define SICSLOWPAN_CONF_RFRAG_TEST_ENABLE 0

/* Parametres applicatifs. */
#define APP_CONF_START_DELAY (15 * CLOCK_SECOND)
#define APP_CONF_CT_TIMEOUT (30 * CLOCK_SECOND)
#define APP_CONF_ROUTE_RETRY_DELAY (5 * CLOCK_SECOND)
/* Laisse les retransmissions RFRAG/ACK se terminer avant le snapshot RESULT. */
#define APP_CONF_RESULT_SETTLE_DELAY (10 * CLOCK_SECOND)

/* ------------------------------------------------------------------------- */
/* Mesure Energest                                                            */
/* ------------------------------------------------------------------------- */
#define ENERGEST_CONF_ON 1

/*
 * La fenetre Energest commence une seconde avant l'envoi planifie du PK sur
 * les deux motes. Cela garantit que le serveur a pris son snapshot avant le
 * premier fragment. Cette seconde est identique dans toutes les variantes.
 */
#define APP_CONF_ENERGY_LEAD_TIME CLOCK_SECOND

/*
 * Modele d'estimation par defaut : valeurs Zolertia Z1 de la documentation
 * Contiki-NG. Les ticks restent la mesure primaire. Adapte ces courants si ton
 * article represente un autre materiel.
 */
#define APP_CONF_ENERGY_MODEL_NAME "Z1_DATASHEET_APPROX"
#define APP_CONF_ENERGY_VOLTAGE_MV 3000UL
#define APP_CONF_ENERGY_CPU_UA 10000UL
#define APP_CONF_ENERGY_LPM_UA 23UL
#define APP_CONF_ENERGY_DEEP_LPM_UA 23UL
#define APP_CONF_ENERGY_TX_UA 17400UL
#define APP_CONF_ENERGY_RX_UA 18800UL


#define LOG_CONF_LEVEL_6LOWPAN LOG_LEVEL_INFO
#endif /* PROJECT_CONF_H_ */
