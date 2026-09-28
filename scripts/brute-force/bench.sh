#!/usr/bin/env bash
# Varredura do protocolo: roda o brute-force sequencial (baseline) e depois o paralelo para cada numero de
# threads, passando o tempo do sequencial para que cada execucao calcule speedup e eficiencia.
#
# Uso: bench.sh <binario> "<lista de threads>" [argumentos repassados ao binario...]
set -euo pipefail

BIN="$1"; THREAD_LIST="$2"; shift 2

# Descobre o diretorio de saida (--out) para ler o runs.csv
OUT="results/brute-force"
args=("$@")
for ((i = 0; i < ${#args[@]}; i++)); do
    [[ "${args[i]}" == "--out" ]] && OUT="${args[i+1]}"
done

# Le a coluna $2 da ultima linha de $1 pelo nome do cabecalho
last_col() {
    awk -F, -v col="$2" 'NR == 1 { for (i = 1; i <= NF; i++) if ($i == col) c = i; next } { v = $c } END { print v }' "$1"
}

echo "=== baseline sequencial ==="
"$BIN" --seq "$@"
BASELINE=$(last_col "$OUT/runs.csv" search_s_mean)

IDS=("$(last_col "$OUT/runs.csv" run_id)")
for t in $THREAD_LIST; do
    echo
    echo "=== paralelo, $t thread(s) ==="
    "$BIN" --threads "$t" --baseline "$BASELINE" "$@"
    IDS+=("$(last_col "$OUT/runs.csv" run_id)")
done

echo
echo "=== resumo (baseline sequencial: ${BASELINE}s) ==="
printf "%-24s %8s %12s %10s %9s %11s %10s\n" run_id threads search_s ms/query speedup eficiencia energia_J
for id in "${IDS[@]}"; do
    awk -F, -v id="$id" '
        NR == 1 { for (i = 1; i <= NF; i++) c[$i] = i; next }
        $c["run_id"] == id {
            printf "%-24s %8s %12s %10s %9s %11s %10s\n", $c["run_id"], $c["threads"], $c["search_s_mean"],
                   $c["ms_per_query"], $c["speedup"], $c["efficiency"], $c["energy_j_mean"]
        }' "$OUT/runs.csv"
done
