from __future__ import annotations

import argparse
import re
from pathlib import Path
from typing import Any

from common import (
    KEM_SIZES,
    parse_kv,
    read_json,
    sha256_file,
    typed_value,
    write_csv,
    write_json,
)

MARKERS = (
    "RESULT_NET_SERVER",
    "RESULT_SERVER",
    "RESULT_NET",
    "RESULT",
    "EXPERIMENT_CONFIG",
    "FORCED_LOSS_APPLIED",
    "FORCED_LOSS",
    "PK_ATTEMPT",
)
EVENT_RE = re.compile(r"(?<![A-Z_])(" + "|".join(MARKERS) + r"),(.*)$")

CLIENT_DONE_RE = re.compile(
    r"EXPERIMENT_DONE_CLIENT,success=(?P<success>[01])(?:,reason=(?P<reason>[^,\s]+))?(?:,|$)"
)

# Hook emitted by the patched 6LoWPAN receiver when a one-shot forced loss is
# actually injected. Keeping this diagnostic lets us reject a targeted run in
# which the intended critical event never occurred.
FORCED_DATA_RE = re.compile(
    r"RFRAG TEST:\s*forced DATA drop\s+node\s+(?P<node>\d+)\s+"
    r"tag\s+(?P<tag>\d+)\s+seq\s+(?P<seq>\d+)\s+"
    r"occurrence\s+(?P<occurrence>\d+)",
    re.IGNORECASE,
)


def prefixed(target: dict[str, Any], prefix: str, values: dict[str, str]) -> None:
    for key, value in values.items():
        target[f"{prefix}_{key}"] = typed_value(value)


def as_int(value: Any) -> int | None:
    if value is None or str(value).strip() == "":
        return None
    try:
        return int(float(value))
    except (TypeError, ValueError):
        return None


def expected_forced_sequences(config: dict[str, Any]) -> list[int]:
    mode = str(config.get("forced_loss") or "none")
    last = int(
        config.get("forced_loss_last_sequence")
        if config.get("forced_loss_last_sequence") is not None
        else KEM_SIZES[int(config["kem_level"])]["pk_frags"] - 1
    )
    return {
        "none": [],
        "first_fragment": [0],
        "last_fragment": [last],
        "first_and_last_fragment": [0, last],
    }.get(mode, [])


def validate_forced_loss_events(
    config: dict[str, Any],
    armed_events: list[dict[str, Any]],
    applied_events: list[dict[str, Any]],
    pk_attempts: list[dict[str, Any]],
) -> tuple[str, int, int, str]:
    """Validate targeted loss in two distinct stages.

    FORCED_LOSS means that the sender armed/tagged the target frame.
    FORCED_LOSS_APPLIED means that the marked frame survived the radio path
    and was then discarded by the receiver-side test hook.

    If the radio path already prevents reception of the marked frame, the
    targeted first-fragment-loss condition still occurred, but the artificial
    receiver drop was pre-empted. We keep the run valid and expose that fact
    explicitly through ``forced_loss_application``.
    """
    configured = str(config.get("forced_loss") or "none")
    expected_labels = {
        "none": [],
        "first_fragment": ["first_fragment"],
        "last_fragment": ["last_fragment"],
        "first_and_last_fragment": ["first_fragment", "last_fragment"],
    }.get(configured, [])

    if configured == "none":
        valid = not armed_events and not applied_events
        return ("valid" if valid else "invalid"), len(armed_events), len(applied_events), "none"

    structured = [event for event in armed_events if event.get("forced_loss_injected")]
    if structured:
        armed_labels = sorted(str(event.get("forced_loss_injected")) for event in structured)
        only_attempt_one = all(as_int(event.get("pk_attempt")) == 1 for event in structured)
        layer_valid = all(
            str(event.get("forced_loss_layer")) == "receiver_after_channel"
            for event in structured
        )
        channel_valid = all(
            str(event.get("channel")) == "normal_then_receiver_drop"
            for event in structured
        )
        later_attempts_normal = all(
            str(attempt.get("forced_loss_injected")) == "none"
            and str(attempt.get("channel")) == "normal"
            for attempt in pk_attempts
            if (as_int(attempt.get("pk_attempt")) or 0) > 1
        )
        armed_valid = (
            armed_labels == sorted(expected_labels)
            and only_attempt_one
            and layer_valid
            and channel_valid
            and later_attempts_normal
        )

        applied_labels: list[str] = []
        for event in applied_events:
            label = str(event.get("forced_loss") or "")
            if not label:
                seq = as_int(event.get("fragment_sequence"))
                if seq == 0:
                    label = "first_fragment"
                elif seq is not None:
                    label = "last_fragment"
            if label:
                applied_labels.append(label)
        applied_labels.sort()

        # All expected targets reached the receiver and were explicitly dropped.
        if armed_valid and applied_labels == sorted(expected_labels):
            return "valid", len(structured), len(applied_events), "applied_after_channel"

        # The sender armed the intended one-shot target, but the normal radio
        # path prevented at least one marked frame from reaching the receiver.
        # The critical target is still lost; expose the mechanism separately.
        if armed_valid and all(label in expected_labels for label in applied_labels):
            return "valid", len(structured), len(applied_events), "radio_preempted"

        return "invalid", len(structured), len(applied_events), "invalid"

    # Legacy logs can only prove which sequence was selected, not whether a
    # separate receiver-side FORCED_LOSS_APPLIED event was observed.
    expected_sequences = sorted(expected_forced_sequences(config))
    observed = sorted(
        seq for seq in (as_int(event.get("fragment_sequence")) for event in armed_events)
        if seq is not None
    )
    valid = observed == expected_sequences
    return (
        "valid" if valid else "invalid",
        len(armed_events),
        len(applied_events),
        "legacy_unverified" if valid else "invalid",
    )


