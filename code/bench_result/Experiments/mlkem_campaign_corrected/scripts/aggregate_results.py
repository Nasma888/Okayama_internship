from __future__ import annotations

import argparse
import statistics
from collections import defaultdict
from pathlib import Path
from typing import Any

from common import read_csv, read_json, write_csv


STUDY_CAMPAIGNS = (
    "A_B1_VS_B2",
    "B_V1_V2_FIRST_LOSS",
)


PREFERRED_FIELDS = [
    "run_id", "paired_group", "pair_group_id", "comparison_arm", "campaign",
    "date", "data_origin", "model", "target_prr", "mean_bad_burst", "p_gb", "p_bg",
    "forced_loss", "forced_loss_mode", "forced_loss_layer",
    "forced_loss_after_radio_channel", "forced_loss_source_id",
    "forced_loss_destination_id", "forced_loss_target_tag",
    "forced_loss_last_sequence", "rfrag_fragment_size",
    "variant", "variant_name", "kem_level", "seed", "repetition", "timeout_s",
    "mac_max_retries", "max_application_retries",
    "raw_log_path", "raw_log_sha256", "log_size_bytes", "log_line_count",
    "parsed_until_line", "parser_stop_reason",
    "parse_status", "result_class", "result_basis", "parser_warnings",
    "cooja_exit_code", "cooja_timed_out",
    "run_success", "exchange_success", "client_success", "server_success",
    "server_completion_observed", "client_server_disagreement",
    "latency_ms", "application_retries", "selective_fragment_retransmissions",
    "selective_fragment_retransmissions_scope",
    "rfrag_retransmissions", "rfrag_rto_expired", "rfrag_null_ack_rx",
    "rfrag_partial_ack_rx", "rfrag_ack_tx",
    "client_data_frag_tx", "server_data_frag_tx", "total_fragments_tx",
    "fragments_tx_scope",
    "forced_loss_injection_count", "forced_loss_armed_count",
    "forced_loss_applied_count", "forced_loss_application",
    "forced_loss_validation", "precondition_not_reached",
    "targeted_test_reached",
    "client_end_reason", "client_pk_size", "client_ct_size",
    "client_pk_frags_expected", "client_ct_frags_expected",
    "client_app_attempts", "client_app_retries", "client_app_timeouts",
    "client_rtt_valid", "client_rtt_ticks", "client_rtt_ms",
    "client_elapsed_ticks", "client_elapsed_ms",
    "client_net_datagram_tx", "client_net_datagram_rx_complete",
    "client_net_data_frag_tx", "client_net_data_frag_rx",
    "client_net_data_bytes_tx", "client_net_data_bytes_rx",
    "client_net_rfrag_retx_tx", "client_net_rfrag_ack_tx",
    "client_net_rfrag_ack_rx", "client_net_rto_expired",
    "client_net_null_ack_rx", "client_net_partial_ack_rx", "client_net_full_ack_rx",
    "server_end_reason", "server_pk_valid_rx", "server_pk_duplicate_rx",
    "server_pk_invalid_rx", "server_ct_tx", "server_last_one_way_ms",
    "server_net_datagram_tx", "server_net_datagram_rx_complete",
    "server_net_data_frag_tx", "server_net_data_frag_rx",
    "server_net_data_bytes_tx", "server_net_data_bytes_rx",
    "server_net_rfrag_retx_tx", "server_net_rfrag_ack_tx",
    "server_net_rfrag_ack_rx", "server_net_rto_expired",
]


def as_number(value: Any) -> float | None:
    try:
        if value is None or str(value).strip() == "":
            return None
        return float(value)
    except (TypeError, ValueError):
        return None


def as_int(value: Any) -> int | None:
    value = as_number(value)
    return int(value) if value is not None else None


def collect(root: Path) -> list[dict[str, Any]]:
    rows: list[dict[str, Any]] = []
    for result_path in sorted((root / "runs").rglob("results.csv")):
        run_dir = result_path.parent
        parsed = read_csv(result_path)
        if len(parsed) != 1:
            continue
        row = dict(parsed[0])
        row["raw_log_path"] = str((run_dir / "cooja.log").relative_to(root))
        if not row.get("date"):
            config = read_json(run_dir / "run_config.json", {})
            status = read_json(run_dir / "status.json", {})
            row["date"] = config.get("date") or status.get("started_at")
        rows.append(row)

    rows.sort(key=lambda row: (
        row.get("campaign", ""),
        int(row.get("kem_level") or 0),
        row.get("model", ""),
        float(row.get("target_prr") or 0),
        float(row.get("mean_bad_burst") or 0),
        row.get("forced_loss", ""),
        int(row.get("seed") or 0),
        int(row.get("variant") or 0),
    ))
    return rows


