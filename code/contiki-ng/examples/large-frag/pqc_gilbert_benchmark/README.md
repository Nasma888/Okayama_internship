# Pipeline de validation du modèle Gilbert (UDGMGilbertLoss)

Benchmark formalisé pour confronter les paramètres théoriques (P_GB, P_BG)
aux mesures empiriques issues de Cooja, sur deux axes découplés :

- **Suite A** : E[L] = 10 fixé, PRR ∈ {0.99, 0.95, 0.90, 0.85, 0.80, 0.70, 0.60}
- **Suite B** : PRR = 0.85 fixé, E[L] ∈ {1, 2, 5, 10, 20, 50}
- Point de jonction (PRR=0.85, E[L]=10) dédupliqué et marqué `suite="A+B"`

**30 seeds identiques** sont utilisées pour toutes les configurations
(appariement), ce qui permet des comparaisons par paire et pas seulement
des comparaisons de moyennes indépendantes.

## Pipeline (4 étapes)

```
configs/generate_configs.py   -> configs/configs.json (12 configs x 30 seeds = 360 runs)
scripts/render_csc.py         -> generated_csc/*.csc + results/run_manifest.json
scripts/run_batch.sh          -> results/raw/*.log (exécution Cooja headless)
scripts/parse_logs.py         -> results/parsed/*.json + results/master_results.csv
```

### 1. Générer les configurations

```bash
cd configs && python3 generate_configs.py
```

Calcule P_GB, P_BG pour chaque config via les relations exactes
(`P_BG = 1/E[L]`, `P_GB = (1-PRR)/(PRR·E[L])`), vérifie l'auto-cohérence
théorique (`assert` sur le drift PRR reconstruit), et écrit `configs.json`.

### 2. Générer les fichiers .csc

```bash
python3 scripts/render_csc.py --duration-ms 1800000
```

**À adapter avant utilisation réelle** (marqué `[CONFIG_ADAPT]` dans
`templates/simulation.csc.template`) :
- chemins vers `client.c` / `server.c` (firmwares ML-KEM déjà compilés)
- topologie des motes (positions, `transmitting_range`/`interference_range`
  cohérents avec ta configuration déjà validée)
- le bloc `ScriptRunner` : le mécanisme exact de capture des logs dépend
  de comment `gilbert_log_bursts` écrit ses événements (System.out capturé
  par `-nogui`, logger Cooja dédié, ou LOG_INFO côté Contiki) — le
  placeholder fourni est un point de départ, pas une solution clé en main.
- `--duration-ms` : dimensionné pour atteindre `min_transmissions` (15 000
  par défaut, cf. §4.1 du protocole) — les configs à PRR élevé (peu de
  rafales) demanderont probablement une durée plus longue que celles à
  PRR faible pour observer un nombre de rafales statistiquement exploitable.

### 3. Exécuter le batch

```bash
export COOJA_DIR=/chemin/vers/contiki-ng/tools/cooja   # ta build patchée
./scripts/run_batch.sh --parallel 4 --suite all
```

- Reprise automatique : les runs déjà marqués `DONE` dans
  `results/run_status/` sont sautés (relance sûre après interruption).
- `--suite A`, `--suite B`, `--suite A+B` ou `--suite all` pour filtrer.
- `--dry-run` pour vérifier la liste des runs sans lancer Cooja.
- Parallélisme prudent par défaut (4) — augmente selon les cœurs
  disponibles, chaque run Cooja étant indépendant.

### 4. Parser les logs

```bash
python3 scripts/parse_logs.py --link-filter "client->server"
```

**À adapter** : `LOG_LINE_PATTERN` dans `parse_logs.py` doit correspondre
au format réel produit par `gilbert_log_bursts`. `--link-filter` est
important : vu le comportement asymétrique DATA/ACK déjà documenté
(NOACK possible même si le fragment a été reçu), ne pas mélanger les
deux chaînes de Markov (client→serveur vs serveur→client) dans une même
estimation.

Sorties :
- `results/parsed/<run_id>.json` — métriques par run, **avec la liste
  complète des longueurs de rafales** (nécessaire pour le test KS contre
  `Geometric(P_BG)` à faire dans l'étape d'analyse suivante)
- `results/master_results.csv` — une ligne par run, théorique vs
  empirique, prête à charger dans pandas/R pour :
  - IC de Wilson sur le PRR empirique
  - IC bootstrap sur E[L] empirique
  - test KS sur la distribution des rafales
  - comparaison au point de jonction (PRR=0.85, E[L]=10) entre les deux
    suites, comme vérification croisée d'auto-cohérence

## Schéma de `master_results.csv`

| colonne | description |
|---|---|
| `run_id`, `config_id`, `suite`, `seed` | identifiants |
| `prr_target`, `e_l_target` | valeurs théoriques cibles |
| `p_gb`, `p_bg` | paramètres Gilbert dérivés (théorie) |
| `prr_empirical`, `e_l_empirical` | mesures empiriques post-burn-in |
| `pi_b_theoretical`, `pi_b_empirical` | fraction de temps en état B |
| `n_events_after_burnin`, `n_bursts_observed` | tailles d'échantillon |
| `sufficient_sample` | booléen — `False` si sous `min_transmissions` |

## Mise à jour : estimation par comptage de transitions (P_GB ET P_BG)

`LOG_BURSTS` seul donne N(B) et N(B→G) (⇒ P_BG_hat = 1/E[L]_empirique),
mais **pas** N(G), donc pas moyen de calculer P_GB_hat -- la MLE standard
(Gilbert 1960) demande le nombre de fois passées dans chaque état, pas
seulement les longueurs de rafales côté BAD.

**`java_patch/UDGMGilbertLoss.patch.txt` (v2)** ajoute un compteur cumulé
de tentatives par lien, rattaché à la ligne de log de fin de rafale déjà
existante (donc pas de ligne supplémentaire, juste un champ en plus) :

```
[Gilbert] burst ended on 2->3: length=4 fragments cumulative_attempts=1583
```

Ça permet de reconstruire N(G), N(G→B) et donc P_GB_hat et PRR_hat sans
logger chaque tentative individuellement (contrairement à une v1 par
tentative, abandonnée car trop verbeuse pour ~15 000+ événements/run).

Comme précédemment, les logs `[Gilbert] ...` (SLF4J) apparaissent dans le
stdout du process `gradlew run`, déjà capturé par `run_batch.sh` dans
`results/run_status/<run_id>.stdout` -- c'est ce fichier que
`parse_logs.py` consomme.

**Avant un batch réel** : applique le patch, recompile, vérifie sur un
run court quel `link_id` (IDs de motes Cooja, ex. `2->3`) correspond à
ton lien client→serveur, puis passe-le à
`python3 scripts/parse_logs.py --link-id "X->Y"`.

Sorties par run : `prr_empirical`, `p_gb_empirical`, `p_bg_empirical`,
`e_l_empirical`, plus `burst_lengths` (liste complète, pour le test KS
contre `Geometric(P_BG)` à faire dans le script d'analyse séparé).

Le burn-in est exprimé en **nombre de rafales exclues**
(`burn_in_bursts`, défaut 5), car le compteur cumulé ne porte pas d'index
de paquet applicatif -- seulement un décompte de tentatives par lien.

## Ce qui n'est volontairement pas inclus

Ce pipeline s'arrête à la production de données structurées. L'étape
d'analyse statistique (IC de Wilson, bootstrap, test KS, critère de
conformité global) est laissée à un script séparé une fois que le format
réel des logs est confirmé sur un premier run réussi — inutile de figer
les seuils de décision avant d'avoir vu un échantillon réel de données.
