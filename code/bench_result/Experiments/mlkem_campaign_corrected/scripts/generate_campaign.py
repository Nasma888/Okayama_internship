from __future__ import annotations

import argparse
import csv
from pathlib import Path


FIXED_PRRS = (1.00, 0.95, 0.90, 0.85, 0.50)
GILBERT_PRRS = (0.95, 0.90, 0.85, 0.50)
GILBERT_BURSTS = (2.0, 5.0, 10.0)
KEMS = (512, 768, 1024)
KEM_PK_FRAGMENTS = {512: 9, 768: 13, 1024: 17}

VOLET_A_VARIANTS = (
    (1, "B1_RFC4944_NO_RECOVERY", 0),
    (2, "B2_RFRAG_FINAL_ACK", 0),
)
VOLET_B_VARIANTS = (
    (3, "V1_RFRAG_INITIAL_FINAL_ACK", 0),
    (4, "V2_RFRAG_FINAL_ACK_APP_RETRY", 1),
)
# Targeted study: V1 and V2 face exactly the same first-fragment loss.
# This campaign validates the additional recovery variants only.
FORCED_LOSSES = (
    ("first_fragment", 1),
)

FIELDS = [
    "run_id",
    "paired_group",
    "pair_group_id",
    "comparison_arm",
    "campaign",
    "model",
    "variant",
    "variant_name",
    "kem_level",
    "target_prr",
    "mean_bad_burst",
    "p_gb",
    "p_bg",
    "forced_loss",
    "forced_loss_mode",
    "forced_loss_layer",
    "forced_loss_after_radio_channel",
    "forced_loss_source_id",
    "forced_loss_destination_id",
    "forced_loss_target_tag",
    "forced_loss_last_sequence",
    "rfrag_fragment_size",
    "seed",
    "repetition",
    "timeout_s",
    "mac_max_retries",
    "max_application_retries",
    "data_origin",
]


def channel_slug(condition: dict[str, object]) -> str:
    prr = f"{float(condition['target_prr']):.2f}".replace(".", "p")
    if condition["model"] == "fixed":
        return f"fixed-prr{prr}"
    burst = f"{float(condition['mean_bad_burst']):g}".replace(".", "p")
    return f"gilbert-prr{prr}-L{burst}"


def channel_conditions() -> list[dict[str, object]]:
    conditions: list[dict[str, object]] = []
    for prr in FIXED_PRRS:
        conditions.append({
            "model": "fixed",
            "target_prr": prr,
            "mean_bad_burst": "",
            "p_gb": "",
            "p_bg": "",
        })
    for prr in GILBERT_PRRS:
        for burst in GILBERT_BURSTS:
            p_bg = 1.0 / burst
            p_gb = p_bg * (1.0 - prr) / prr
            conditions.append({
                "model": "gilbert",
                "target_prr": prr,
                "mean_bad_burst": burst,
                "p_gb": f"{p_gb:.10f}",
                "p_bg": f"{p_bg:.10f}",
            })
    return conditions


def common_row(
    *,
    kem: int,
    condition: dict[str, object],
    seed: int,
    repetition: int,
    timeout_s: int,
) -> dict[str, object]:
    return {
        **condition,
        "kem_level": kem,
        "forced_loss_source_id": 2,
        "forced_loss_destination_id": 1,
        "forced_loss_target_tag": 0,
        "forced_loss_last_sequence": KEM_PK_FRAGMENTS[kem] - 1,
        "rfrag_fragment_size": 96,
        "seed": seed,
        "repetition": repetition,
        "timeout_s": timeout_s,
        "mac_max_retries": 0,
        "data_origin": "COOJA_SIMULATION",
    }


def build_rows(
    repetitions: int,
    timeout_a_s: int,
    timeout_b_s: int,
    seed_base: int = 200000,
) -> list[dict[str, object]]:
    """Build the two paired studies.

    A seed is deliberately shared by all variants inside one paired_group.
    This isolates the recovery mechanism from radio RNG differences.
    """
    rows: list[dict[str, object]] = []

    for kem in KEMS:
        for condition in channel_conditions():
            condition_id = channel_slug(condition)

            # Volet A: B1 vs B2, no forced loss.
            for repetition in range(1, repetitions + 1):
                seed = seed_base + repetition
                paired_group = (
                    f"A-kem{kem}-{condition_id}-rep{repetition:02d}-seed{seed}"
                )
                for variant, variant_name, app_retries in VOLET_A_VARIANTS:
                    rows.append({
                        **common_row(
                            kem=kem,
                            condition=condition,
                            seed=seed,
                            repetition=repetition,
                            timeout_s=timeout_a_s,
                        ),
                        "run_id": f"{paired_group}-v{variant}",
                        "paired_group": paired_group,
                        "pair_group_id": paired_group,
                        "comparison_arm": "B1" if variant == 1 else "B2",
                        "campaign": "A_B1_VS_B2",
                        "variant": variant,
                        "variant_name": variant_name,
                        "forced_loss": "none",
                        "forced_loss_mode": 0,
                        "forced_loss_layer": "none",
                        "forced_loss_after_radio_channel": 0,
                        "max_application_retries": app_retries,
                    })

            # Volet B: same radio condition/seed for V1 and V2 under a targeted first-fragment loss.
            for forced_loss, forced_mode in FORCED_LOSSES:
                for repetition in range(1, repetitions + 1):
                    seed = seed_base + repetition
                    paired_group = (
                        f"B-kem{kem}-{condition_id}-loss{forced_mode}"
                        f"-rep{repetition:02d}-seed{seed}"
                    )
                    for variant, variant_name, app_retries in VOLET_B_VARIANTS:
                        rows.append({
                            **common_row(
                                kem=kem,
                                condition=condition,
                                seed=seed,
                                repetition=repetition,
                                timeout_s=timeout_b_s,
                            ),
                            "run_id": f"{paired_group}-v{variant}",
                            "paired_group": paired_group,
                            "pair_group_id": paired_group,
                            "comparison_arm": "V1" if variant == 3 else "V2",
                            "campaign": "B_V1_V2_FIRST_LOSS",
                            "variant": variant,
                            "variant_name": variant_name,
                            "forced_loss": forced_loss,
                            "forced_loss_mode": forced_mode,
                            "forced_loss_layer": "receiver_after_channel",
                            "forced_loss_after_radio_channel": 1,
                            "max_application_retries": app_retries,
                        })
    return rows


def main() -> None:
    parser = argparse.ArgumentParser(
        description=(
            "Génère les volets appariés A (B1/B2) et B "
            "(V1/V2 avec perte forcée du premier fragment)."
        )
    )
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--repetitions", type=int, default=1)
    parser.add_argument("--timeout-a-s", type=int, default=240)
    parser.add_argument("--timeout-b-s", type=int, default=360)
    parser.add_argument("--seed-base", type=int, default=200000)
    # Backward compatibility with the former CLI.
    parser.add_argument("--timeout-s", type=int)
    args = parser.parse_args()

    if args.repetitions < 1:
        raise SystemExit("--repetitions doit être >= 1")
    if args.seed_base < 0:
        raise SystemExit("--seed-base doit être >= 0")

    timeout_a = args.timeout_s or args.timeout_a_s
    timeout_b = args.timeout_s or args.timeout_b_s
    rows = build_rows(args.repetitions, timeout_a, timeout_b, args.seed_base)

    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(handle, fieldnames=FIELDS)
        writer.writeheader()
        writer.writerows(rows)
    print(f"{len(rows)} runs appariés écrits dans {args.output}")


if __name__ == "__main__":
    main()
