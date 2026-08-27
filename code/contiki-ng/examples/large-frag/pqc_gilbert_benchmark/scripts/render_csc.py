#!/usr/bin/env python3
"""
Génère un fichier .csc par run (config x seed) à partir du template.

Usage:
    python3 render_csc.py [--out-dir DIR] [--duration-ms N]
"""

import argparse
import json
from pathlib import Path

ROOT = Path(__file__).parent.parent
TEMPLATE_PATH = ROOT / "templates" / "simulation.csc.template"
CONFIGS_PATH = ROOT / "configs" / "configs.json"


def render_one(template: str, run: dict, duration_ms: int) -> str:
    substitutions = {
        "__RUN_ID__": run["run_id"],
        "__SEED__": str(run["seed"]),
        "__P_GB__": f"{run['p_gb']:.10f}",
        "__P_BG__": f"{run['p_bg']:.10f}",
        "__DURATION_MS__": str(duration_ms),
    }
    out = template
    for token, value in substitutions.items():
        out = out.replace(token, value)
    return out


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out-dir", default=str(ROOT / "generated_csc"))
    parser.add_argument("--duration-ms", type=int, default=1_800_000,
                         help="Durée max par run (défaut 30 min simulées). "
                              "Ajuste selon ton débit d'échange applicatif "
                              "pour atteindre min_transmissions par run.")
    args = parser.parse_args()

    out_dir = Path(args.out_dir)
    out_dir.mkdir(parents=True, exist_ok=True)
    # run_batch.sh capture déjà le stdout de `gradlew run` (où atterrissent
    # les logs SLF4J "[Gilbert] ...") dans results/run_status/<run_id>.stdout
    # -- c'est ce fichier, pas un log séparé, qui sert de source à parse_logs.py
    stdout_dir = ROOT / "results" / "run_status"
    stdout_dir.mkdir(parents=True, exist_ok=True)

    template = TEMPLATE_PATH.read_text()
    data = json.loads(CONFIGS_PATH.read_text())

    manifest = []
    for run in data["runs"]:
        rendered = render_one(template, run, args.duration_ms)
        csc_path = out_dir / f"{run['run_id']}.csc"
        csc_path.write_text(rendered)
        manifest.append({
            "run_id": run["run_id"],
            "config_id": run["config_id"],
            "suite": run["suite"],
            "seed": run["seed"],
            "csc_path": str(csc_path.resolve()),
            "log_path": str((stdout_dir / f"{run['run_id']}.stdout").resolve()),
            "p_gb": run["p_gb"],
            "p_bg": run["p_bg"],
            "prr_target": run["prr_target"],
            "e_l_target": run["e_l_target"],
        })

    manifest_path = out_dir.parent / "results" / "run_manifest.json"
    manifest_path.write_text(json.dumps(manifest, indent=2))
    print(f"[OK] {len(manifest)} fichiers .csc générés dans {out_dir}")
    print(f"[OK] manifest écrit -> {manifest_path}")


if __name__ == "__main__":
    main()
