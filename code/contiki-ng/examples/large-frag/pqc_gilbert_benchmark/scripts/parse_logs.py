#!/usr/bin/env python3
"""
Parse le stdout capturé (results/run_status/<run_id>.stdout) et calcule
P_GB_hat, P_BG_hat, PRR_hat par comptage de transitions (MLE standard
pour une chaîne de Markov à deux états, cf. Gilbert 1960).

SOURCE DE LOG (format confirmé + patch v2, cf. java_patch/*.patch.txt) :

  "[Gilbert] burst ended on <src>-><dst>: length=<N> fragments cumulative_attempts=<C>"

Une seule ligne par rafale (pas par tentative) -- le compteur cumulé
`cumulative_attempts` permet de reconstruire N(G) et N(G->B) sans logger
chaque tentative individuellement.

--- Rappel du comptage (cf. discussion protocole) ---
Pour une fenêtre entre deux rafales consécutives i et j (i < j), après
exclusion des burn_in_bursts premières rafales :

  N_total_fenêtre = cumulative_attempts[j] - cumulative_attempts[i_burn_in]
  N(B)            = somme des longueurs de rafales dans la fenêtre
  N(B->G)         = nombre de rafales dans la fenêtre (chaque rafale se
                     termine par exactement une transition B->G)
  N(G->B)         = nombre de rafales dans la fenêtre (chaque rafale
                     démarre par exactement une transition G->B)
  N(G)            = N_total_fenêtre - N(B)

  P_BG_hat = N(B->G) / N(B)              (= 1 / E[L]_empirique, identique
                                            aux deux approches)
  P_GB_hat = N(G->B) / N(G)
  PRR_hat  = N(G) / N_total_fenêtre

LIMITE CONNUE (censure) : les tentatives après la fin de la DERNIÈRE
rafale loggée (si le run se termine en état GOOD) ne sont pas comptées
dans N_total_fenêtre -- léger biais, cohérent avec le traitement déjà
prévu pour la censure de la dernière rafale (cf. protocole §4).

*** ADAPTER LINK_ID avant de lancer le parsing (obligatoire) : ***
Le "link" utilise les IDs de motes Cooja (ex: "2->3"), pas des noms de
rôle. Vérifie sur un premier run quel ID correspond au client et lequel
au serveur avant de figer --link-id.
"""

import argparse
import csv
import json
import re
from pathlib import Path
from statistics import mean

ROOT = Path(__file__).parent.parent
MANIFEST_PATH = ROOT / "results" / "run_manifest.json"
PARSED_DIR = ROOT / "results" / "parsed"
MASTER_CSV_PATH = ROOT / "results" / "master_results.csv"

BURST_PATTERN = re.compile(
    r"\[Gilbert\]\s+burst ended on\s+(?P<link>\S+):\s+"
    r"length=(?P<length>\d+)\s+fragments\s+cumulative_attempts=(?P<cum>\d+)"
)


def extract_bursts(log_path: Path, link_id: str) -> list[tuple[int, int]]:
    """Retourne [(length, cumulative_attempts), ...] en ordre chronologique."""
    bursts = []
    if not log_path.exists():
        return bursts
    with log_path.open(errors="replace") as f:
        for line in f:
            m = BURST_PATTERN.search(line)
            if not m or m.group("link") != link_id:
                continue
            bursts.append((int(m.group("length")), int(m.group("cum"))))
    return bursts


