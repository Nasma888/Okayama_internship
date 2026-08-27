#ifndef EXPERIMENT_VARIANTS_H_
#define EXPERIMENT_VARIANTS_H_

/* ------------------------------------------------------------------------- */
/* 1. Variante protocolaire                                                   */
/* ------------------------------------------------------------------------- */
#define EXPERIMENT_VARIANT_B1 1
#define EXPERIMENT_VARIANT_B2 2
#define EXPERIMENT_VARIANT_V3 3
#define EXPERIMENT_VARIANT_V4 4
#define EXPERIMENT_VARIANT_V5 5

#ifndef EXPERIMENT_VARIANT
#define EXPERIMENT_VARIANT EXPERIMENT_VARIANT_V4
#endif

#if EXPERIMENT_VARIANT == EXPERIMENT_VARIANT_B1
#define EXPERIMENT_VARIANT_NAME              "B1_RFC4944_NO_RECOVERY"
#define EXPERIMENT_RFRAG_ENABLED              0
#define EXPERIMENT_RFRAG_ACK_FIRST            0
#define EXPERIMENT_RFRAG_ACK_IDLE_ENABLED     0
#define EXPERIMENT_APP_RETRY_ENABLED          0
#elif EXPERIMENT_VARIANT == EXPERIMENT_VARIANT_B2
#define EXPERIMENT_VARIANT_NAME              "B2_RFRAG_FINAL_ACK"
#define EXPERIMENT_RFRAG_ENABLED              1
#define EXPERIMENT_RFRAG_ACK_FIRST            0
#define EXPERIMENT_RFRAG_ACK_IDLE_ENABLED     0
#define EXPERIMENT_APP_RETRY_ENABLED          0
#elif EXPERIMENT_VARIANT == EXPERIMENT_VARIANT_V3
#define EXPERIMENT_VARIANT_NAME              "V3_RFRAG_FIRST_FINAL_ACK"
#define EXPERIMENT_RFRAG_ENABLED              1
#define EXPERIMENT_RFRAG_ACK_FIRST            1
#define EXPERIMENT_RFRAG_ACK_IDLE_ENABLED     0
#define EXPERIMENT_APP_RETRY_ENABLED          0
#elif EXPERIMENT_VARIANT == EXPERIMENT_VARIANT_V4
#define EXPERIMENT_VARIANT_NAME              "V4_RFRAG_FIRST_FINAL_IDLE_ACK"
#define EXPERIMENT_RFRAG_ENABLED              1
#define EXPERIMENT_RFRAG_ACK_FIRST            1
#define EXPERIMENT_RFRAG_ACK_IDLE_ENABLED     1
#define EXPERIMENT_APP_RETRY_ENABLED          0
#elif EXPERIMENT_VARIANT == EXPERIMENT_VARIANT_V5
#define EXPERIMENT_VARIANT_NAME              "V5_RFC4944_APP_FULL_RETRY"
#define EXPERIMENT_RFRAG_ENABLED              0
#define EXPERIMENT_RFRAG_ACK_FIRST            0
#define EXPERIMENT_RFRAG_ACK_IDLE_ENABLED     0
#define EXPERIMENT_APP_RETRY_ENABLED          1
#else
#error "EXPERIMENT_VARIANT must be 1, 2, 3, 4, or 5"
#endif

#define SICSLOWPAN_CONF_RFRAG                  EXPERIMENT_RFRAG_ENABLED
#define SICSLOWPAN_CONF_RFRAG_ACK_FIRST        EXPERIMENT_RFRAG_ACK_FIRST
#define SICSLOWPAN_CONF_RFRAG_ACK_IDLE_ENABLE  EXPERIMENT_RFRAG_ACK_IDLE_ENABLED
#define APP_CONF_FULL_RETRY_ENABLE             EXPERIMENT_APP_RETRY_ENABLED

/* ------------------------------------------------------------------------- */
/* 2. Un seul niveau ML-KEM par simulation                                    */
/* ------------------------------------------------------------------------- */
#define EXPERIMENT_KEM_512  512
#define EXPERIMENT_KEM_768  768
#define EXPERIMENT_KEM_1024 1024

#ifndef EXPERIMENT_KEM_LEVEL
#define EXPERIMENT_KEM_LEVEL EXPERIMENT_KEM_512
#endif

#if EXPERIMENT_KEM_LEVEL == EXPERIMENT_KEM_512
#define EXPERIMENT_CASE_INDEX          0
#define EXPERIMENT_KEM_NAME            "MLKEM512"
#define EXPERIMENT_PK_SIZE             800
#define EXPERIMENT_CT_SIZE             768
#define EXPERIMENT_PK_FRAGS_EXPECTED   9
#define EXPERIMENT_CT_FRAGS_EXPECTED   9
#elif EXPERIMENT_KEM_LEVEL == EXPERIMENT_KEM_768
#define EXPERIMENT_CASE_INDEX          1
#define EXPERIMENT_KEM_NAME            "MLKEM768"
#define EXPERIMENT_PK_SIZE             1184
#define EXPERIMENT_CT_SIZE             1088
#define EXPERIMENT_PK_FRAGS_EXPECTED   13
#define EXPERIMENT_CT_FRAGS_EXPECTED   12
#elif EXPERIMENT_KEM_LEVEL == EXPERIMENT_KEM_1024
#define EXPERIMENT_CASE_INDEX          2
#define EXPERIMENT_KEM_NAME            "MLKEM1024"
#define EXPERIMENT_PK_SIZE             1568
#define EXPERIMENT_CT_SIZE             1568
#define EXPERIMENT_PK_FRAGS_EXPECTED   17
#define EXPERIMENT_CT_FRAGS_EXPECTED   17
#else
#error "EXPERIMENT_KEM_LEVEL must be 512, 768, or 1024"
#endif

/* ------------------------------------------------------------------------- */
/* 3. Parametres applicatifs et identification du run                         */
/* ------------------------------------------------------------------------- */
#ifndef EXPERIMENT_SEED
#define EXPERIMENT_SEED 0
#endif

#ifndef APP_CONF_MAX_RETRIES
#define APP_CONF_MAX_RETRIES 3
#endif

#if APP_CONF_FULL_RETRY_ENABLE
#define EXPERIMENT_MAX_APP_RETRIES APP_CONF_MAX_RETRIES
#else
#define EXPERIMENT_MAX_APP_RETRIES 0
#endif

#if EXPERIMENT_APP_RETRY_ENABLED && EXPERIMENT_RFRAG_ENABLED
#error "V5 application retry and RFRAG must not be enabled together"
#endif
#if EXPERIMENT_RFRAG_ACK_FIRST && !EXPERIMENT_RFRAG_ENABLED
#error "ACK_FIRST requires RFRAG"
#endif
#if EXPERIMENT_RFRAG_ACK_IDLE_ENABLED && !EXPERIMENT_RFRAG_ENABLED
#error "ACK_IDLE requires RFRAG"
#endif

#endif /* EXPERIMENT_VARIANTS_H_ */