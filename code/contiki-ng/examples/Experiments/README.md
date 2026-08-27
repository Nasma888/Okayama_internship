# Etapes 2 et 3 — instrumentation et un seul niveau ML-KEM

## Fichiers

- `client.c` : un seul échange PK/CT, compteurs applicatifs, lignes `RESULT` et `RESULT_NET`.
- `server.c` : accepte uniquement le niveau sélectionné, compte les PK/CT et produit `RESULT_SERVER` et `RESULT_NET_SERVER`.
- `experiment-variants.h` : sélection de B1/B2/V3/V4/V5 et de ML-KEM-512/768/1024.
- `experiment-stats.h` : structure commune des compteurs réseau locaux.
- `project-conf.h` : exemple de configuration d'une cellule expérimentale.
- `sicslowpan.c` : version instrumentée pour RFC4944 et RFRAG.
- `sicslowpan-step23.patch` : différences par rapport à la version de l'étape 1.

## Sélection d'une cellule

Dans `project-conf.h` :

```c
#define EXPERIMENT_VARIANT 4       /* 1, 2, 3, 4 ou 5 */
#define EXPERIMENT_KEM_LEVEL 512   /* 512, 768 ou 1024 */
#define EXPERIMENT_SEED 0
```

V5 utilise :

```c
#define APP_CONF_MAX_RETRIES 3
```

Les autres variantes forcent automatiquement le nombre de reprises applicatives à zéro.

## Installation

Dans le dossier de l'exemple Contiki-NG :

```bash
cp client.c server.c experiment-variants.h experiment-stats.h project-conf.h \
   "$CONTIKI_NG/examples/large-frag/mon-experience/"
```

Remplacer le fichier réseau :

```bash
cp sicslowpan.c "$CONTIKI_NG/os/net/ipv6/sicslowpan.c"
```

Puis recompiler :

```bash
make clean TARGET=cooja
rm -rf build
make -j"$(nproc)" client.cooja server.cooja TARGET=cooja
```

## Lignes structurées

Le client produit exactement une ligne canonique :

```text
RESULT,role=client,...
```

Puis une ligne réseau locale :

```text
RESULT_NET,role=client,...
```

Le serveur produit, après le délai de stabilisation :

```text
RESULT_SERVER,role=server,...
RESULT_NET_SERVER,role=server,...
```

Lorsqu'il y a plusieurs reprises V5, retenir la dernière ligne serveur, c'est-à-dire celle ayant la plus grande valeur `ct_tx`.

## Sens des principaux compteurs

- `data_frag_tx/rx` : trames de fragmentation RFC4944 ou RFRAG uniquement.
- `rfrag_retx_tx` : fragments RFRAG retransmis, y compris après redémarrage complet RFRAG.
- `rfrag_ack_tx/rx` : ACK sémantiques RFRAG, pas les ACK MAC 802.15.4.
- `idle_ack_tx` : bitmap envoyé par le timeout d'inactivité, uniquement V4.
- `rto_expired` : expirations du timer ARQ côté émetteur RFRAG.
- `ctx_*` : création, complétion, expiration et occupation des contextes RX RFRAG.
- `app_attempts/app_retries` : envois complets du PK au niveau applicatif, principalement V5.

## Vérification minimale sans perte

- B1 : `rfrag_ack_tx=0`, `app_attempts=1`.
- B2 : ACK final seulement.
- V3 : ACK du premier fragment + ACK final.
- V4 : même comportement nominal que V3 et `idle_ack_tx=0` sans perte.
- V5 : `app_retries=0` sans perte ; les reprises ne s'activent qu'après `CT_TIMEOUT`.

# Etape 4 — instrumentation Energest

## Fichiers a placer dans le dossier de l'experience

- `client.c` ou `client_repet.c` (les deux contenus sont identiques)
- `server.c`
- `experiment-energy.h`
- `experiment-stats.h`
- `experiment-variants.h`
- `project-conf.h`

Le `sicslowpan.c` instrumente des etapes 2–3 reste inchangé pour cette étape.

## Makefile

Pour un projet qui utilise `client_repet.c` :

```makefile
CONTIKI_PROJECT = server client_repet

all: $(CONTIKI_PROJECT)

MAKE_MAC = MAKE_MAC_CSMA
MAKE_NET = MAKE_NET_IPV6
MAKE_ROUTING = MAKE_ROUTING_RPL_LITE

CFLAGS += -DPROJECT_CONF_PATH=\"project-conf.h\"

CONTIKI = ../../..
include $(CONTIKI)/Makefile.include
```