def parse_run(run: dict, link_id: str, burn_in_bursts: int) -> dict:
    log_path = Path(run["log_path"])
    all_bursts = extract_bursts(log_path, link_id)

    n_raw = len(all_bursts)
    if n_raw <= burn_in_bursts:
        # Pas assez de rafales pour dépasser le burn-in -- run inexploitable
        return {
            "run_id": run["run_id"], "config_id": run["config_id"],
            "suite": run["suite"], "seed": run["seed"], "link_id": link_id,
            "prr_target": run["prr_target"], "e_l_target": run["e_l_target"],
            "p_gb": run["p_gb"], "p_bg": run["p_bg"],
            "n_bursts_raw": n_raw, "sufficient_sample": False,
            "has_burst_log": n_raw > 0,
            "prr_empirical": None, "p_gb_empirical": None, "p_bg_empirical": None,
            "e_l_empirical": None, "pi_b_empirical": None,
            "n_window_attempts": None, "burst_lengths": [],
        }

    # Fenêtre analysée = à partir de la fin de la burn_in_bursts-ième rafale
    baseline_cum = all_bursts[burn_in_bursts - 1][1] if burn_in_bursts > 0 else 0
    window = all_bursts[burn_in_bursts:]

    lengths = [l for l, _ in window]
    last_cum = window[-1][1]

    n_total_window = last_cum - baseline_cum
    n_b = sum(lengths)                 # N(B)
    n_bursts_window = len(window)      # N(B->G) = N(G->B)
    n_g = n_total_window - n_b         # N(G)

    p_bg_hat = n_bursts_window / n_b if n_b > 0 else None
    p_gb_hat = n_bursts_window / n_g if n_g and n_g > 0 else None
    prr_hat = n_g / n_total_window if n_total_window > 0 else None
    e_l_hat = mean(lengths)
    pi_b_hat = n_b / n_total_window if n_total_window > 0 else None

    return {
        "run_id": run["run_id"],
        "config_id": run["config_id"],
        "suite": run["suite"],
        "seed": run["seed"],
        "link_id": link_id,
        "prr_target": run["prr_target"],
        "e_l_target": run["e_l_target"],
        "p_gb": run["p_gb"],
        "p_bg": run["p_bg"],
        "n_bursts_raw": n_raw,
        "n_bursts_window": n_bursts_window,
        "n_total_window": n_total_window,
        "n_b": n_b,
        "n_g": n_g,
        "prr_empirical": prr_hat,
        "p_gb_empirical": p_gb_hat,
        "p_bg_empirical": p_bg_hat,
        "e_l_empirical": e_l_hat,
        "pi_b_empirical": pi_b_hat,
        "burst_lengths": lengths,   # conservé pour le test KS ultérieur
        "sufficient_sample": n_total_window >= run.get("min_transmissions", 15_000),
        "has_burst_log": True,
    }


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--link-id", required=True,
                         help="Lien dirigé au format Cooja, ex: '2->3'. "
                              "Obligatoire, à vérifier au préalable (cf. docstring).")
    parser.add_argument("--burn-in-bursts", type=int, default=None,
                         help="Override du burn-in (défaut: valeur de configs.json)")
    args = parser.parse_args()

    if not MANIFEST_PATH.exists():
        raise SystemExit(f"Manifest introuvable: {MANIFEST_PATH}. Lance render_csc.py d'abord.")

    manifest = json.loads(MANIFEST_PATH.read_text())
    configs = json.loads((ROOT / "configs" / "configs.json").read_text())["configs"]
    default_burn_in = {c["config_id"]: c["burn_in_bursts"] for c in configs}

    PARSED_DIR.mkdir(parents=True, exist_ok=True)

    rows = []
    n_no_burst_log = 0
    for run in manifest:
        burn_in = args.burn_in_bursts if args.burn_in_bursts is not None \
            else default_burn_in.get(run["config_id"], 5)
        result = parse_run(run, link_id=args.link_id, burn_in_bursts=burn_in)

        if not result["has_burst_log"]:
            n_no_burst_log += 1

        (PARSED_DIR / f"{result['run_id']}.json").write_text(json.dumps(result, indent=2))

        rows.append({
            "run_id": result["run_id"],
            "config_id": result["config_id"],
            "suite": result["suite"],
            "seed": result["seed"],
            "link_id": result["link_id"],
            "prr_target": result["prr_target"],
            "e_l_target": result["e_l_target"],
            "p_gb_theoretical": result["p_gb"],
            "p_bg_theoretical": result["p_bg"],
            "p_gb_empirical": result["p_gb_empirical"],
            "p_bg_empirical": result["p_bg_empirical"],
            "prr_empirical": result["prr_empirical"],
            "e_l_empirical": result["e_l_empirical"],
            "pi_b_theoretical": result["p_gb"] / (result["p_gb"] + result["p_bg"]),
            "pi_b_empirical": result["pi_b_empirical"],
            "n_bursts_window": result.get("n_bursts_window"),
            "n_total_window": result.get("n_total_window"),
            "sufficient_sample": result["sufficient_sample"],
        })

    with MASTER_CSV_PATH.open("w", newline="") as f:
        writer = csv.DictWriter(f, fieldnames=list(rows[0].keys()))
        writer.writeheader()
        writer.writerows(rows)

    n_insufficient = sum(1 for r in rows if not r["sufficient_sample"])
    print(f"[OK] {len(rows)} runs parsés (link_id={args.link_id}) -> {MASTER_CSV_PATH}")
    print(f"[OK] détails par run (avec listes de rafales) -> {PARSED_DIR}/")
    if n_no_burst_log:
        print(f"[!] {n_no_burst_log} runs sans log 'burst ended' exploitable -- "
              f"vérifie gilbert_log_bursts=true et le patch v2 (cumulative_attempts) "
              f"appliqué et recompilé, et que --link-id correspond au run.")
    if n_insufficient:
        print(f"[!] {n_insufficient} runs sous min_transmissions dans la fenêtre "
              f"post-burn-in -- augmenter --duration-ms dans render_csc.py si besoin.")


if __name__ == "__main__":
    main()
