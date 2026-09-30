#!/usr/bin/env bash
# Varredura do protocolo para o LSH: roda o sequencial (baseline) e depois o paralelo para cada numero de
# threads, passando os tempos do sequencial — busca E indexacao, que escalam diferente e sao reportadas
# separadas — para que cada execucao calcule os dois speedups e eficiencias.
#
# Uso: bench.sh <binario> "<lista de threads>" [argumentos repassados ao binario...]
set -euo pipefail

BIN="$1"; THREAD_LIST="$2"; shift 2

# Descobre o diretorio de saida (--out) para ler o runs.csv; --batch so vale para o paralelo
OUT="results/lsh"
args=("$@")
seq_args=()
for ((i = 0; i < ${#args[@]}; i++)); do
    [[ "${args[i]}" == "--out" ]] && OUT="${args[i+1]}"
    if [[ "${args[i]}" == "--batch" ]]; then i=$((i + 1)); continue; fi
    seq_args+=("${args[i]}")
done

# Le a coluna $2 da ultima linha de $1 pelo nome do cabecalho
last_col() {
    awk -F, -v col="$2" 'NR == 1 { for (i = 1; i <= NF; i++) if ($i == col) c = i; next } { v = $c } END { print v }' "$1"
}

echo "=== baseline sequencial ==="
"$BIN" --seq "${seq_args[@]}"
BASELINE=$(last_col "$OUT/runs.csv" search_s_mean)
INDEX_BASELINE=$(last_col "$OUT/runs.csv" index_s_mean)

IDS=("$(last_col "$OUT/runs.csv" run_id)")
for t in $THREAD_LIST; do
    echo
    echo "=== paralelo, $t thread(s) ==="
    "$BIN" --threads "$t" --baseline "$BASELINE" --index-baseline "$INDEX_BASELINE" "$@"
    IDS+=("$(last_col "$OUT/runs.csv" run_id)")
done

echo
echo "=== resumo (baseline sequencial: indexacao ${INDEX_BASELINE}s, busca ${BASELINE}s) ==="
printf "%-24s %7s | %10s %8s %6s | %10s %9s %8s %6s | %7s\n" run_id threads indexacao speedup efic \
    busca ms/query speedup efic recall
for id in "${IDS[@]}"; do
    awk -F, -v id="$id" '
        NR == 1 { for (i = 1; i <= NF; i++) c[$i] = i; next }
        $c["run_id"] == id {
            printf "%-24s %7s | %10s %8s %6s | %10s %9s %8s %6s | %7s\n", $c["run_id"], $c["threads"],
                   $c["index_s_mean"], $c["index_speedup"], $c["index_efficiency"], $c["search_s_mean"],
                   $c["ms_per_query"], $c["speedup"], $c["efficiency"], $c["recall_at_k"]
        }' "$OUT/runs.csv"
done
