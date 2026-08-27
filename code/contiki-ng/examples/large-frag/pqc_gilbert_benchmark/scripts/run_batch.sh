#!/usr/bin/env bash
#
# Exécute tous les runs listés dans results/run_manifest.json en mode
# headless Cooja (-nogui), en parallélisant modérément pour ne pas saturer
# la machine (les simulations Gilbert sont indépendantes entre seeds).
#
# ADAPTER : COOJA_DIR vers ton install Contiki-NG/Cooja (celle où tu as
# déjà patché ExtensionManager.java et enregistré UDGMGilbertLoss).
#
# Usage:
#   ./run_batch.sh [--parallel N] [--suite A|B|A+B|all] [--dry-run]

set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
MANIFEST="${ROOT_DIR}/results/run_manifest.json"
COOJA_DIR="${COOJA_DIR:-/path/to/contiki-ng/tools/cooja}"   # <-- ADAPTER
PARALLEL=4
SUITE_FILTER="all"
DRY_RUN=false

while [[ $# -gt 0 ]]; do
  case "$1" in
    --parallel) PARALLEL="$2"; shift 2 ;;
    --suite) SUITE_FILTER="$2"; shift 2 ;;
    --dry-run) DRY_RUN=true; shift ;;
    *) echo "Argument inconnu: $1"; exit 1 ;;
  esac
done

if [[ ! -f "$MANIFEST" ]]; then
  echo "Manifest introuvable: $MANIFEST"
  echo "Lance d'abord: python3 scripts/render_csc.py"
  exit 1
fi

mkdir -p "${ROOT_DIR}/results/run_status"

run_one() {
  local csc_path="$1"
  local run_id="$2"
  local status_file="${ROOT_DIR}/results/run_status/${run_id}.status"

  if [[ -f "$status_file" ]] && grep -q "DONE" "$status_file" 2>/dev/null; then
    echo "[SKIP] ${run_id} (déjà terminé)"
    return 0
  fi

  echo "[RUN ] ${run_id}"
  if $DRY_RUN; then
    echo "DRY_RUN" > "$status_file"
    return 0
  fi

  local start_ts=$(date +%s)
  if ( cd "$COOJA_DIR" && ./gradlew run --args="-nogui=${csc_path} -contiki=." \
        > "${ROOT_DIR}/results/run_status/${run_id}.stdout" 2>&1 ); then
    local end_ts=$(date +%s)
    echo "DONE elapsed=$((end_ts-start_ts))s" > "$status_file"
    echo "[OK  ] ${run_id} ($((end_ts-start_ts))s)"
  else
    echo "FAILED" > "$status_file"
    echo "[FAIL] ${run_id} -- voir ${run_id}.stdout"
  fi
}
export -f run_one
export ROOT_DIR COOJA_DIR DRY_RUN

# Filtrage par suite + construction de la liste (csc_path, run_id)
python3 - "$MANIFEST" "$SUITE_FILTER" <<'PYEOF' > "${ROOT_DIR}/results/.batch_list.tsv"
import json, sys
manifest = json.load(open(sys.argv[1]))
suite_filter = sys.argv[2]
for r in manifest:
    if suite_filter == "all" or r["suite"] == suite_filter:
        print(f"{r['csc_path']}\t{r['run_id']}")
PYEOF

N_RUNS=$(wc -l < "${ROOT_DIR}/results/.batch_list.tsv")
echo "=== ${N_RUNS} runs à traiter (suite=${SUITE_FILTER}, parallélisme=${PARALLEL}) ==="

cat "${ROOT_DIR}/results/.batch_list.tsv" | \
  xargs -P "$PARALLEL" -I{} bash -c '
    IFS=$'"'"'\t'"'"' read -r csc_path run_id <<< "{}"
    run_one "$csc_path" "$run_id"
  '

echo "=== Batch terminé. Statuts dans results/run_status/ ==="
echo "Résumé:"
grep -l "DONE" "${ROOT_DIR}"/results/run_status/*.status 2>/dev/null | wc -l | xargs echo "  DONE  :"
grep -l "FAILED" "${ROOT_DIR}"/results/run_status/*.status 2>/dev/null | wc -l | xargs echo "  FAILED:"
