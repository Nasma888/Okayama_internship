# Correctif des résultats absents sous fortes pertes

Ce correctif traite les runs qui atteignaient le timeout Cooja sans produire
`RESULT/client`, `RESULT_NET/client` ni `RESULT_ENERGY_EXCHANGE/client`.

## Cause

Si le client ne trouvait jamais de root RPL, il réarmait son timer toutes les
5 secondes sans limite. Le timeout applicatif ne commençait qu'après un premier
PK; aucun résultat n'était donc émis lorsque ce PK n'était jamais envoyé.

Le validateur interprétait ensuite à tort les compteurs absents/nuls comme une
instrumentation 6LoWPAN manquante.

## Installation sans toucher aux logs existants

~~~bash
CAMPAIGN_DIR="/home/nasma/Japon/Internship/code/bench_result/Experiments/mlkem_campaign_corrected"
FIX_ROOT="/chemin/vers/mlkem_campaign_validation_fix"

cp -a "$FIX_ROOT/config/." "$CAMPAIGN_DIR/config/"
cp -a "$FIX_ROOT/firmware/." "$CAMPAIGN_DIR/firmware/"
cp -a "$FIX_ROOT/scripts/." "$CAMPAIGN_DIR/scripts/"
cp -a "$FIX_ROOT/tests/." "$CAMPAIGN_DIR/tests/"
cp "$FIX_ROOT/README.md" "$CAMPAIGN_DIR/README.md"

cd "$CAMPAIGN_DIR"
python3 -m unittest discover -s tests -v
~~~

Le résultat attendu est `Ran 9 tests` puis `OK`.

## Smoke test exact : ML-KEM-512, B1, fixed PRR=0.50

~~~bash
CONTIKI_DIR="/chemin/absolu/vers/contiki-ng-modifie"

cd "$CAMPAIGN_DIR"

awk -F, \
  'NR==1 || ($1=="fixed" && $2==1 && $3==512 && $4==0.5)' \
  config/experiments.csv \
  > config/smoke_kem512_b1_fixed_prr050.csv

python3 scripts/run_campaign.py \
  --root "$CAMPAIGN_DIR" \
  --campaign "$CAMPAIGN_DIR/config/smoke_kem512_b1_fixed_prr050.csv" \
  --contiki "$CONTIKI_DIR" \
  --force \
  --no-excel
~~~

Vérifier le journal :

~~~bash
RUN_DIR="$CAMPAIGN_DIR/runs/kem_512/variant_1/fixed_prr_0.50/seed_100005_rep_01"

rg -n \
  "RESULT_ENERGY_EXCHANGE,role=client|RESULT,role=client|RESULT_NET,role=client|EXPERIMENT_DONE_CLIENT|EXPERIMENT_DONE_BOTH" \
  "$RUN_DIR/cooja.log"

cat "$RUN_DIR/status.json"
cat "$RUN_DIR/results.csv"
~~~

Même si le réseau échoue, les trois événements client doivent être présents.
Un échec propre doit être classé `FAILED_PROTOCOL`, pas `INCOMPLETE` ni
`SIMULATION_TIMEOUT`.

## Relancer les 17 cellules B1 / ML-KEM-512

~~~bash
cd "$CAMPAIGN_DIR"

python3 scripts/run_campaign.py \
  --root "$CAMPAIGN_DIR" \
  --campaign "$CAMPAIGN_DIR/config/experiments.csv" \
  --contiki "$CONTIKI_DIR" \
  --kem 512 \
  --variant 1 \
  --force \
  --no-excel
~~~

Puis vérifier les anomalies restantes :

~~~bash
python3 scripts/aggregate_results.py --root "$CAMPAIGN_DIR"
python3 scripts/validate_results.py --root "$CAMPAIGN_DIR"

rg -n \
  "kem512_v1_.*(événement requis absent|instrumentation stack absente)" \
  "$CAMPAIGN_DIR/results/validation_issues.csv" \
  || echo "B1 ML-KEM-512: aucun warning de capture/termination"
~~~

Un warning d'instrumentation est désormais émis uniquement si un datagramme a
été tenté alors que tous les compteurs de fragments restent nuls. Dans ce cas,
le patch 6LoWPAN du dépôt Contiki-NG doit réellement être réparé; le validateur
ne masque pas cette anomalie.
