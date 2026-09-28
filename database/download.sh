#!/usr/bin/env bash
# Baixa e extrai o SIFT1M (sift) e o SIFT10K (siftsmall) do corpus TEXMEX para data/.
# Idempotente: pula o dataset se os arquivos ja estiverem extraidos.
#
# Uso: database/download.sh [sift|siftsmall|all]   (padrao: all)
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

mkdir -p "$DATA_DIR"
case "${1:-all}" in
    sift)      fetch sift ;;
    siftsmall) fetch siftsmall ;;
    all)       fetch siftsmall; fetch sift ;;
    *) echo "uso: $0 [sift|siftsmall|all]" >&2; exit 1 ;;
esac
