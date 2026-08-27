#!/usr/bin/env python3
"""
Génération des configurations théoriques pour la validation du modèle Gilbert.

Suite A : E[L] = 10 fixé, PRR variable
Suite B : PRR = 0.85 fixé, E[L] variable
Le point (PRR=0.85, E[L]=10) est commun aux deux suites -> dédupliqué,
mais marqué avec suite="A+B" pour servir de point de jonction.

Les MEMES seeds sont utilisées pour TOUTES les configurations (appariement),
ce qui permet des comparaisons par paire (variance réduite) et pas seulement
des comparaisons de moyennes indépendantes.

Sortie : configs.json (une entrée par configuration x seed = un "run")
"""

import json
import hashlib
from pathlib import Path

# ---------------------------------------------------------------------------
# Paramètres globaux du plan d'expérience
# ---------------------------------------------------------------------------

N_SEEDS = 30                     # réplications indépendantes par configuration
BASE_SEED_POOL_SIZE = 10_000     # espace duquel on tire les seeds (déterministe)
MIN_TRANSMISSIONS = 15_000       # cf. dimensionnement d'échantillon (§4.1)
BURN_IN_PACKETS = 500            # info seulement -- non exploitable directement
                                  # depuis les logs [Gilbert], cf. BURN_IN_BURSTS
BURN_IN_BURSTS = 5               # nb de rafales exclues en début de run (proxy du
                                  # burn-in quand on ne dispose que du flux
                                  # d'événements par tentative/rafale, pas d'un
                                  # index de paquet applicatif). À ajuster : doit
                                  # rester petit devant n_bursts_observed attendu,
                                  # et grand devant le temps de mixage de la
                                  # chaîne (~ quelques (E[G]+E[L])).

# Géométrie de fragmentation ML-KEM-1024 (n=17 fragments, cf. mémoire de thèse)
FRAGMENT_COUNT = 17
PARAM_SET = "ML-KEM-1024"

OUTPUT_PATH = Path(__file__).parent / "configs.json"

# ---------------------------------------------------------------------------
# Génération des seeds : déterministe et reproductible (pas de random.seed
# global qui pourrait dériver selon l'ordre d'exécution)
# ---------------------------------------------------------------------------

def generate_seed_pool(n: int, pool_size: int = BASE_SEED_POOL_SIZE) -> list[int]:
    """Seeds déterministes dérivées d'un hash, indépendantes de l'environnement."""
    seeds = []
    i = 0
    while len(seeds) < n:
        h = hashlib.sha256(f"gilbert-validation-{i}".encode()).hexdigest()
        seeds.append(int(h[:8], 16) % pool_size)
        i += 1
    return sorted(set(seeds))[:n]


SEEDS = generate_seed_pool(N_SEEDS)

# ---------------------------------------------------------------------------
# Relations théoriques (cf. dérivation Gilbert simple)
# P_BG = 1 / E[L]
# P_GB = (1 - PRR) / (PRR * E[L])
# ---------------------------------------------------------------------------

def gilbert_params(prr: float, e_l: float) -> dict:
    p_bg = 1.0 / e_l
    p_gb = (1.0 - prr) / (prr * e_l)
    pi_b = p_gb / (p_gb + p_bg)          # doit être ~ (1-PRR)
    e_g = 1.0 / p_gb if p_gb > 0 else float("inf")
    prr_check = e_g / (e_g + e_l)
    return {
        "prr_target": prr,
        "e_l_target": e_l,
        "p_gb": p_gb,
        "p_bg": p_bg,
        "pi_b_theoretical": pi_b,
        "e_g_theoretical": e_g,
        "prr_self_consistency_check": prr_check,  # doit ≈ prr_target
    }


# ---------------------------------------------------------------------------
# Suite A : E[L] = 10, PRR variable
# ---------------------------------------------------------------------------

SUITE_A_PRR_VALUES = [0.99, 0.95, 0.90, 0.85, 0.80, 0.70, 0.60]
SUITE_A_E_L = 10.0

# ---------------------------------------------------------------------------
# Suite B : PRR = 0.85, E[L] variable
# ---------------------------------------------------------------------------

SUITE_B_E_L_VALUES = [1, 2, 5, 10, 20, 50]
SUITE_B_PRR = 0.85


def build_configs() -> list[dict]:
    configs = {}

    for prr in SUITE_A_PRR_VALUES:
        key = (round(prr, 4), round(SUITE_A_E_L, 4))
        params = gilbert_params(prr, SUITE_A_E_L)
        params["config_id"] = f"A_prr{prr:.2f}_el{int(SUITE_A_E_L)}"
        params["suite"] = "A"
        configs[key] = params

    for e_l in SUITE_B_E_L_VALUES:
        key = (round(SUITE_B_PRR, 4), round(float(e_l), 4))
        params = gilbert_params(SUITE_B_PRR, float(e_l))
        params["config_id"] = f"B_prr{SUITE_B_PRR:.2f}_el{e_l}"
        params["suite"] = "B"
        if key in configs:
            # point de jonction Suite A ∩ Suite B
            configs[key]["suite"] = "A+B"
            configs[key]["config_id"] = f"AB_prr{SUITE_B_PRR:.2f}_el{e_l}_junction"
        else:
            configs[key] = params

    # Estimation de la durée de simulation nécessaire par run
    for cfg in configs.values():
        # heuristique : il faut voir suffisamment de rafales, pas seulement
        # suffisamment de paquets -> on majore par (E[G]+E[L]) * marge
        cfg["fragment_count"] = FRAGMENT_COUNT
        cfg["param_set"] = PARAM_SET
        cfg["min_transmissions"] = MIN_TRANSMISSIONS
        cfg["burn_in_packets"] = BURN_IN_PACKETS
        cfg["burn_in_bursts"] = BURN_IN_BURSTS
        cfg["seeds"] = SEEDS

    return list(configs.values())


def main():
    configs = build_configs()

    # Sanity check auto-cohérence avant d'écrire quoi que ce soit
    for cfg in configs:
        drift = abs(cfg["prr_self_consistency_check"] - cfg["prr_target"])
        assert drift < 1e-9, f"Incohérence théorique détectée pour {cfg['config_id']}: drift={drift}"

    # Génère la liste des runs individuels (config x seed)
    runs = []
    for cfg in configs:
        for seed in cfg["seeds"]:
            run = {k: v for k, v in cfg.items() if k != "seeds"}
            run["seed"] = seed
            run["run_id"] = f"{cfg['config_id']}_seed{seed}"
            runs.append(run)

    output = {
        "meta": {
            "n_seeds_per_config": N_SEEDS,
            "seeds": SEEDS,
            "n_configs": len(configs),
            "n_runs_total": len(runs),
            "min_transmissions": MIN_TRANSMISSIONS,
            "burn_in_packets": BURN_IN_PACKETS,
        },
        "configs": configs,
        "runs": runs,
    }

    OUTPUT_PATH.write_text(json.dumps(output, indent=2))
    print(f"[OK] {len(configs)} configurations, {len(SEEDS)} seeds partagées, "
          f"{len(runs)} runs générés -> {OUTPUT_PATH}")
    for cfg in configs:
        print(f"  {cfg['config_id']:<32} suite={cfg['suite']:<3} "
              f"PRR={cfg['prr_target']:.2f} E[L]={cfg['e_l_target']:<5} "
              f"P_GB={cfg['p_gb']:.5f} P_BG={cfg['p_bg']:.5f}")


if __name__ == "__main__":
    main()
