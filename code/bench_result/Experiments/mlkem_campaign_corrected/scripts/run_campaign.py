from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path

from aggregate_results import rebuild
from common import normalize_experiment, read_csv, read_json, run_directory, utc_now, write_json
from generate_csc import generate
from parse_cooja_log import parse_run
from run_one_simulation import run
from validate_results import validate, write_report


def _read_candidates(paths: list[Path]) -> str:
    chunks: list[str] = []
    for path in paths:
        if path.is_file():
            try:
                chunks.append(path.read_text(encoding="utf-8", errors="ignore"))
            except OSError:
                pass
    return "\n".join(chunks)


def preflight(contiki: Path, experiments: list[dict]) -> tuple[Path, list[str]]:
    cooja = contiki / "tools" / "cooja"
    if not cooja.is_dir():
        raise FileNotFoundError(f"Cooja introuvable: {cooja}")
    if not (cooja / "gradlew").is_file():
        raise FileNotFoundError(f"gradlew introuvable: {cooja / 'gradlew'}")

    warnings: list[str] = []
    if any(exp["model"] == "gilbert" for exp in experiments):
        matches = list(cooja.rglob("UDGMGilbertLoss.java"))
        if not matches:
            raise FileNotFoundError(
                "UDGMGilbertLoss.java absent de Cooja; la campagne Gilbert "
                "ne peut pas être exécutée fidèlement"
            )

    rfrag_experiments = [exp for exp in experiments if exp["variant"] in {2, 3, 4}]
    if rfrag_experiments:
        candidates = list((contiki / "os" / "net").rglob("sicslowpan*"))
        text = _read_candidates(candidates)
        if "SICSLOWPAN_CONF_RFRAG" not in text:
            raise RuntimeError("Patch RFRAG introuvable dans Contiki-NG")

    forced_experiments = [exp for exp in experiments if exp["forced_loss"] != "none"]
    if forced_experiments:
        candidates = list((contiki / "os" / "net").rglob("sicslowpan*"))
        text = _read_candidates(candidates)
        required_tokens = (
            "SICSLOWPAN_CONF_RFRAG_TEST_ENABLE",
            "SICSLOWPAN_CONF_RFRAG_TEST_DROP_DATA_BITMAP",
        )
        missing = [token for token in required_tokens if token not in text]
        has_loss_log = (
            "FORCED_LOSS," in text
            or "RFRAG TEST: forced DATA drop" in text
        )
        if missing or not has_loss_log:
            details = missing + ([] if has_loss_log else ["forced-loss log hook"])
            raise RuntimeError(
                "Le hook de perte forcée RFRAG n'est pas installé dans sicslowpan.c: "
                + ", ".join(details)
            )

    return cooja, warnings


def build_workbooks(root: Path, campaign: Path) -> bool:
    del campaign  # retained in the signature for CLI compatibility
    report_script = Path(__file__).with_name("report.py")
    if not report_script.is_file():
        print(f"  Excel non généré: {report_script} absent")
        return False
    command = [
        sys.executable,
        str(report_script),
        "--input-dir", str(root / "results"),
        "--output-dir", str(root / "results" / "workbooks"),
    ]
    completed = subprocess.run(command, check=False)
    if completed.returncode != 0:
        print("  Excel non généré automatiquement; les CSV et logs restent valides")
        return False
    return True




def result_has_current_schema(path: Path) -> bool:
    if not path.is_file():
        return False
    try:
        rows = read_csv(path)
    except (OSError, ValueError):
        return False
    if len(rows) != 1:
        return False
    required = {
        "run_success",
        "forced_loss",
        "application_retries",
        "selective_fragment_retransmissions",
        "total_fragments_tx",
        "forced_loss_application",
        "rfrag_rto_expired",
    }
    return required.issubset(rows[0])

