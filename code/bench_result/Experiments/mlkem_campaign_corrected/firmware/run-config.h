#ifndef RUN_CONFIG_H_
#define RUN_CONFIG_H_

/* Fichier de secours. Chaque run réel reçoit sa propre copie générée. */
#ifndef EXPERIMENT_VARIANT
#define EXPERIMENT_VARIANT 3
#endif
#ifndef EXPERIMENT_KEM_LEVEL
#define EXPERIMENT_KEM_LEVEL 512
#endif
#ifndef EXPERIMENT_SEED
#define EXPERIMENT_SEED 0
#endif
#ifndef FORCED_LOSS_MODE
#define FORCED_LOSS_MODE 1
#endif
#ifndef FORCED_LOSS_AFTER_RADIO_CHANNEL 
#define FORCED_LOSS_AFTER_RADIO_CHANNEL 1
#endif

#endif
