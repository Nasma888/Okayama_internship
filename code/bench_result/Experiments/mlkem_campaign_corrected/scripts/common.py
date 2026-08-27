from __future__ import annotations

import csv
import hashlib
import json
import math
import re
from datetime import datetime, timezone
from pathlib import Path
from typing import Any, Iterable


VARIANT_NAMES = {
    1: "B1_RFC4944_NO_RECOVERY",
    2: "B2_RFRAG_FINAL_ACK",
    3: "V1_RFRAG_INITIAL_FINAL_ACK",
    4: "V2_RFRAG_FINAL_ACK_APP_RETRY",
}
VARIANT_ARMS = {
    1: "B1",
    2: "B2",
    3: "V1",
    4: "V2",
}
KEM_SIZES = {
    512: {"pk_size": 800, "ct_size": 768, "pk_frags": 9, "ct_frags": 9},
    768: {"pk_size": 1184, "ct_size": 1088, "pk_frags": 13, "ct_frags": 12},
    1024: {"pk_size": 1568, "ct_size": 1568, "pk_frags": 17, "ct_frags": 17},
}
FORCED_LOSS_MODES = {
    "none": 0,
    "first_fragment": 1,
}
SAFE_IDENTIFIER = re.compile(r"^[A-Za-z0-9_.-]+$")


def utc_now() -> str:
    return datetime.now(timezone.utc).isoformat(timespec="seconds")


def read_json(path: Path, default: Any = None) -> Any:
    if not path.is_file():
        return default
    return json.loads(path.read_text(encoding="utf-8"))


def write_json(path: Path, value: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        json.dumps(value, indent=2, ensure_ascii=False) + "\n",
        encoding="utf-8",
    )


def read_csv(path: Path) -> list[dict[str, str]]:
    with path.open(newline="", encoding="utf-8-sig") as handle:
        return list(csv.DictReader(handle))