def main() -> int:
    parser = argparse.ArgumentParser(
        description="Pipeline Cooja -> logs bruts -> CSV -> classeurs"
    )
    parser.add_argument("--campaign", type=Path, default=Path("config/experiments.csv"))
    parser.add_argument("--contiki", type=Path)
    parser.add_argument("--cooja", type=Path)
    parser.add_argument("--root", type=Path, default=Path(__file__).resolve().parents[1])
    parser.add_argument("--force", action="store_true")
    parser.add_argument("--parse-only", action="store_true")
    parser.add_argument("--no-excel", action="store_true")
    parser.add_argument("--kem", type=int, choices=[512, 768, 1024])
    parser.add_argument("--variant", type=int, choices=[1, 2, 3, 4])
    parser.add_argument("--model", choices=["fixed", "gilbert"])
    parser.add_argument("--campaign-arm", choices=["A_B1_VS_B2", "B_V1_V2_FIRST_LOSS"])
    parser.add_argument(
        "--forced-loss",
        choices=["none", "first_fragment"],
    )
    parser.add_argument("--limit", type=int)
    args = parser.parse_args()

    root = args.root.resolve()
    campaign = args.campaign if args.campaign.is_absolute() else root / args.campaign
    experiments = [normalize_experiment(row) for row in read_csv(campaign)]

    if args.kem:
        experiments = [exp for exp in experiments if exp["kem_level"] == args.kem]
    if args.variant:
        experiments = [exp for exp in experiments if exp["variant"] == args.variant]
    if args.model:
        experiments = [exp for exp in experiments if exp["model"] == args.model]
    if args.campaign_arm:
        experiments = [exp for exp in experiments if exp["campaign"] == args.campaign_arm]
    if args.forced_loss:
        experiments = [exp for exp in experiments if exp["forced_loss"] == args.forced_loss]
    if args.limit:
        experiments = experiments[: args.limit]
    if not experiments:
        raise SystemExit("Aucune expérience après filtrage")

    cooja = args.cooja.resolve() if args.cooja else None
    contiki = args.contiki.resolve() if args.contiki else None
    if not args.parse_only:
        if contiki is None:
            raise SystemExit("--contiki est requis hors mode --parse-only")
        default_cooja, warnings = preflight(contiki, experiments)
        cooja = cooja or default_cooja
        for warning in warnings:
            print(f"PRÉ-VÉRIFICATION: {warning}")

    failures = 0
    for index, exp in enumerate(experiments, 1):
        run_dir = run_directory(root / "runs", exp)
        status = read_json(run_dir / "status.json", {})
        print(f"[{index}/{len(experiments)}] {exp['run_id']}")
        existing_log = run_dir / "cooja.log"
        existing_result = run_dir / "results.csv"
        if (
            not args.force
            and existing_log.is_file()
            and existing_log.stat().st_size > 0
            and result_has_current_schema(existing_result)
            and status.get("parse_status") in {"success", "success_with_warnings"}
        ):
            print("  déjà parsé; --force pour refaire")
            continue

        run_dir.mkdir(parents=True, exist_ok=True)
        exp["date"] = utc_now()
        write_json(run_dir / "run_config.json", exp)

        if not args.parse_only:
            assert contiki is not None and cooja is not None
            simulation = root / "generated" / "simulations" / f"{exp['run_id']}.csc"
            build_dir = root / "generated" / "build" / exp["run_id"]
            try:
                generate(
                    root / "config" / "template.csc",
                    root / "config" / "motes.xml",
                    root / "firmware",
                    simulation,
                    build_dir,
                    exp,
                    contiki,
                )
                shutil.copy2(simulation, run_dir / "simulation.csc")
                run(cooja, contiki, simulation, run_dir, exp)
            except Exception as exc:
                failures += 1
                write_json(run_dir / "status.json", {
                    "run_id": exp["run_id"], "exit_code": -1, "timed_out": False,
                    "parse_status": "generation_or_run_error", "error": str(exc),
                })
                print(f"  ERREUR EXÉCUTION: {exc}")
                continue

        if not existing_log.is_file():
            failures += 1
            print("  ERREUR: cooja.log absent")
            continue
        try:
            parse_run(run_dir)
        except Exception as exc:
            failures += 1
            status = read_json(run_dir / "status.json", {})
            status.update({"parse_status": "parser_error", "error": str(exc)})
            write_json(run_dir / "status.json", status)
            print(f"  ERREUR PARSING: {exc}")

    rows, _ = rebuild(root)
    issues = validate(root)
    write_report(root, issues)
    for issue in issues:
        print(f"VALIDATION {issue['severity']}: {issue['run_id']}: {issue['issue']}")
    if not args.no_excel and rows:
        build_workbooks(root, campaign)
    errors = [item for item in issues if item["severity"] == "ERROR"]
    print(f"Terminé: {len(rows)} runs agrégés")
    return 1 if failures or errors else 0


if __name__ == "__main__":
    raise SystemExit(main())
