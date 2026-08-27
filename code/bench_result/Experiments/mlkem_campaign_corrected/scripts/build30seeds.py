#!/usr/bin/env python3
"""
build_30seed_campaign.py
=========================

Étend un fichier de campagne (experiments.csv) à N seeds PARTAGÉES par
sous-cas (combinaison model/variant/kem_level/target_prr/mean_bad_burst),
au lieu d'une seule seed par configuration.

Pourquoi des seeds *partagées* (et non 30 seeds différentes par config) :
pour un test de Wilcoxon apparié entre H_N (RFC4944) et T_N (FragmentR),
il faut que chaque paire de variantes, à une condition de canal donnée,
voie exactement la même trace de pertes. Le seed n°k doit donc être
identique pour toutes les configs (tous variants confondus) à un rang k
donné — sinon la comparaison appariée n'a plus de sens.

Le script :
  1. lit experiments.csv (une ligne = une configuration, colonne 'seed'
     ignorée car remplacée, colonne 'repetition' ignorée car remplacée)
  2. déduplique sur les colonnes qui définissent un sous-cas
     (model, variant, kem_level, target_prr, mean_bad_burst, p_gb, p_bg)
  3. génère un pool fixe de N seeds déterministes
  4. produit le produit cartésien (sous-cas x seeds), avec repetition = 1..N
  5. écrit un nouveau CSV, prêt à être passé à run_campaign.py

Usage
-----
    python3 build_30seed_campaign.py \
        --input config/experiments.csv \
        --output config/experiments_30seeds.csv \
        --n-seeds 30 \
        --seed-base 200001

Le pool de seeds par défaut (200001..200030) est volontairement disjoint
de la plage déjà utilisée par la campagne à 1 run/config (100001..100255)
pour éviter toute confusion entre les deux jeux de données.
"""

from __future__ import annotations

import argparse
from pathlib import Path

import pandas as pd

# Colonnes qui définissent un "sous-cas" (une condition expérimentale unique,
# indépendamment du seed/de la répétition).
SUBCASE_COLUMNS = [
    "model", "variant", "kem_level", "target_prr", "mean_bad_burst", "p_gb", "p_bg",
]


def build_seed_pool(n_seeds: int, seed_base: int) -> list[int]:
    """Pool de seeds déterministes, reproductible d'une exécution à l'autre."""
    return [seed_base + i for i in range(n_seeds)]


def expand_campaign(df: pd.DataFrame, seeds: list[int]) -> pd.DataFrame:
    # colonnes de configuration à conserver (tout sauf seed/repetition,
    # qui sont recalculées) ; on garde l'ordre original des colonnes.
    original_columns = df.columns.tolist()
    config_columns = [c for c in original_columns if c not in ("seed", "repetition")]

    # Un sous-cas peut apparaître une seule fois dans le fichier d'origine
    # (c'est le cas ici : 255 lignes = 255 sous-cas uniques). On vérifie
    # cette hypothèse et on prévient si ce n'est pas le cas.
    n_subcases_declared = len(df)
    n_subcases_unique = df.drop_duplicates(subset=SUBCASE_COLUMNS).shape[0]
    if n_subcases_declared != n_subcases_unique:
        print(f"  [!] Attention : {n_subcases_declared} lignes mais seulement "
              f"{n_subcases_unique} sous-cas uniques sur {SUBCASE_COLUMNS} — "
              f"vérifier qu'il n'y a pas de doublons involontaires.")

    base = df[config_columns].reset_index(drop=True)

    rows = []
    for _, config_row in base.iterrows():
        for rep_idx, seed in enumerate(seeds, start=1):
            row = config_row.to_dict()
            row["seed"] = seed
            row["repetition"] = rep_idx
            rows.append(row)

    expanded = pd.DataFrame(rows, columns=config_columns + ["seed", "repetition"])
    # on remet les colonnes dans l'ordre original du fichier source
    expanded = expanded[original_columns]
    return expanded


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument("--input", type=Path, required=True,
                         help="Fichier experiments.csv source (1 ligne = 1 sous-cas)")
    parser.add_argument("--output", type=Path, required=True,
                         help="Fichier CSV de sortie (1 ligne = 1 sous-cas x 1 seed)")
    parser.add_argument("--n-seeds", type=int, default=30,
                         help="Nombre de seeds partagées par sous-cas (défaut : 30)")
    parser.add_argument("--seed-base", type=int, default=200001,
                         help="Premier seed du pool ; les suivants sont seed_base+1, +2... (défaut : 200001)")
    args = parser.parse_args()

    print(f"Lecture de {args.input}")
    df = pd.read_csv(args.input)
    print(f"  {len(df)} sous-cas trouvés")

    seeds = build_seed_pool(args.n_seeds, args.seed_base)
    print(f"Pool de {len(seeds)} seeds partagées : {seeds[0]}..{seeds[-1]}")

    expanded = expand_campaign(df, seeds)
    print(f"Campagne étendue : {len(expanded)} runs "
          f"({len(df)} sous-cas x {len(seeds)} seeds)")

    args.output.parent.mkdir(parents=True, exist_ok=True)
    expanded.to_csv(args.output, index=False)
    print(f"Écrit : {args.output}")


if __name__ == "__main__":
    main()