def write_csv(
    path: Path,
    rows: Iterable[dict[str, Any]],
    fields: list[str],
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    with path.open("w", newline="", encoding="utf-8") as handle:
        writer = csv.DictWriter(
            handle,
            fieldnames=fields,
            extrasaction="ignore",
        )
        writer.writeheader()
        writer.writerows(rows)


def opt_float(value: Any) -> float | None:
    if value is None or str(value).strip() == "":
        return None
    number = float(value)
    if not math.isfinite(number):
        raise ValueError(f"Valeur non finie: {value}")
    return number


def opt_int(value: Any) -> int | None:
    if value is None or str(value).strip() == "":
        return None
    return int(float(value))


def slug_number(value: float | None, digits: int) -> str:
    if value is None:
        return "na"
    return f"{value:.{digits}f}".replace(".", "p")


def normalize_experiment(row: dict[str, Any]) -> dict[str, Any]:
    model = str(row.get("model", "")).strip().lower()
    if model not in {"fixed", "gilbert"}:
        raise ValueError(f"Modèle inconnu: {model!r}")

    variant = int(row["variant"])
    kem_level = int(row["kem_level"])
    if variant not in VARIANT_NAMES:
        raise ValueError(f"Variante invalide: {variant}")
    if kem_level not in KEM_SIZES:
        raise ValueError(f"Niveau ML-KEM invalide: {kem_level}")

    target_prr = opt_float(row.get("target_prr") or row.get("prr"))
    mean_bad_burst = opt_float(row.get("mean_bad_burst"))
    p_gb = opt_float(row.get("p_gb"))
    p_bg = opt_float(row.get("p_bg"))

    if model == "fixed":
        if target_prr is None or not 0 <= target_prr <= 1:
            raise ValueError("Le modèle fixed exige target_prr dans [0,1]")
        mean_bad_burst = None
        p_gb = None
        p_bg = None
    else:
        if p_gb is None or p_bg is None:
            if target_prr is None or not 0 < target_prr <= 1:
                raise ValueError("Gilbert exige target_prr dans ]0,1]")
            if mean_bad_burst is None or mean_bad_burst < 1:
                raise ValueError("Gilbert exige mean_bad_burst >= 1")
            p_bg = 1.0 / mean_bad_burst
            p_gb = p_bg * (1.0 - target_prr) / target_prr
        if not (0 <= p_gb <= 1 and 0 < p_bg <= 1):
            raise ValueError("p_gb doit être dans [0,1] et p_bg dans ]0,1]")
        stationary_prr = p_bg / (p_gb + p_bg)
        if target_prr is None:
            target_prr = stationary_prr
        if abs(stationary_prr - target_prr) > 1e-6:
            raise ValueError(
                "Incohérence Gilbert: p_gb/p_bg ne donnent pas target_prr"
            )
        if mean_bad_burst is None:
            mean_bad_burst = 1.0 / p_bg

    seed = int(row["seed"])
    repetition = int(row.get("repetition") or 1)
    timeout_s = int(row.get("timeout_s") or 240)
    mac_max_retries = int(row.get("mac_max_retries") or 0)
    if seed < 0 or repetition < 1 or timeout_s < 1:
        raise ValueError("seed, repetition ou timeout_s invalide")
    if mac_max_retries < 0:
        raise ValueError("mac_max_retries doit être >= 0")

    forced_loss = str(row.get("forced_loss") or "none").strip()
    if forced_loss not in FORCED_LOSS_MODES:
        raise ValueError(f"Scénario de perte forcée inconnu: {forced_loss!r}")
    forced_loss_mode = opt_int(row.get("forced_loss_mode"))
    if forced_loss_mode is None:
        forced_loss_mode = FORCED_LOSS_MODES[forced_loss]
    if forced_loss_mode != FORCED_LOSS_MODES[forced_loss]:
        raise ValueError(
            "forced_loss_mode incohérent avec forced_loss: "
            f"{forced_loss_mode} != {FORCED_LOSS_MODES[forced_loss]}"
        )

    # Les pertes forcées de la campagne ciblée concernent uniquement V1 et V2.
    if forced_loss != "none" and variant not in {3, 4}:
        raise ValueError(
            "Les pertes forcées ne sont autorisées que pour V1 et V2"
        )

    forced_loss_layer = str(
        row.get("forced_loss_layer")
        or ("none" if forced_loss == "none" else "receiver_after_channel")
    ).strip()
    after_channel = opt_int(row.get("forced_loss_after_radio_channel"))
    if after_channel is None:
        after_channel = int(forced_loss != "none")
    if forced_loss != "none" and after_channel != 1:
        raise ValueError("Les pertes forcées doivent être injectées après le canal radio")

    max_application_retries = opt_int(row.get("max_application_retries"))
    if max_application_retries is None:
        max_application_retries = 1 if variant == 4 else 0
    if max_application_retries < 0:
        raise ValueError("max_application_retries doit être >= 0")
    if variant == 4 and max_application_retries < 1:
        raise ValueError("V2 doit autoriser une retransmission applicative complète")
    if variant in {1, 2, 3} and max_application_retries != 0:
        raise ValueError("Seule V2 doit autoriser une retransmission applicative")

    if model == "fixed":
        channel_slug = f"fixed_prr{slug_number(target_prr, 2)}"
    else:
        channel_slug = (
            f"gilbert_prr{slug_number(target_prr, 2)}"
            f"_L{slug_number(mean_bad_burst, 1)}"
        )
    loss_slug = "" if forced_loss == "none" else f"_loss{forced_loss_mode}"
    generated_run_id = (
        f"kem{kem_level}_v{variant}_{channel_slug}{loss_slug}"
        f"_seed{seed}_rep{repetition:02d}"
    )
    run_id = str(row.get("run_id") or generated_run_id).strip()
    if not SAFE_IDENTIFIER.fullmatch(run_id):
        raise ValueError(f"run_id non sûr: {run_id!r}")

    paired_group = str(
        row.get("paired_group") or row.get("pair_group_id") or ""
    ).strip()
    if paired_group and not SAFE_IDENTIFIER.fullmatch(paired_group):
        raise ValueError(f"paired_group non sûr: {paired_group!r}")

    return {
        "run_id": run_id,
        "paired_group": paired_group,
        "pair_group_id": paired_group,
        "comparison_arm": str(
            row.get("comparison_arm") or VARIANT_ARMS[variant]
        ).strip(),
        "campaign": str(row.get("campaign") or "").strip(),
        "data_origin": str(row.get("data_origin") or "COOJA_SIMULATION"),
        "model": model,
        "target_prr": target_prr,
        "mean_bad_burst": mean_bad_burst,
        "p_gb": p_gb,
        "p_bg": p_bg,
        "variant": variant,
        "variant_name": VARIANT_NAMES[variant],
        "kem_level": kem_level,
        "forced_loss": forced_loss,
        "forced_loss_mode": forced_loss_mode,
        "forced_loss_layer": forced_loss_layer,
        "forced_loss_after_radio_channel": after_channel,
        "forced_loss_source_id": opt_int(row.get("forced_loss_source_id")) or 2,
        "forced_loss_destination_id": opt_int(row.get("forced_loss_destination_id")) or 1,
        "forced_loss_target_tag": (
            opt_int(row.get("forced_loss_target_tag"))
            if row.get("forced_loss_target_tag") not in (None, "")
            else 0
        ),
        "forced_loss_last_sequence": (
            opt_int(row.get("forced_loss_last_sequence"))
            if row.get("forced_loss_last_sequence") not in (None, "")
            else KEM_SIZES[kem_level]["pk_frags"] - 1
        ),
        "rfrag_fragment_size": opt_int(row.get("rfrag_fragment_size")) or 96,
        "seed": seed,
        "repetition": repetition,
        "timeout_s": timeout_s,
        "mac_max_retries": mac_max_retries,
        "max_application_retries": max_application_retries,
    }


def run_directory(base: Path, exp: dict[str, Any]) -> Path:
    run_id = str(exp.get("run_id") or "")
    if run_id:
        if not SAFE_IDENTIFIER.fullmatch(run_id):
            raise ValueError(f"run_id non sûr: {run_id!r}")
        return base / run_id
    return base / f"seed_{exp['seed']}_rep_{exp['repetition']:02d}"


def run_config_text(exp: dict[str, Any]) -> str:
    """Render compile-time settings shared by both firmware motes."""
    return (
        "#ifndef RUN_CONFIG_H_\n"
        "#define RUN_CONFIG_H_\n\n"
        f"/* {exp.get('campaign', '')} | {exp.get('forced_loss', 'none')} | "
        f"{exp.get('forced_loss_layer', 'none')} */\n"
        f"#define EXPERIMENT_VARIANT {int(exp['variant'])}\n"
        f"#define EXPERIMENT_KEM_LEVEL {int(exp['kem_level'])}\n"
        f"#define EXPERIMENT_SEED {int(exp['seed'])}UL\n"
        f"#define CSMA_CONF_MAX_FRAME_RETRIES {int(exp.get('mac_max_retries') or 0)}\n"
        f"#define FORCED_LOSS_MODE {int(exp.get('forced_loss_mode') or 0)}\n"
        "#define FORCED_LOSS_AFTER_RADIO_CHANNEL "
        f"{int(exp.get('forced_loss_after_radio_channel') or 0)}\n"
        f"#define FORCED_LOSS_SOURCE_ID {int(exp.get('forced_loss_source_id') or 2)}\n"
        f"#define FORCED_LOSS_DESTINATION_ID {int(exp.get('forced_loss_destination_id') or 1)}\n"
        f"#define FORCED_LOSS_TARGET_TAG {int(exp.get('forced_loss_target_tag') or 0)}\n"
        f"#define FORCED_LOSS_LAST_SEQUENCE {int(exp.get('forced_loss_last_sequence') or 0)}\n"
        f"#define RFRAG_FRAGMENT_SIZE {int(exp.get('rfrag_fragment_size') or 96)}\n"
        f"#define APP_CONF_MAX_RETRIES {int(exp.get('max_application_retries') or 0)}\n\n"
        "#endif /* RUN_CONFIG_H_ */\n"
    )


def parse_kv(text: str) -> dict[str, str]:
    values: dict[str, str] = {}
    for part in text.split(","):
        if "=" not in part:
            continue
        key, value = part.split("=", 1)
        key = key.strip()
        if re.fullmatch(r"[A-Za-z_][A-Za-z0-9_-]*", key):
            values[key] = value.strip()
    return values


def typed_value(value: str) -> Any:
    stripped = value.strip()
    if stripped == "":
        return None
    if re.fullmatch(r"[-+]?\d+", stripped):
        try:
            return int(stripped)
        except ValueError:
            return stripped
    if re.fullmatch(
        r"[-+]?(?:\d+\.\d*|\d*\.\d+)(?:[eE][-+]?\d+)?",
        stripped,
    ):
        try:
            return float(stripped)
        except ValueError:
            return stripped
    return stripped


def sha256_file(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as handle:
        for block in iter(lambda: handle.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()