def parse_log(
    log_path: Path,
    config: dict[str, Any],
    status: dict[str, Any],
) -> tuple[dict[str, Any], list[dict[str, Any]], list[str]]:
    text = log_path.read_text(encoding="utf-8", errors="replace")
    events: list[dict[str, Any]] = []
    buckets: dict[tuple[str, str], dict[str, str]] = {}
    warnings: list[str] = []
    forced_events: list[dict[str, Any]] = []
    forced_applied_events: list[dict[str, Any]] = []
    pk_attempts: list[dict[str, Any]] = []
    parsed_until_line: int | None = None
    parser_stop_reason = "end_of_log"
    client_done_success: int | None = None
    client_end_reason: str | None = None

    for line_number, raw in enumerate(text.splitlines(), 1):
        # EXPERIMENT_DONE_CLIENT is deliberately parsed separately from EVENT_RE.
        # We retain failures (notably route_acquisition_timeout) for diagnosis,
        # while only a client-reported success stops parsing.
        client_done_match = CLIENT_DONE_RE.search(raw)
        if client_done_match:
            client_done_success = int(client_done_match.group("success"))
            client_end_reason = client_done_match.group("reason") or client_end_reason

        match = EVENT_RE.search(raw)
        if match:
            marker, tail = match.groups()
            values = parse_kv(tail)
            role = values.get("role", "")
            events.append({
                "line_number": line_number,
                "marker": marker,
                "role": role,
                "raw": raw,
            })
            if marker == "FORCED_LOSS":
                forced_events.append({
                    "line_number": line_number,
                    **{key: typed_value(value) for key, value in values.items()},
                })
                continue
            if marker == "FORCED_LOSS_APPLIED":
                forced_applied_events.append({
                    "line_number": line_number,
                    **{key: typed_value(value) for key, value in values.items()},
                })
                continue
            if marker == "PK_ATTEMPT":
                pk_attempts.append({
                    "line_number": line_number,
                    **{key: typed_value(value) for key, value in values.items()},
                })
                continue
            key = (marker, role)
            if key in buckets:
                warnings.append(
                    f"événement dupliqué {marker}/{role or '-'}; dernière valeur retenue"
                )
            buckets[key] = values
            continue

        # Backward-compatible parser for the older unstructured test hook.
        forced_match = FORCED_DATA_RE.search(raw)
        if forced_match:
            data = forced_match.groupdict()
            forced_events.append({
                "line_number": line_number,
                "pk_attempt": 1,
                "forced_loss_injected": "",
                "forced_loss_layer": "receiver_after_channel",
                "fragment_sequence": int(data["seq"]),
                "tag": int(data["tag"]),
                "occurrence": int(data["occurrence"]),
                "channel": "normal_then_receiver_drop",
            })
            events.append({
                "line_number": line_number,
                "marker": "FORCED_LOSS",
                "role": "receiver",
                "raw": raw,
            })

        # The client is the sole authority for successful completion.
        # RESULT and RESULT_NET are emitted before EXPERIMENT_DONE_CLIENT in
        # client.c, so all mandatory client-side metrics have already been
        # captured when this marker is observed.
        if client_done_match and client_done_success == 1:
            parsed_until_line = line_number
            parser_stop_reason = "client_reported_success"
            events.append({
                "line_number": line_number,
                "marker": "EXPERIMENT_DONE_CLIENT_SUCCESS",
                "role": "client",
                "raw": raw,
            })
            break

    row: dict[str, Any] = dict(config)
    row.update({
        "raw_log_sha256": sha256_file(log_path),
        "log_size_bytes": log_path.stat().st_size,
        "log_line_count": len(text.splitlines()),
        "parsed_until_line": (
            parsed_until_line if parsed_until_line is not None
            else len(text.splitlines())
        ),
        "parser_stop_reason": parser_stop_reason,
        "client_end_reason": client_end_reason,
        "client_done_success": client_done_success,
        "cooja_exit_code": status.get("exit_code"),
        "cooja_timed_out": bool(status.get("timed_out")),
    })

    mapping = [
        (("RESULT", "client"), "client"),
        (("RESULT_NET", "client"), "client_net"),
        (("RESULT_SERVER", "server"), "server"),
        (("RESULT_NET_SERVER", "server"), "server_net"),
        (("EXPERIMENT_CONFIG", "client"), "client_config"),
        (("EXPERIMENT_CONFIG", "server"), "server_config"),
    ]
    # Only client termination and client network counters are required for the
    # principal outcome. Server results are diagnostic and may arrive later.
    required = {
        ("RESULT", "client"),
        ("RESULT_NET", "client"),
    }
    for key, prefix in mapping:
        values = buckets.get(key)
        if values is None:
            if key in required:
                warnings.append(f"événement requis absent: {key[0]}/{key[1]}")
            continue
        prefixed(row, prefix, values)

    expected = KEM_SIZES.get(int(config["kem_level"]), {})
    for field, expected_field in (
        ("client_pk_size", "pk_size"),
        ("client_ct_size", "ct_size"),
        ("client_pk_frags_expected", "pk_frags"),
        ("client_ct_frags_expected", "ct_frags"),
    ):
        value = row.get(field)
        if value is not None and value != expected.get(expected_field):
            warnings.append(
                f"{field}={value} mais attendu={expected.get(expected_field)}"
            )

    for field in (
        "client_variant",
        "server_variant",
        "client_config_variant",
        "server_config_variant",
    ):
        if row.get(field) is not None and int(row[field]) != int(config["variant"]):
            warnings.append(f"{field}={row[field]} diffère de la variante configurée")
    for field in (
        "client_kem",
        "server_kem",
        "client_config_kem",
        "server_config_kem",
    ):
        if row.get(field) is not None and int(row[field]) != int(config["kem_level"]):
            warnings.append(f"{field}={row[field]} diffère du KEM configuré")

    timed_out = bool(status.get("timed_out")) or (
        "PIPELINE_EVENT,SIMULATION_TIMEOUT=1" in text
        or "PIPELINE_EVENT,EXTERNAL_TIMEOUT=1" in text
    )
    row["cooja_timed_out"] = timed_out
    exit_code = status.get("exit_code")

    client_success = as_int(row.get("client_success"))
    server_success = as_int(row.get("server_success"))

    # Single source of truth for experimental success: the client.
    row["run_success"] = client_success if client_success in (0, 1) else None
    # Backward-compatible alias, with exactly the same client-based semantics.
    row["exchange_success"] = row["run_success"]
    row["server_completion_observed"] = (
        server_success if server_success in (0, 1) else None
    )
    row["client_server_disagreement"] = int(
        client_success in (0, 1)
        and server_success in (0, 1)
        and client_success != server_success
    )
    # A disagreement is diagnostic only; it is intentionally not a warning.

    # Canonical analysis metrics, explicitly separating recovery layers.
    row["latency_ms"] = row.get("client_rtt_ms")
    row["application_retries"] = as_int(row.get("client_app_retries")) or 0

    client_selective = as_int(
        row.get("client_net_selective_fragment_retransmissions")
    )
    server_selective = as_int(
        row.get("server_net_selective_fragment_retransmissions")
    )
    if client_selective is not None and server_selective is not None:
        row["selective_fragment_retransmissions"] = (
            client_selective + server_selective
        )
        row["selective_fragment_retransmissions_scope"] = "both_motes"
    else:
        # Do not invent a server-side zero when Cooja was intentionally
        # stopped by the client success marker.
        row["selective_fragment_retransmissions"] = None
        row["selective_fragment_retransmissions_scope"] = (
            "client_only_available" if client_selective is not None
            else "unavailable"
        )

    client_fragments = as_int(row.get("client_net_data_frag_tx"))
    server_fragments = as_int(row.get("server_net_data_frag_tx"))
    row["client_data_frag_tx"] = client_fragments
    row["server_data_frag_tx"] = server_fragments
    row["total_fragments_tx"] = (
        client_fragments + server_fragments
        if client_fragments is not None and server_fragments is not None
        else None
    )
    row["fragments_tx_scope"] = (
        "both_motes"
        if client_fragments is not None and server_fragments is not None
        else "client_only_available"
        if client_fragments is not None
        else "unavailable"
    )

    def sum_optional(client_field: str, server_field: str) -> int | None:
        client_value = as_int(row.get(client_field))
        server_value = as_int(row.get(server_field))
        if client_value is None or server_value is None:
            return None
        return client_value + server_value

    row["rfrag_retransmissions"] = sum_optional(
        "client_net_rfrag_retx_tx", "server_net_rfrag_retx_tx"
    )
    row["rfrag_rto_expired"] = sum_optional(
        "client_net_rto_expired", "server_net_rto_expired"
    )
    row["rfrag_null_ack_rx"] = sum_optional(
        "client_net_null_ack_rx", "server_net_null_ack_rx"
    )
    row["rfrag_partial_ack_rx"] = sum_optional(
        "client_net_partial_ack_rx", "server_net_partial_ack_rx"
    )
    row["rfrag_ack_tx"] = sum_optional(
        "client_net_rfrag_ack_tx", "server_net_rfrag_ack_tx"
    )

    configured_forced_loss = str(config.get("forced_loss") or "none")
    client_app_attempts = as_int(row.get("client_app_attempts"))

    # A targeted forced-loss experiment cannot be exercised if routing never
    # becomes available and the client therefore sends no PK at all.  This is
    # not an invalid injection: the experimental precondition was never
    # reached.  Keep the run for traceability, but mark it explicitly so that
    # downstream V1/V2 analysis can exclude it from recovery-performance
    # statistics without deleting the run.
    precondition_not_reached = (
        configured_forced_loss != "none"
        and client_done_success == 0
        and client_end_reason == "route_acquisition_timeout"
        and (client_app_attempts or 0) == 0
        and not pk_attempts
        and not forced_events
        and not forced_applied_events
    )

    if precondition_not_reached:
        forced_validation = "not_reached"
        armed_count = 0
        applied_count = 0
        forced_application = "precondition_not_reached"
    else:
        forced_validation, armed_count, applied_count, forced_application = (
            validate_forced_loss_events(
                config, forced_events, forced_applied_events, pk_attempts
            )
        )

    # Backward-compatible count: number of target frames armed at the sender.
    row["forced_loss_injection_count"] = armed_count
    row["forced_loss_armed_count"] = armed_count
    row["forced_loss_applied_count"] = applied_count
    row["forced_loss_application"] = forced_application
    row["forced_loss_validation"] = forced_validation
    row["precondition_not_reached"] = int(precondition_not_reached)
    row["targeted_test_reached"] = (
        0 if precondition_not_reached
        else 1 if configured_forced_loss != "none" and forced_validation == "valid"
        else None
    )

    if configured_forced_loss != "none":
        if forced_validation == "invalid":
            warnings.append(
                "perte forcée configurée mais cible/événements observés incohérents"
            )
        elif forced_validation == "not_reached":
            # Expected classification when route acquisition fails before any
            # PK attempt.  This is not a parser warning or an injection error.
            pass
        elif forced_application == "radio_preempted":
            warnings.append(
                "perte ciblée armée, mais le canal/radio a perdu la trame avant le hook receiver"
            )
        elif forced_application == "legacy_unverified":
            warnings.append(
                "ancien log: application receiver-after-channel non vérifiable explicitement"
            )

    if client_success in (0, 1):
        result_class = "SUCCESS" if client_success == 1 else "FAILED_PROTOCOL"
        result_basis = (
            "client_reported_success" if client_success == 1
            else "client_reported_failure"
        )
    elif timed_out:
        result_class = "SIMULATION_TIMEOUT"
        result_basis = "cooja_timeout"
    elif exit_code not in (0, None):
        result_class = "COOJA_ERROR"
        result_basis = "cooja_exit_code"
    else:
        result_class = "INCOMPLETE"
        result_basis = "missing_client_result"

    row["result_class"] = result_class
    row["result_basis"] = result_basis
    row["parse_status"] = "success" if not warnings else "success_with_warnings"
    row["parser_warnings"] = " | ".join(warnings)
    return row, events, warnings


def parse_run(run_dir: Path) -> dict[str, Any]:
    log_path = run_dir / "cooja.log"
    if not log_path.is_file() or log_path.stat().st_size == 0:
        raise FileNotFoundError(f"Log Cooja absent ou vide: {log_path}")
    config = read_json(run_dir / "run_config.json", {})
    status = read_json(run_dir / "status.json", {})
    if not config.get("run_id"):
        raise ValueError(f"run_config.json invalide dans {run_dir}")
    row, events, warnings = parse_log(log_path, config, status)
    write_csv(run_dir / "results.csv", [row], list(row))
    write_csv(
        run_dir / "parsed_events.csv",
        events,
        ["line_number", "marker", "role", "raw"],
    )
    status.update({
        "parse_status": row["parse_status"],
        "result_class": row["result_class"],
        "run_success": row["run_success"],
        "timed_out": bool(row["cooja_timed_out"]),
        "raw_log_sha256": row["raw_log_sha256"],
        "parser_warning_count": len(warnings),
    })
    write_json(run_dir / "status.json", status)
    return row


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--run-dir", type=Path, required=True)
    args = parser.parse_args()
    parse_run(args.run_dir.resolve())
