from __future__ import annotations

import argparse
from pathlib import Path

from aggregate_results import rebuild
from common import (
    KEM_SIZES,
    VARIANT_NAMES,
    normalize_experiment,
    run_directory,
    utc_now,
    write_csv,
    write_json,
)
from parse_cooja_log import parse_run
from validate_results import validate, write_report


def fixture_log(exp: dict, success: int) -> str:
    sizes = KEM_SIZES[exp["kem_level"]]
    variant = exp["variant"]
    rfrag = int(variant in {2, 3, 4})
    ack_first = int(variant in {3, 4})
    idle_ack = int(variant == 4)
    app_retry = int(variant == 5)
    retries = 1 if variant in {2, 3, 4, 5} else 0
    attempts = 1 + retries
    rtt_ms = 800 + exp["kem_level"] + variant * 37
    client_tx = sizes["pk_frags"] + retries
    client_rx = sizes["ct_frags"] if success else 0
    server_tx = sizes["ct_frags"]
    server_rx = sizes["pk_frags"]
    rfrag_ack = 2 if rfrag else 0
    end_reason = (
        "rfrag_full_ack_mac_ok" if rfrag
        else "ct_delivered_rfc4944"
    )
    return f"""PARSER_FIXTURE_SYNTHETIC,not_a_scientific_measurement=1
00:00.001 ID:2 EXPERIMENT_CONFIG,role=client,variant={variant},variant_name={VARIANT_NAMES[variant]},kem={exp['kem_level']},kem_name=MLKEM{exp['kem_level']},seed={exp['seed']},rfrag={rfrag},ack_first={ack_first},idle_ack={idle_ack},app_retry={app_retry},max_app_retries={3 if app_retry else 0},energest=1,energy_model=Z1_CC2420_APPROX
00:00.002 ID:1 EXPERIMENT_CONFIG,role=server,variant={variant},variant_name={VARIANT_NAMES[variant]},kem={exp['kem_level']},kem_name=MLKEM{exp['kem_level']},seed={exp['seed']},rfrag={rfrag},ack_first={ack_first},idle_ack={idle_ack},app_retry={app_retry},energest=1,energy_boundary=sync_T0,exchange_T0_ticks=3840
00:02.000 ID:2 RESULT_ENERGY_EXCHANGE,role=client,outcome={'success' if success else 'failure'},end_reason={end_reason},variant={variant},kem={exp['kem_level']},seed={exp['seed']},model=Z1_CC2420_APPROX,voltage_mV=3000,energest_second=32768,window_ticks=3000,cpu_ticks=2900,lpm_ticks=100,deep_lpm_ticks=0,tx_ticks={300 + client_tx},rx_ticks={400 + client_rx},cpu_energy_uJ=1141,lpm_energy_uJ=0,deep_lpm_energy_uJ=0,tx_energy_uJ=160,rx_energy_uJ=230,total_energy_uJ=1531,cpu_partition_valid=1,radio_partition_valid=1,absolute_node_energy_valid=1
00:02.001 ID:2 RESULT,role=client,variant={variant},variant_name={VARIANT_NAMES[variant]},kem={exp['kem_level']},kem_name=MLKEM{exp['kem_level']},seed={exp['seed']},success={success},seq=0,pk_size={sizes['pk_size']},ct_size={sizes['ct_size']},pk_frags_expected={sizes['pk_frags']},ct_frags_expected={sizes['ct_frags']},app_attempts={attempts},app_retries={retries},app_timeouts={0 if success else 1},rtt_valid={success},rtt_ticks={rtt_ms},rtt_ms={rtt_ms if success else 0},elapsed_ticks={rtt_ms},elapsed_ms={rtt_ms},energy_end_reason={end_reason}
00:02.002 ID:2 RESULT_NET,role=client,variant={variant},kem={exp['kem_level']},seed={exp['seed']},datagram_tx={attempts},datagram_rx_complete={success},data_frag_tx={client_tx},data_frag_rx={client_rx},data_bytes_tx={sizes['pk_size'] * attempts},data_bytes_rx={sizes['ct_size'] if success else 0},rfrag_retx_tx={retries if rfrag else 0},rfrag_ack_tx={rfrag_ack},rfrag_ack_rx={rfrag_ack},rfrag_ack_tx_ok={rfrag_ack},rfrag_full_ack_tx_done={success if rfrag else 0},rfrag_full_ack_tx_ok={success if rfrag else 0},idle_ack_tx={1 if idle_ack else 0},rto_expired={retries if rfrag else 0},null_ack_rx=0,partial_ack_rx={retries if rfrag else 0},full_ack_rx={success if rfrag else 0},ctx_create={rfrag},ctx_complete={success if rfrag else 0},ctx_timeout_incomplete={0 if success else rfrag},ctx_timeout_complete=0,ctx_alloc_fail=0,ctx_active_now=0,ctx_active_peak={rfrag},ctx_ticks_closed={100 if rfrag else 0}
00:02.003 ID:1 RESULT_ENERGY_EXCHANGE,role=server,outcome={'success' if success else 'failure'},end_reason={'rfrag_full_ack_received' if rfrag else 'rfc4944_final_fragment_mac_ok'},variant={variant},kem={exp['kem_level']},seed={exp['seed']},model=Z1_CC2420_APPROX,voltage_mV=3000,energest_second=32768,window_ticks=3050,cpu_ticks=2950,lpm_ticks=100,deep_lpm_ticks=0,tx_ticks={350 + server_tx},rx_ticks={450 + server_rx},cpu_energy_uJ=1160,lpm_energy_uJ=0,deep_lpm_energy_uJ=0,tx_energy_uJ=170,rx_energy_uJ=240,total_energy_uJ=1570,cpu_partition_valid=1,radio_partition_valid=1,absolute_node_energy_valid=1
00:02.004 ID:1 RESULT_SERVER,role=server,variant={variant},variant_name={VARIANT_NAMES[variant]},kem={exp['kem_level']},kem_name=MLKEM{exp['kem_level']},seed={exp['seed']},success={success},end_reason={'rfrag_full_ack_received' if rfrag else 'rfc4944_final_fragment_mac_ok'},energy_window_aligned=1,pk_valid_rx=1,pk_duplicate_rx={retries if app_retry else 0},pk_invalid_rx=0,ct_tx={attempts if app_retry else 1},last_seq=0,last_one_way_ticks=400,last_one_way_ms=400
00:02.005 ID:1 RESULT_NET_SERVER,role=server,variant={variant},kem={exp['kem_level']},seed={exp['seed']},datagram_tx=1,datagram_rx_complete=1,data_frag_tx={server_tx},data_frag_rx={server_rx},data_bytes_tx={sizes['ct_size']},data_bytes_rx={sizes['pk_size']},rfrag_retx_tx={retries if rfrag else 0},rfrag_ack_tx={rfrag_ack},rfrag_ack_rx={rfrag_ack},rfrag_ack_tx_done={rfrag_ack},rfrag_ack_tx_ok={rfrag_ack},rfrag_full_ack_tx_done={success if rfrag else 0},rfrag_full_ack_tx_ok={success if rfrag else 0},rfrag_full_ack_rx={success if rfrag else 0},rfc4944_final_tx_done={0 if rfrag else 1},rfc4944_final_tx_ok={0 if rfrag else 1},idle_ack_tx={1 if idle_ack else 0},rto_expired={retries if rfrag else 0},null_ack_rx=0,partial_ack_rx={retries if rfrag else 0},ctx_create={rfrag},ctx_complete={success if rfrag else 0},ctx_timeout_incomplete={0 if success else rfrag},ctx_timeout_complete=0,ctx_alloc_fail=0,ctx_active_now=0,ctx_active_peak={rfrag},ctx_ticks_closed={100 if rfrag else 0}
EXPERIMENT_DONE_CLIENT,success={success},reason={end_reason}
EXPERIMENT_DONE_SERVER,success={success},reason=fixture
EXPERIMENT_DONE_BOTH
"""


