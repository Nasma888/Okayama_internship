/*
 * À fusionner dans ton project-conf.h existant.
 * Ne remplace pas tes paramètres de variante/fragmentation.
 */

#define ENERGEST_CONF_ON 1

/* Modèle électrique utilisé pour convertir les ticks en µJ. */
#define APP_CONF_ENERGY_MODEL_NAME "Z1_CC2420_APPROX"
#define APP_CONF_ENERGY_VOLTAGE_MV 3000ULL
#define APP_CONF_ENERGY_CPU_UA 4300ULL
#define APP_CONF_ENERGY_LPM_UA 1ULL
#define APP_CONF_ENERGY_DEEP_LPM_UA 1ULL
#define APP_CONF_ENERGY_RADIO_TX_UA 17400ULL
#define APP_CONF_ENERGY_RADIO_RX_UA 18800ULL

/*
 * Le serveur corrigé utilise ce T0.
 * Pour une comparaison stricte client/serveur, utilise la même échéance
 * pour le premier essai client.
 */
#define APP_CONF_EXCHANGE_T0 (20 * CLOCK_SECOND)