Aucun `MODULES += os/services/simple-energest` n'est nécessaire : le code lit
l'API Energest directement. `project-conf.h` active le module avec :

```c
#define ENERGEST_CONF_ON 1
```

## Fenêtre de mesure

Les deux motes prennent leur snapshot de départ une seconde avant l'envoi
planifié du PK :

```c
#define APP_CONF_ENERGY_LEAD_TIME CLOCK_SECOND
```

Cette seconde garantit que le serveur a pris son snapshot avant la réception
du premier fragment. Elle est constante pour toutes les variantes.

### Client

Le client produit trois phases :

- `decision` : début de la fenêtre jusqu'à la réception du CT valide ou à la
  décision d'échec ;
- `tail` : décision jusqu'au snapshot final, afin d'isoler les ARQ/contextes
  encore actifs pendant `APP_CONF_RESULT_SETTLE_DELAY` ;
- `settled` : fenêtre complète, à utiliser pour le coût local final du client.

### Serveur

Le serveur produit :

- `pk_complete` : début de la fenêtre jusqu'à la livraison du premier PK
  complet à l'application ;
- `after_pk` : réception du PK complet jusqu'au snapshot final ;
- `settled` : fenêtre complète, à utiliser pour le coût local final du serveur.

Si plusieurs `RESULT_SERVER` sont produits en V5, retenir le plus grand
`report_index`. Un timer de repli produit aussi un résultat lorsqu'aucun PK
complet n'arrive au serveur.

## Ligne RESULT_ENERGY

Exemple :

```text
RESULT_ENERGY,role=client,phase=settled,...,
cpu_ticks=...,lpm_ticks=...,deep_lpm_ticks=...,
tx_ticks=...,rx_ticks=...,total_charge_uC=...,total_energy_uJ=...
```

La mesure primaire est constituée des ticks Energest. Les valeurs `uC` et `uJ`
sont des estimations calculées avec le modèle défini dans `project-conf.h`.

## Modèle électrique par défaut

Le modèle par défaut reprend les valeurs Zolertia Z1 utilisées comme exemple
dans la documentation Contiki-NG :

```c
#define APP_CONF_ENERGY_MODEL_NAME "Z1_DATASHEET_APPROX"
#define APP_CONF_ENERGY_VOLTAGE_MV 3000UL
#define APP_CONF_ENERGY_CPU_UA 10000UL
#define APP_CONF_ENERGY_LPM_UA 23UL
#define APP_CONF_ENERGY_DEEP_LPM_UA 23UL
#define APP_CONF_ENERGY_TX_UA 17400UL
#define APP_CONF_ENERGY_RX_UA 18800UL
```

Le mote Cooja générique n'est pas physiquement un Z1. Pour le papier, conserver
les ticks comme données brutes et remplacer ces constantes par le modèle du
matériel réellement revendiqué.

## Energie totale d'un échange réussi

Pour une simulation réussie, utiliser les lignes `phase=settled` :

```text
E_exchange_uJ = E_client_settled_uJ + E_server_settled_uJ
```

De même pour la charge :

```text
Q_exchange_uC = Q_client_settled_uC + Q_server_settled_uC
```

Les deux fins de fenêtre sont locales : le serveur mesure après la stabilisation
de son dernier CT, le client après sa décision. Pour les échanges réussis, cet
écart est faible et la comparaison reste appariée. Pour une analyse stricte des
échecs jusqu'à un instant global commun, il faudra arrêter les motes avec un
contrôleur Cooja externe et lire les snapshots au même temps simulé.

## Compilation

```bash
make clean TARGET=cooja
rm -rf build
make client_repet.cooja server.cooja TARGET=cooja
```

ou, si le fichier s'appelle `client.c` :

```bash
make client.cooja server.cooja TARGET=cooja
```

## Vérifications minimales

Avec un canal parfait :

- `ENERGY_START` doit apparaître sur les deux motes ;
- `cpu_ticks`, `tx_ticks` et `rx_ticks` ne doivent pas tous être nuls ;
- `phase=settled` doit apparaître côté client et côté serveur ;
- pour le serveur, retenir la ligne ayant le plus grand `report_index` ;
- `total_energy_uJ` doit être égal à la somme des cinq composantes d'énergie.