def field_order(rows: list[dict[str, Any]]) -> list[str]:
    present = {key for row in rows for key in row}
    ordered = [field for field in PREFERRED_FIELDS if field in present]
    ordered.extend(sorted(present - set(ordered)))
    return ordered


def is_precondition_not_reached(row: dict[str, Any]) -> bool:
    return (
        row.get("campaign") == "B_V1_V2_FIRST_LOSS"
        and row.get("forced_loss_validation") == "not_reached"
        and row.get("forced_loss_application") == "precondition_not_reached"
    )


def is_targeted_test_reached(row: dict[str, Any]) -> bool:
    if row.get("campaign") != "B_V1_V2_FIRST_LOSS":
        return False
    if row.get("forced_loss_validation") != "valid":
        return False

    flag = as_int(row.get("targeted_test_reached"))
    return flag == 1 if flag is not None else True


def is_analysis_eligible(row: dict[str, Any]) -> bool:
    """Rows used to evaluate protocol performance.

    Study A: every client-evaluated run.
    Study B: only runs where the targeted first-fragment-loss test was reached
    and validated. Route-acquisition failures are retained in all_runs.csv but
    excluded from V1/V2 recovery performance metrics.
    """
    client_success = as_number(row.get("client_success"))
    if client_success not in (0, 1):
        return False

    if row.get("campaign") == "B_V1_V2_FIRST_LOSS":
        return is_targeted_test_reached(row)

    return True


def aggregate(rows: list[dict[str, Any]]) -> list[dict[str, Any]]:
    keys = (
        "campaign", "comparison_arm", "kem_level", "variant", "variant_name",
        "model", "target_prr", "mean_bad_burst", "p_gb", "p_bg", "forced_loss",
    )
    groups: dict[tuple, list[dict[str, Any]]] = defaultdict(list)
    for row in rows:
        groups[tuple(row.get(key, "") for key in keys)].append(row)

    output: list[dict[str, Any]] = []

    for group_key, items in sorted(
        groups.items(),
        key=lambda item: tuple(map(str, item[0])),
    ):
        campaign = str(group_key[0])

        client_evaluated = [
            row for row in items
            if as_number(row.get("client_success")) in (0, 1)
        ]
        evaluated = [row for row in items if is_analysis_eligible(row)]
        successful = [
            row for row in evaluated
            if as_number(row.get("client_success")) == 1
        ]

        precondition_rows = [
            row for row in items if is_precondition_not_reached(row)
        ]
        targeted_rows = [
            row for row in items if is_targeted_test_reached(row)
        ]
        invalid_targeted_rows = [
            row for row in items
            if campaign == "B_V1_V2_FIRST_LOSS"
            and row.get("forced_loss_validation") not in {"valid", "not_reached"}
        ]

        def mean(
            field: str,
            source: list[dict[str, Any]] | None = None,
        ) -> float | None:
            src = evaluated if source is None else source
            values = [
                value
                for value in (as_number(row.get(field)) for row in src)
                if value is not None
            ]
            return statistics.fmean(values) if values else None

        latencies = [
            value
            for value in (
                as_number(row.get("latency_ms")) for row in successful
            )
            if value is not None
        ]

        result = dict(zip(keys, group_key))
        result.update({
            # Total raw evidence retained for traceability.
            "number_of_runs": len(items),
            "client_evaluated_runs": len(client_evaluated),

            # Runs used for protocol-performance analysis.
            "evaluated_runs": len(evaluated),
            "successful_runs": len(successful),
            "success_rate": (
                len(successful) / len(evaluated)
                if evaluated else None
            ),

            # Targeted-study reachability accounting.
            "targeted_test_reached_runs": (
                len(targeted_rows)
                if campaign == "B_V1_V2_FIRST_LOSS"
                else None
            ),
            "targeted_test_reached_rate": (
                len(targeted_rows) / len(items)
                if campaign == "B_V1_V2_FIRST_LOSS" and items
                else None
            ),
            "precondition_not_reached_runs": (
                len(precondition_rows)
                if campaign == "B_V1_V2_FIRST_LOSS"
                else None
            ),
            "precondition_not_reached_rate": (
                len(precondition_rows) / len(items)
                if campaign == "B_V1_V2_FIRST_LOSS" and items
                else None
            ),
            "invalid_targeted_runs": (
                len(invalid_targeted_rows)
                if campaign == "B_V1_V2_FIRST_LOSS"
                else None
            ),

            "mean_latency_success": (
                statistics.fmean(latencies) if latencies else None
            ),
            "median_latency_success": (
                statistics.median(latencies) if latencies else None
            ),
            "mean_application_retries": mean("application_retries"),
            "mean_selective_retransmissions": mean(
                "selective_fragment_retransmissions"
            ),
            "mean_rfrag_retransmissions": mean("rfrag_retransmissions"),
            "mean_rfrag_rto_expired": mean("rfrag_rto_expired"),
            "mean_rfrag_null_ack_rx": mean("rfrag_null_ack_rx"),
            "mean_rfrag_partial_ack_rx": mean("rfrag_partial_ack_rx"),
            "mean_rfrag_ack_tx": mean("rfrag_ack_tx"),
            "mean_fragments_tx": mean("total_fragments_tx"),

            # Denominator is the targeted-test-reached subset, not all B runs.
            "forced_loss_applied_rate": (
                sum(
                    1 for row in targeted_rows
                    if row.get("forced_loss_application")
                    == "applied_after_channel"
                ) / len(targeted_rows)
                if targeted_rows
                else None
            ),
        })
        output.append(result)

    return output


