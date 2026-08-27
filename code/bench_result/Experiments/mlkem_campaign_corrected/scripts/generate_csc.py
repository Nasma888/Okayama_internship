from __future__ import annotations

import argparse
import re
import shutil
import xml.etree.ElementTree as ET
from pathlib import Path
from xml.sax.saxutils import escape

from common import normalize_experiment, run_config_text


# Stock Cooja fixed-PRR medium.
FIXED_CLASS = "org.contikios.cooja.radiomediums.UDGM"
GILBERT_CLASS = "org.contikios.cooja.radiomediums.UDGMGilbertLoss"
FIRMWARE_FILES = (
    "client.c",
    "server.c",
    "project-conf.h",
    "experiment-variants.h",
    "experiment-stats.h",
    "forced_loss_tests.h",
    "log_events.h",
    "Makefile",
)


def prepare_build(
    firmware: Path,
    build_dir: Path,
    exp: dict,
    contiki: Path,
) -> None:
    missing = [name for name in FIRMWARE_FILES if not (firmware / name).is_file()]
    if missing:
        raise FileNotFoundError(
            f"Fichiers firmware manquants dans {firmware}: {missing}"
        )
    if build_dir.exists():
        shutil.rmtree(build_dir)
    build_dir.mkdir(parents=True)
    for name in FIRMWARE_FILES:
        target = build_dir / name
        text = (firmware / name).read_text(encoding="utf-8")
        if name == "Makefile":
            text = text.replace("[CONTIKI_DIR]", str(contiki.resolve()))
        target.write_text(text, encoding="utf-8")

    # All per-run protocol/forced-loss settings are generated in one place.
    (build_dir / "run-config.h").write_text(
        run_config_text(exp), encoding="utf-8"
    )


def generate(
    template: Path,
    motes: Path,
    firmware: Path,
    output: Path,
    build_dir: Path,
    exp: dict,
    contiki: Path,
) -> None:
    prepare_build(firmware, build_dir, exp, contiki)
    text = template.read_text(encoding="utf-8")
    motes_xml = motes.read_text(encoding="utf-8")
    if motes_xml.count("<motetype>") != 2 or motes_xml.count("<mote>") != 2:
        raise ValueError(f"{motes}: deux <motetype> et deux <mote> sont requis")
    motes_xml = motes_xml.replace(
        "[BUILD_DIR]", escape(str(build_dir.resolve()))
    )

    if exp["model"] == "fixed":
        radio_class = FIXED_CLASS
        radio_parameters = (
            "<success_ratio_tx>1.0</success_ratio_tx>\n"
            f"<success_ratio_rx>{exp['target_prr']:.8f}</success_ratio_rx>"
        )
    else:
        radio_class = GILBERT_CLASS
        radio_parameters = (
            "<success_ratio_tx>1.0</success_ratio_tx>\n"
            "<success_ratio_rx>1.0</success_ratio_rx>\n"
            f"<gilbert_p_gb>{exp['p_gb']:.10f}</gilbert_p_gb>\n"
            f"<gilbert_p_bg>{exp['p_bg']:.10f}</gilbert_p_bg>\n"
            "<gilbert_log_bursts>true</gilbert_log_bursts>"
        )

    replacements = {
        "[CONTIKI_DIR]": escape(str(contiki.resolve())),
        "[RUN_ID]": escape(exp["run_id"]),
        "[SEED]": str(exp["seed"]),
        "[RADIO_MEDIUM_CLASS]": radio_class,
        "[RADIO_MODEL_PARAMETERS]": radio_parameters,
        "[TIMEOUT_MS]": str(exp["timeout_s"] * 1000),
        "[MOTES_XML]": motes_xml,
    }
    for token, value in replacements.items():
        text = text.replace(token, value)
    unresolved = sorted(set(re.findall(r"\[[A-Z][A-Z0-9_]*\]", text)))
    if unresolved:
        raise ValueError(f"Jetons non remplacés: {unresolved}")
    try:
        root = ET.fromstring(text)
    except ET.ParseError as exc:
        raise ValueError(f"CSC XML invalide: {exc}") from exc
    if len(root.findall("./simulation/motetype")) != 2:
        raise ValueError("Le CSC généré ne contient pas deux motetypes")
    if len(root.findall("./simulation/mote")) != 2:
        raise ValueError("Le CSC généré ne contient pas deux motes")
    output.parent.mkdir(parents=True, exist_ok=True)
    output.write_text(text, encoding="utf-8")


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--motes", type=Path, required=True)
    parser.add_argument("--firmware", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--build-dir", type=Path, required=True)
    parser.add_argument("--contiki", type=Path, required=True)
    parser.add_argument("--run-id", required=True)
    parser.add_argument("--model", choices=["fixed", "gilbert"], required=True)
    parser.add_argument("--variant", type=int, choices=[1, 2, 3, 4], required=True)
    parser.add_argument("--kem-level", type=int, choices=[512, 768, 1024], required=True)
    parser.add_argument("--seed", type=int, required=True)
    parser.add_argument("--prr", type=float, required=True)
    parser.add_argument("--p-gb", type=float)
    parser.add_argument("--p-bg", type=float)
    parser.add_argument("--timeout-s", type=int, default=240)
    parser.add_argument("--mac-max-retries", type=int, default=0)
    parser.add_argument(
        "--forced-loss",
        choices=["none", "first_fragment", "last_fragment", "first_and_last_fragment"],
        default="none",
    )
    parser.add_argument("--forced-loss-mode", type=int, choices=[0, 1, 2, 3])
    parser.add_argument("--forced-loss-after-radio-channel", type=int, choices=[0, 1])
    parser.add_argument("--max-application-retries", type=int)
    parser.add_argument("--campaign", default="")
    args = parser.parse_args()

    exp = normalize_experiment({
        "run_id": args.run_id,
        "model": args.model,
        "variant": args.variant,
        "kem_level": args.kem_level,
        "seed": args.seed,
        "target_prr": args.prr,
        "p_gb": args.p_gb,
        "p_bg": args.p_bg,
        "timeout_s": args.timeout_s,
        "mac_max_retries": args.mac_max_retries,
        "forced_loss": args.forced_loss,
        "forced_loss_mode": args.forced_loss_mode,
        "forced_loss_after_radio_channel": args.forced_loss_after_radio_channel,
        "max_application_retries": args.max_application_retries,
        "campaign": args.campaign,
    })
    generate(
        args.template,
        args.motes,
        args.firmware,
        args.output,
        args.build_dir,
        exp,
        args.contiki,
    )
