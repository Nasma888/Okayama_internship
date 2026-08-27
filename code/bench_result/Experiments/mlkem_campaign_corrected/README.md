# Campagne ML-KEM / Cooja

Ce dossier corrige et complète la chaîne :

configuration → compilation isolée par run → Cooja → log brut → parsing → CSV → 3 classeurs

## Ce qui a été corrigé

- Les modules Python manquants ont été ajoutés.
- run_campaign.py ne vérifie plus un chemin Cooja avant de lire ses arguments.
- Chaque run reçoit son propre répertoire de compilation et son propre
  run-config.h : variante, niveau ML-KEM, seed et retries MAC ne peuvent
  plus être réutilisés silencieusement depuis un ancien binaire.
- Le XML Cooja contient deux motetype et deux mote valides, avec des
  identifiants explicites et des chemins entièrement résolus.
- Le modèle fixed et le modèle Gilbert n'injectent plus les mêmes paramètres
  XML. Pour Gilbert : p_bg = 1/E[L] et p_gb = p_bg × (1-PRR)/PRR.
- Le serveur n'arrête plus Energest immédiatement après simple_udp_sendto();
  il attend le FULL ACK RFRAG ou le callback MAC du dernier fragment RFC 4944.
- V4 possède réellement un timeout d'inactivité et un contexte long.
- Les timeouts applicatifs laissent jusqu'à 120 s aux variantes RFRAG, tandis
  que V5 conserve ses reprises complètes toutes les 30 s.
- Le parseur lit les événements RESULT_*, vérifie KEM/variante/tailles,
  conserve les événements reconnus et calcule le SHA-256 de chaque log brut.
- Le lanceur sépare la console Gradle (`cooja-console.log`) du journal
  ScriptRunner (`cooja.log`, copié depuis `tools/cooja/COOJA.testlog`). Il
  supprime le journal ScriptRunner précédent avant chaque run afin d'empêcher
  l'attribution d'un log périmé à une nouvelle configuration.
- Le client ne peut plus attendre indéfiniment une route RPL : à 60 s, il
  termine proprement avec `route_acquisition_timeout` et imprime les événements
  `RESULT_ENERGY_EXCHANGE`, `RESULT` et `RESULT_NET`.
- Le timeout interne du ScriptRunner est classé `SIMULATION_TIMEOUT`, même si
  le processus Gradle se termine avec un code nul.
- Des compteurs de fragments nuls ne signalent une instrumentation absente que
  si un trafic applicatif ou un datagramme a effectivement été observé.
- `SUCCESS` exige une terminaison client/serveur cohérente pour B1–V4. Pour V5,
  une récupération confirmée côté client après un premier résultat serveur en
  échec reste un succès si `app_retries>0`.
- Trois classeurs séparés sont produits : ML-KEM-512, 768 et 1024. Chaque
  classeur possède les feuilles README, Runs, Summary, Campaign,
  Log Evidence et QC.

## Prérequis réels

Le dépôt Contiki-NG utilisé doit déjà contenir :

1. la classe Cooja UDGMGilbertLoss;
2. le patch RFRAG qui implémente B2/V3/V4;
3. les hooks qui incrémentent experiment_net_stats dans la pile 6LoWPAN.

Le fichier experiment-stats.h fourni définit l'API et les compteurs, mais il
ne peut pas, à lui seul, instrumenter une pile Contiki-NG non patchée.

## Matrice fournie

config/experiments.csv contient 255 runs :

- 3 niveaux ML-KEM;
- 5 stratégies;
- fixed PRR = 1.00, 0.95, 0.90, 0.85, 0.50;
- Gilbert PRR = 0.95, 0.90, 0.85, 0.50;
- Gilbert E[L] = 2, 5, 10;
- une répétition par cellule pour le smoke test.

Pour une campagne statistique, régénérer la matrice avec davantage de
répétitions :

~~~bash
python3 scripts/generate_campaign.py \
  --output config/experiments.csv \
  --repetitions 30 \
  --timeout-s 240
~~~

## Exécution

Smoke test d'une cellule :

~~~bash
python3 scripts/run_campaign.py \
  --root . \
  --contiki /chemin/vers/contiki-ng \
  --kem 512 \
  --variant 1 \
  --model fixed \
  --limit 1
~~~

Campagne complète :

~~~bash
python3 scripts/run_campaign.py \
  --root . \
  --contiki /chemin/vers/contiki-ng
~~~

Reparser des logs existants sans relancer Cooja :

~~~bash
python3 scripts/run_campaign.py --root . --parse-only --force --no-excel
~~~

`--force` est requis pour remplacer les anciens `results.csv` et `status.json`.
Le reparsing reclasse les anciens runs coupés, mais ne peut pas inventer leurs
événements absents : ces runs doivent ensuite être relancés avec le firmware
corrigé.

Les logs expérimentaux bruts restent sous `runs/**/cooja.log`; la sortie de
construction/exécution Gradle reste sous `runs/**/cooja-console.log`. Le fichier
results/log_manifest.csv associe chaque log à son SHA-256.

## Preuve du parseur incluse

proof_output contient 15 logs synthétiques couvrant 3 KEM × 5 stratégies.
Ils servent uniquement à tester la génération, le parsing, le manifeste et les
classeurs. Ils portent le marqueur
PARSER_FIXTURE_SYNTHETIC,not_a_scientific_measurement=1 et ne doivent jamais
être utilisés comme résultats scientifiques.

Pour reconstruire cette preuve :

~~~bash
python3 scripts/create_parser_proof.py --output-root proof_output
~~~

## Interprétation de V5

Le résultat client est l'indicateur global de la récupération applicative. Après le
premier résultat serveur, le serveur continue à répondre aux PK dupliqués de
V5 afin que la reprise applicative reste fonctionnelle. Les métriques serveur
imprimées décrivent donc la première terminaison de CT; les tentatives
applicatives globales sont celles du client. Le parseur exige néanmoins la
présence du résultat serveur; il autorise `server_success=0` uniquement si le
client finit en succès avec au moins une retransmission applicative.