def _write_result_set(
    output_dir: Path,
    rows: list[dict[str, Any]],
    summary: list[dict[str, Any]],
) -> None:
    output_dir.mkdir(parents=True, exist_ok=True)
    fields = field_order(rows) if rows else PREFERRED_FIELDS

    write_csv(output_dir / "all_runs.csv", rows, fields)

    summary_fields = list(summary[0]) if summary else [
        "campaign", "comparison_arm", "kem_level", "variant", "variant_name",
        "model", "target_prr", "mean_bad_burst", "p_gb", "p_bg", "forced_loss",
        "number_of_runs", "client_evaluated_runs",
        "evaluated_runs", "successful_runs", "success_rate",
        "targeted_test_reached_runs", "targeted_test_reached_rate",
        "precondition_not_reached_runs", "precondition_not_reached_rate",
        "invalid_targeted_runs",
        "mean_latency_success", "median_latency_success",
        "mean_application_retries", "mean_selective_retransmissions",
        "mean_rfrag_retransmissions", "mean_rfrag_rto_expired",
        "mean_rfrag_null_ack_rx", "mean_rfrag_partial_ack_rx",
        "mean_rfrag_ack_tx", "mean_fragments_tx",
        "forced_loss_applied_rate",
    ]
    write_csv(output_dir / "summary.csv", summary, summary_fields)

    # Failed runs now mean failures among runs that were actually eligible for
    # protocol-performance analysis. In Study B, route-acquisition failures are
    # reported separately as precondition_not_reached.
    failed = [
        row for row in rows
        if is_analysis_eligible(row)
        and as_number(row.get("client_success")) == 0
    ]
    write_csv(output_dir / "failed_runs.csv", failed, fields)

    manifest_fields = [
        "run_id", "paired_group", "campaign", "data_origin",
        "kem_level", "variant", "model", "target_prr",
        "mean_bad_burst", "forced_loss", "seed",
        "forced_loss_validation", "forced_loss_application",
        "precondition_not_reached", "targeted_test_reached",
        "raw_log_path", "raw_log_sha256", "log_size_bytes", "log_line_count",
        "parse_status", "run_success", "result_class",
    ]
    write_csv(output_dir / "log_manifest.csv", rows, manifest_fields)

    if any(row.get("campaign") == "B_V1_V2_FIRST_LOSS" for row in rows):
        targeted_rows = [
            row for row in rows if is_targeted_test_reached(row)
        ]
        precondition_rows = [
            row for row in rows if is_precondition_not_reached(row)
        ]
        invalid_rows = [
            row for row in rows
            if row.get("campaign") == "B_V1_V2_FIRST_LOSS"
            and row.get("forced_loss_validation") not in {"valid", "not_reached"}
        ]

        write_csv(
            output_dir / "targeted_runs.csv",
            targeted_rows,
            fields,
        )
        write_csv(
            output_dir / "precondition_not_reached_runs.csv",
            precondition_rows,
            fields,
        )
        write_csv(
            output_dir / "invalid_targeted_runs.csv",
            invalid_rows,
            fields,
        )


def rebuild(root: Path) -> tuple[list[dict[str, Any]], list[dict[str, Any]]]:
    rows = collect(root)
    summary = aggregate(rows)
    results_dir = root / "results"

    # Global machine-readable view across both studies.
    _write_result_set(results_dir, rows, summary)

    # Separate analysis-ready outputs for the two scientific questions.
    for campaign in STUDY_CAMPAIGNS:
        study_rows = [
            row for row in rows
            if row.get("campaign") == campaign
        ]
        study_summary = [
            row for row in summary
            if row.get("campaign") == campaign
        ]
        _write_result_set(
            results_dir / f"study_{campaign}",
            study_rows,
            study_summary,
        )

    return rows, summary


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    rebuild(args.root.resolve())
