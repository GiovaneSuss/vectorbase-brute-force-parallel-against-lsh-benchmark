#!/usr/bin/env bash
# Baixa e extrai os datasets do corpus TEXMEX para data/:
#   - siftsmall: SIFT10K  (data/siftsmall/)
#   - sift:      SIFT1M   (data/sift/)
#   - sift10m:   SIFT10M = os 10 primeiros milhoes de vetores do SIFT1B/BIGANN (data/bigann/)
# Idempotente: pula o dataset se os arquivos ja estiverem extraidos.
#
# Uso: database/download.sh [sift|siftsmall|sift10m|all]   (padrao: all = siftsmall + sift)
set -euo pipefail

DATA_DIR="$(cd "$(dirname "$0")/.." && pwd)/data"
BASE_URL="ftp://ftp.irisa.fr/local/texmex/corpus"

fetch() {
    local name="$1"
    local dir="$DATA_DIR/$name"
    if [[ -f "$dir/${name}_base.fvecs" && -f "$dir/${name}_query.fvecs" && -f "$dir/${name}_groundtruth.ivecs" ]]; then
        echo "[download] $name ja existe em $dir — pulando"
        return
    fi
    local tarball="$DATA_DIR/$name.tar.gz"
    echo "[download] baixando $BASE_URL/$name.tar.gz"
    curl -fL --retry 3 --progress-bar -o "$tarball" "$BASE_URL/$name.tar.gz"
    echo "[download] extraindo $tarball"
    tar -xzf "$tarball" -C "$DATA_DIR"
    rm -f "$tarball"
    ls -l "$dir"
}

# SIFT10M a partir do BIGANN (SIFT1B). O bigann_base.bvecs.gz tem ~98 GB, mas como o gzip e sequencial basta
# descompactar em streaming e cortar nos primeiros 10M registros (~1,3 GB descompactados): o curl e encerrado
# assim que o `head` fecha o pipe, entao so o comeco do arquivo e baixado. Formato .bvecs: [int32 d][d x uint8].
# O ground truth dos 10M primeiros vem pronto no bigann_gnd.tar.gz (gnd/idx_10M.ivecs, 1000 vizinhos/query).
fetch_sift10m() {
    local dir="$DATA_DIR/bigann"
    local n=10000000 d=128 nq=10000 gt_k=1000
    local base="$dir/bigann_base_10M.bvecs" query="$dir/bigann_query.bvecs" gt="$dir/idx_10M.ivecs"
    local base_size=$((n * (4 + d))) query_size=$((nq * (4 + d))) gt_size=$((nq * (4 + 4 * gt_k)))
    mkdir -p "$dir"

    # Tamanho exato de cada arquivo = prova de que a copia esta completa (o pipe cortado faz o curl
    # sair com erro de proposito, por isso o `|| true` e a conferencia depois).
    check_size() { [[ -f "$1" && "$(stat -c %s "$1")" -eq "$2" ]]; }

    if check_size "$base" "$base_size"; then
        echo "[download] $base ja existe — pulando"
    else
        echo "[download] baixando os primeiros 10M vetores de $BASE_URL/bigann_base.bvecs.gz (~1,3 GB descompactados)"
        (curl -fsSL --retry 3 "$BASE_URL/bigann_base.bvecs.gz" | gunzip -c | head -c "$base_size" > "$base.part") || true
        check_size "$base.part" "$base_size" || { echo "[download] base incompleta ($base.part)" >&2; exit 1; }
        mv "$base.part" "$base"
    fi

    if check_size "$query" "$query_size"; then
        echo "[download] $query ja existe — pulando"
    else
        echo "[download] baixando $BASE_URL/bigann_query.bvecs.gz"
        curl -fL --retry 3 --progress-bar "$BASE_URL/bigann_query.bvecs.gz" | gunzip -c > "$query.part"
        check_size "$query.part" "$query_size" || { echo "[download] queries incompletas ($query.part)" >&2; exit 1; }
        mv "$query.part" "$query"
    fi

    if check_size "$gt" "$gt_size"; then
        echo "[download] $gt ja existe — pulando"
    else
        echo "[download] extraindo gnd/idx_10M.ivecs de $BASE_URL/bigann_gnd.tar.gz (~512 MB)"
        local tmp="$dir/gnd.tmp"
        rm -rf "$tmp" && mkdir -p "$tmp"
        (curl -fL --retry 3 --progress-bar "$BASE_URL/bigann_gnd.tar.gz" |
            tar -xzf - -C "$tmp" --wildcards --occurrence=1 '*idx_10M.ivecs') || true
        local found
        found="$(find "$tmp" -name idx_10M.ivecs | head -n1)"
        [[ -n "$found" ]] && check_size "$found" "$gt_size" || { echo "[download] ground truth incompleto" >&2; exit 1; }
        mv "$found" "$gt"
        rm -rf "$tmp"
    fi
    ls -l "$dir"
}

mkdir -p "$DATA_DIR"
case "${1:-all}" in
    sift)      fetch sift ;;
    siftsmall) fetch siftsmall ;;
    sift10m)   fetch_sift10m ;;
    all)       fetch siftsmall; fetch sift ;;
    *) echo "uso: $0 [sift|siftsmall|sift10m|all]" >&2; exit 1 ;;
esac
