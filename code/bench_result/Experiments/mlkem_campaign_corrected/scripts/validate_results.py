from __future__ import annotations

import argparse
from collections import defaultdict
from pathlib import Path

from common import KEM_SIZES, read_csv, sha256_file, write_csv


def as_int(value: str | None) -> int | None:
    if value is None or value == "":
        return None
    return int(float(value))


def comparable(value: str | None) -> str:
    return "" if value is None else str(value)


def validate(root: Path) -> list[dict[str, str]]:
    path = root / "results" / "all_runs.csv"
    if not path.is_file():
        return [{
            "run_id": "",
            "severity": "ERROR",
            "issue": "all_runs.csv absent",
        }]

    rows = read_csv(path)
    issues: list[dict[str, str]] = []
    if not rows:
        return [{
            "run_id": "",
            "severity": "ERROR",
            "issue": "aucun run parsé",
        }]

    seen: set[str] = set()
    paired: dict[str, list[dict[str, str]]] = defaultdict(list)

    for row in rows:
        run_id = row.get("run_id", "")

        def add(message: str, severity: str = "ERROR") -> None:
            issues.append({
                "run_id": run_id,
                "severity": severity,
                "issue": message,
            })

        if not run_id or run_id in seen:
            add("run_id absent ou dupliqué")
        seen.add(run_id)

        if row.get("paired_group"):
            paired[row["paired_group"]].append(row)

        raw_rel = row.get("raw_log_path", "")
        raw = root / raw_rel if raw_rel else None
        if raw is None or not raw.is_file():
            add(f"preuve brute absente: {raw_rel}")
        elif sha256_file(raw) != row.get("raw_log_sha256"):
            add("SHA-256 du log différent du manifeste")

        if row.get("parse_status") not in {
            "success",
            "success_with_warnings",
        }:
            add(f"parse_status invalide: {row.get('parse_status')}")

        if (
            row.get("parse_status") == "success_with_warnings"
            and row.get("parser_warnings")
        ):
            add(row["parser_warnings"], "WARNING")

        kem = as_int(row.get("kem_level"))
        expected = KEM_SIZES.get(kem or -1)
        if expected is None:
            add(f"KEM invalide: {kem}")
            continue

        for field, wanted in {
            "client_pk_size": expected["pk_size"],
            "client_ct_size": expected["ct_size"],
            "client_pk_frags_expected": expected["pk_frags"],
            "client_ct_frags_expected": expected["ct_frags"],
        }.items():
            observed = as_int(row.get(field))
            if observed is not None and observed != wanted:
                add(f"{field}={observed}, attendu={wanted}")

        client_success = as_int(row.get("client_success"))
        run_success = as_int(row.get("run_success"))
        exchange_success = as_int(row.get("exchange_success"))
        result_class = row.get("result_class")

        # Principal invariant: the client alone determines experimental success.
        if client_success in (0, 1):
            if run_success != client_success:
                add("run_success différent de client_success")
            if exchange_success != client_success:
                add("exchange_success différent de client_success")

        if result_class == "SUCCESS":
            if client_success != 1 or run_success != 1:
                add("SUCCESS sans client_success=1 et run_success=1")
        elif result_class == "FAILED_PROTOCOL":
            if client_success != 0 or run_success != 0:
                add("FAILED_PROTOCOL sans client_success=0 et run_success=0")

        # server_success is diagnostic only.
        variant = as_int(row.get("variant"))
        campaign = row.get("campaign") or ""
        forced_loss = row.get("forced_loss") or "none"

        if forced_loss != "none":
            if variant not in {3, 4}:
                add("perte forcée utilisée hors V1/V2")

            if as_int(row.get("forced_loss_after_radio_channel")) != 1:
                add("perte forcée non configurée après le canal radio")

            validation = row.get("forced_loss_validation") or ""
            application_mode = row.get("forced_loss_application") or ""

            if validation == "valid":
                if application_mode == "radio_preempted":
                    add(
                        "cible armée mais perdue par le canal/radio avant "
                        "FORCED_LOSS_APPLIED",
                        "WARNING",
                    )
                elif application_mode == "legacy_unverified":
                    add(
                        "ancien log: receiver-after-channel non vérifiable "
                        "explicitement",
                        "WARNING",
                    )
                elif application_mode != "applied_after_channel":
                    add(
                        "mode d'application de perte forcée inattendu pour "
                        f"un run validé: {application_mode}"
                    )

                targeted_reached = as_int(row.get("targeted_test_reached"))
                if targeted_reached is not None and targeted_reached != 1:
                    add(
                        "forced_loss_validation=valid mais "
                        "targeted_test_reached != 1"
                    )

                precondition_flag = as_int(
                    row.get("precondition_not_reached")
                )
                if precondition_flag is not None and precondition_flag != 0:
                    add(
                        "forced_loss_validation=valid mais "
                        "precondition_not_reached != 0"
                    )

            elif validation == "not_reached":
                # Legitimate targeted-study state: the client never acquired
                # a route, sent no PK, and therefore there was no fragment 0
                # on which the controlled loss could act.
                if application_mode != "precondition_not_reached":
                    add(
                        "not_reached doit utiliser "
                        "forced_loss_application=precondition_not_reached"
                    )

                if campaign != "B_V1_V2_FIRST_LOSS":
                    add(
                        "forced_loss_validation=not_reached hors campagne B"
                    )

                if row.get("client_end_reason") != "route_acquisition_timeout":
                    add(
                        "not_reached sans client_end_reason="
                        "route_acquisition_timeout"
                    )

                app_attempts = as_int(row.get("client_app_attempts"))
                if app_attempts != 0:
                    add(
                        f"not_reached avec client_app_attempts={app_attempts}, "
                        "attendu=0"
                    )

                armed = as_int(row.get("forced_loss_armed_count"))
                applied = as_int(row.get("forced_loss_applied_count"))
                if (armed or 0) != 0:
                    add(
                        f"not_reached avec forced_loss_armed_count={armed}, "
                        "attendu=0"
                    )
                if (applied or 0) != 0:
                    add(
                        f"not_reached avec forced_loss_applied_count={applied}, "
                        "attendu=0"
                    )

                precondition_flag = as_int(
                    row.get("precondition_not_reached")
                )
                if precondition_flag is not None and precondition_flag != 1:
                    add(
                        "not_reached mais precondition_not_reached != 1"
                    )

                targeted_reached = as_int(
                    row.get("targeted_test_reached")
                )
                if targeted_reached is not None and targeted_reached != 0:
                    add(
                        "not_reached mais targeted_test_reached != 0"
                    )

            else:
                add("injection de perte forcée réellement invalide dans le log")
                add(
                    "mode d'application de perte forcée inattendu: "
                    f"{application_mode}"
                )

        max_app = as_int(row.get("max_application_retries"))
        app_retries = as_int(row.get("application_retries")) or 0

        if max_app is not None and app_retries > max_app:
            add(
                f"application_retries={app_retries} > "
                f"max_application_retries={max_app}"
            )

        if variant in {1, 2, 3} and app_retries != 0:
            add(
                "retransmission applicative observée alors qu'elle est "
                "désactivée"
            )

        if variant == 4 and (max_app is None or max_app < 1):
            add("V2 sans retransmission applicative autorisée")

        if variant == 4 and max_app is not None and max_app != 1:
            add(
                "V2 doit autoriser exactement une retransmission applicative",
                "WARNING",
            )

        client_frag = as_int(row.get("client_data_frag_tx"))
        server_frag = as_int(row.get("server_data_frag_tx"))
        total_frag = as_int(row.get("total_fragments_tx"))

        if (
            total_frag is not None
            and client_frag is not None
            and server_frag is not None
            and total_frag != client_frag + server_frag
        ):
            add("total_fragments_tx incohérent avec client+server")

        selective = as_int(
            row.get("selective_fragment_retransmissions")
        )
        client_sel = as_int(
            row.get("client_net_selective_fragment_retransmissions")
        )
        server_sel = as_int(
            row.get("server_net_selective_fragment_retransmissions")
        )
        if (
            selective is not None
            and client_sel is not None
            and server_sel is not None
            and selective != client_sel + server_sel
        ):
            add("selective_fragment_retransmissions incohérent")

        net_total = sum(
            as_int(row.get(field)) or 0
            for field in (
                "client_net_data_frag_tx",
                "client_net_data_frag_rx",
                "server_net_data_frag_tx",
                "server_net_data_frag_rx",
            )
        )
        datagram_total = sum(
            as_int(row.get(field)) or 0
            for field in (
                "client_net_datagram_tx",
                "client_net_datagram_rx_complete",
                "server_net_datagram_tx",
                "server_net_datagram_rx_complete",
            )
        )
        app_attempts = as_int(row.get("client_app_attempts")) or 0

        if (datagram_total > 0 or app_attempts > 0) and net_total == 0:
            add(
                "trafic applicatif observé mais compteurs de fragments "
                "6LoWPAN tous nuls",
                "WARNING",
            )

    # Pairing checks: all protocol arms in a group must see the same experiment.
    invariant_fields = (
        "seed", "kem_level", "model", "target_prr", "mean_bad_burst",
        "p_gb", "p_bg", "forced_loss", "mac_max_retries",
        "rfrag_fragment_size", "forced_loss_after_radio_channel",
    )

    for group, members in paired.items():
        baseline = members[0]

        for field in invariant_fields:
            values = {
                comparable(row.get(field))
                for row in members
            }
            if len(values) != 1:
                issues.append({
                    "run_id": group,
                    "severity": "ERROR",
                    "issue": (
                        f"paired_group non équitable: {field} diffère "
                        f"({sorted(values)})"
                    ),
                })

        campaign = baseline.get("campaign", "")
        variants = {
            as_int(row.get("variant"))
            for row in members
        }

        if campaign == "A_B1_VS_B2":
            if variants != {1, 2}:
                issues.append({
                    "run_id": group,
                    "severity": "ERROR",
                    "issue": (
                        "volet A incomplet: variantes observées "
                        f"{sorted(v for v in variants if v is not None)}"
                    ),
                })

            losses = {
                row.get("forced_loss") or "none"
                for row in members
            }
            if losses != {"none"}:
                issues.append({
                    "run_id": group,
                    "severity": "ERROR",
                    "issue": (
                        "volet A ne doit utiliser aucune perte forcée, "
                        f"observé {sorted(losses)}"
                    ),
                })

        if campaign == "B_V1_V2_FIRST_LOSS":
            if variants != {3, 4}:
                issues.append({
                    "run_id": group,
                    "severity": "ERROR",
                    "issue": (
                        "volet B incomplet: variantes observées "
                        f"{sorted(v for v in variants if v is not None)}"
                    ),
                })

            losses = {
                row.get("forced_loss") or "none"
                for row in members
            }
            if losses != {"first_fragment"}:
                issues.append({
                    "run_id": group,
                    "severity": "ERROR",
                    "issue": (
                        "volet B doit cibler uniquement first_fragment, "
                        f"observé {sorted(losses)}"
                    ),
                })

            # V1 and V2 must either both reach the targeted test or both fail
            # the common precondition. Any asymmetry would bias the comparison.
            states = {
                row.get("forced_loss_validation") or ""
                for row in members
            }
            if states not in ({"valid"}, {"not_reached"}):
                issues.append({
                    "run_id": group,
                    "severity": "ERROR",
                    "issue": (
                        "volet B asymétrique/invalide: états forced_loss_validation "
                        f"{sorted(states)}"
                    ),
                })

            if states == {"not_reached"}:
                reasons = {
                    row.get("client_end_reason") or ""
                    for row in members
                }
                attempts = {
                    as_int(row.get("client_app_attempts"))
                    for row in members
                }
                if reasons != {"route_acquisition_timeout"}:
                    issues.append({
                        "run_id": group,
                        "severity": "ERROR",
                        "issue": (
                            "volet B not_reached avec raisons différentes "
                            f"ou inattendues: {sorted(reasons)}"
                        ),
                    })
                if attempts != {0}:
                    issues.append({
                        "run_id": group,
                        "severity": "ERROR",
                        "issue": (
                            "volet B not_reached mais app_attempts "
                            f"diffèrent de 0: {sorted(a for a in attempts if a is not None)}"
                        ),
                    })

    return issues


def write_report(
    root: Path,
    issues: list[dict[str, str]],
) -> None:
    write_csv(
        root / "results" / "validation_issues.csv",
        issues,
        ["run_id", "severity", "issue"],
    )


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()

    root = args.root.resolve()
    issues = validate(root)
    write_report(root, issues)

    for item in issues:
        print(
            f"{item['severity']}: "
            f"{item['run_id']}: "
            f"{item['issue']}"
        )

    raise SystemExit(
        1 if any(
            item["severity"] == "ERROR"
            for item in issues
        ) else 0
    )