def main() -> None:
    parser = argparse.ArgumentParser()
    parser.add_argument("--output-root", type=Path, required=True)
    args = parser.parse_args()
    root = args.output_root.resolve()
    campaign_rows = []
    for kem in (512, 768, 1024):
        for variant in (1, 2, 3, 4, 5):
            row = {
                "model": "fixed",
                "variant": variant,
                "kem_level": kem,
                "target_prr": 0.90,
                "mean_bad_burst": "",
                "p_gb": "",
                "p_bg": "",
                "seed": 900000 + kem + variant,
                "repetition": 1,
                "timeout_s": 240,
                "mac_max_retries": 0,
                "data_origin": "SYNTHETIC_PARSER_FIXTURE",
            }
            campaign_rows.append(row)
            exp = normalize_experiment(row)
            exp["date"] = utc_now()
            run_dir = run_directory(root / "runs", exp)
            run_dir.mkdir(parents=True, exist_ok=True)
            write_json(run_dir / "run_config.json", exp)
            write_json(run_dir / "status.json", {
                "run_id": exp["run_id"],
                "exit_code": 0,
                "timed_out": False,
                "parse_status": "not_parsed",
            })
            success = 0 if variant == 2 else 1
            (run_dir / "cooja.log").write_text(
                fixture_log(exp, success),
                encoding="utf-8",
            )
            parse_run(run_dir)
    campaign = root / "proof_experiments.csv"
    write_csv(campaign, campaign_rows, list(campaign_rows[0]))
    rebuild(root)
    issues = validate(root)
    write_report(root, issues)
    print(f"Preuve parseur créée: {root}")


if __name__ == "__main__":
    main()
