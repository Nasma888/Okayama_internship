from __future__ import annotations

import os
import shutil
import subprocess
from pathlib import Path

from common import utc_now, write_json


def run(
    cooja: Path,
    contiki: Path,
    simulation: Path,
    run_dir: Path,
    exp: dict,
) -> dict:
    gradlew = cooja / "gradlew"
    if not gradlew.is_file():
        raise FileNotFoundError(f"gradlew Cooja absent: {gradlew}")
    run_dir.mkdir(parents=True, exist_ok=True)
    log_path = run_dir / "cooja.log"
    console_log_path = run_dir / "cooja-console.log"
    script_log_path = cooja / "COOJA.testlog"

    # ScriptRunner writes log.log(...) to COOJA.testlog in Cooja's working
    # directory.  Never let a previous simulation's test log be parsed as the
    # current run.
    try:
        script_log_path.unlink(missing_ok=True)
    except OSError as exc:
        raise OSError(
            f"Impossible de nettoyer l'ancien journal ScriptRunner "
            f"{script_log_path}: {exc}"
        ) from exc
    log_path.unlink(missing_ok=True)
    args_value = (
        f"--contiki={contiki.resolve()} --no-gui {simulation.resolve()}"
    )
    command = [str(gradlew), "run", f"--args={args_value}"]
    started = utc_now()
    exit_code = -1
    timed_out = False
    error = None
    try:
        with console_log_path.open("wb") as console_log:
            completed = subprocess.run(
                command,
                cwd=cooja,
                stdout=console_log,
                stderr=subprocess.STDOUT,
                timeout=exp["timeout_s"] + 60,
                env=os.environ.copy(),
                check=False,
            )
            exit_code = completed.returncode
    except subprocess.TimeoutExpired:
        timed_out = True
        exit_code = 124
        error = "Arrêt par timeout externe"
        with console_log_path.open("ab") as console_log:
            console_log.write(b"\nPIPELINE_EVENT,EXTERNAL_TIMEOUT=1\n")
    except OSError as exc:
        error = f"Impossible de lancer Cooja: {exc}"
        console_log_path.write_text(error + "\n", encoding="utf-8")

    log_source = None
    if script_log_path.is_file() and script_log_path.stat().st_size > 0:
        shutil.copy2(script_log_path, log_path)
        log_source = str(script_log_path)
    elif console_log_path.is_file() and console_log_path.stat().st_size > 0:
        # Some Cooja versions may mirror ScriptRunner lines to stdout.  Use
        # that output only when it demonstrably contains experiment events.
        console_text = console_log_path.read_text(
            encoding="utf-8", errors="replace"
        )
        if "EXPERIMENT_CONFIG," in console_text:
            shutil.copy2(console_log_path, log_path)
            log_source = str(console_log_path)

    if log_source is None:
        missing_log_error = (
            "COOJA.testlog absent ou vide: aucun journal expérimental "
            "ScriptRunner n'a été produit"
        )
        error = f"{error}; {missing_log_error}" if error else missing_log_error

    status = {
        "run_id": exp["run_id"],
        "started_at": started,
        "ended_at": utc_now(),
        "command": command,
        "exit_code": exit_code,
        "timed_out": timed_out,
        "log_found": log_path.is_file() and log_path.stat().st_size > 0,
        "log_source": log_source,
        "console_log": str(console_log_path),
        "script_runner_log": str(script_log_path),
        "parse_status": "not_parsed",
        "error": error,
    }
    write_json(run_dir / "status.json", status)
    return status
