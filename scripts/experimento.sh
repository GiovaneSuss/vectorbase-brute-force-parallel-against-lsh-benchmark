#!/usr/bin/env bash
# Protocolo experimental completo numa tacada so (chamado por `make experimento`). Grava tudo em
# results/<MAQUINA>/:
#
#   maquina.txt        CPU, RAM (e modulos de memoria, no WSL via Windows), SO, compilador, commit, energia
#   fases.csv          inicio/fim (relogio de calendario) de cada fase — para cruzar com o log do HWiNFO
#   relogio.csv        diferenca entre o relogio do WSL e o do Windows, no inicio e no fim
#   avisos.txt         o que foi pulado ou pode ter ficado invalido (ex.: pouca memoria para o indice do LSH)
#   membw/             banda de memoria por numero de threads
#   brute-force/       sequencial + cada numero de THREAD_LIST, para cada dataset
#   lsh/               LSH sequencial em cada configuracao de LSH_CONFIGS, para cada dataset
#   logs/              saida completa do experimento e do banco
#
# Variaveis (o Makefile repassa as dele): MAQUINA, DATASETS ("sift1m sift10m"), QUERIES, K, REPEATS,
# THREAD_LIST, THREAD_LIST_10M (padrao = THREAD_LIST), LSH_CONFIGS (L:K:w ...), LSH_CONFIGS_10M (padrao =
# LSH_CONFIGS), SEED, IDLE_S (janela ociosa,
# padrao 60 s), PULAR_LSH_10M=1, SEM_PAUSA=1 (nao espera Enter para o HWiNFO), CONTINUAR=1 (aceita pasta
# de resultados ja existente).
set -euo pipefail
cd "$(dirname "$0")/.."

MAQUINA="${MAQUINA:-$(hostname)}"
DATASETS="${DATASETS:-sift1m sift10m}"
QUERIES="${QUERIES:-1024}"
K="${K:-10}"
REPEATS="${REPEATS:-5}"
THREAD_LIST="${THREAD_LIST:-1 2 4 8 16 32}"
THREAD_LIST_10M="${THREAD_LIST_10M:-$THREAD_LIST}"
LSH_CONFIGS="${LSH_CONFIGS:-8:8:1000 32:10:800 32:12:1000 32:14:1200 40:14:1300}"
LSH_CONFIGS_10M="${LSH_CONFIGS_10M:-$LSH_CONFIGS}"
SEED="${SEED:-42}"
IDLE_S="${IDLE_S:-60}"

OUT="results/$MAQUINA"
if [[ -d "$OUT" && -n "$(ls -A "$OUT" 2>/dev/null)" && -z "${CONTINUAR:-}" ]]; then
    echo "erro: $OUT ja existe e nao esta vazia. Mova/apague a pasta (resultados de rodadas diferentes nao"
    echo "      devem se misturar), use outro MAQUINA=, ou CONTINUAR=1 para acrescentar nela."
    exit 1
fi
mkdir -p "$OUT/logs"
exec > >(tee -a "$OUT/logs/experimento.log") 2>&1

now() { date +%s.%3N; }
[[ -s "$OUT/fases.csv" ]] || echo "fase,dataset,detalhe,start_unix,end_unix" > "$OUT/fases.csv"
fase() { echo "$1,$2,$3,$4,$(now)" >> "$OUT/fases.csv"; } # fase <nome> <dataset> <detalhe> <inicio>
aviso() { echo "  AVISO: $*"; echo "$(date '+%F %T') $*" >> "$OUT/avisos.txt"; }

IS_WSL=0; grep -qi microsoft /proc/version && IS_WSL=1
PS=""; [[ $IS_WSL == 1 ]] && PS="$(command -v powershell.exe || true)"
ps_run() { timeout 60 "$PS" -NoProfile -NonInteractive -Command "$1" 2>/dev/null | tr -d '\r'; }

# Diferenca de relogio WSL x Windows (o log do HWiNFO usa o relogio do Windows, em hora local)
relogio() {
    [[ -n "$PS" ]] || return 0
    [[ -s "$OUT/relogio.csv" ]] || echo "momento,wsl_unix_ms,windows_unix_ms,windows_utc_offset_min" > "$OUT/relogio.csv"
    local a b w
    a=$(date +%s%3N)
    w=$(ps_run '[DateTimeOffset]::UtcNow.ToUnixTimeMilliseconds(); [TimeZoneInfo]::Local.GetUtcOffset([DateTime]::Now).TotalMinutes')
    b=$(date +%s%3N)
    echo "$1,$(( (a + b) / 2 )),$(echo "$w" | sed -n 1p),$(echo "$w" | sed -n 2p)" >> "$OUT/relogio.csv"
}

AMOSTRADOR=scripts/energia/amostrador.ps1
AMOSTRADOR_PID=""
RAPL="nao"
if [[ -r /sys/class/powercap/intel-rapl:0/energy_uj ]]; then RAPL="sim"
elif [[ -e /sys/class/powercap/intel-rapl:0/energy_uj ]]; then RAPL="sem permissao"; fi

# ------------------------------------------------------------------------------------------------
# Maquina
# ------------------------------------------------------------------------------------------------
{
    echo "== execucao"
    echo "maquina: $MAQUINA | host: $(hostname) | inicio: $(date '+%F %T %z')"
    echo "commit: $(git rev-parse --short HEAD 2>/dev/null || echo '?')$(git diff --quiet 2>/dev/null || echo ' (com alteracoes locais)')"
    echo "queries=$QUERIES k=$K repeats=$REPEATS seed=$SEED"
    echo "threads: $THREAD_LIST | threads 10M: $THREAD_LIST_10M"
    echo "lsh (L:K:w): $LSH_CONFIGS | lsh 10M: $LSH_CONFIGS_10M"
    echo "OMP_PROC_BIND=${OMP_PROC_BIND:-unset} OMP_PLACES=${OMP_PLACES:-unset}"
    echo
    echo "== cpu"
    lscpu | grep -E '^(CPU\(s\)|Model name|Thread\(s\) per core|Core\(s\) per socket|Socket\(s\)|L3 cache|CPU max MHz):' || true
    echo
    echo "== memoria"
    free -h
    df -h /dev/shm | tail -1 | awk '{print "/dev/shm: " $2 " total, " $4 " livres"}'
    if [[ -n "$PS" ]]; then
        echo "modulos de RAM (Windows; SMBIOSMemoryType 26=DDR4, 34=DDR5, 30=LPDDR4, 35=LPDDR5):"
        ps_run 'Get-CimInstance Win32_PhysicalMemory | Format-Table Manufacturer,PartNumber,Speed,ConfiguredClockSpeed,Capacity,SMBIOSMemoryType -AutoSize | Out-String -Width 200'
        echo "RAM total do Windows (MB): $(ps_run '[math]::Round((Get-CimInstance Win32_ComputerSystem).TotalPhysicalMemory/1MB)')"
    elif command -v dmidecode >/dev/null && [[ $EUID == 0 ]]; then
        dmidecode -t memory | grep -E '^\s+(Type|Speed|Configured Memory Speed|Size):' | sort | uniq -c
    else
        echo "modulos de RAM: desconhecido (Linux: rode como root para o dmidecode, ou anote tipo/frequencia a mao)"
    fi
    echo
    echo "== sistema"
    uname -r
    grep PRETTY_NAME /etc/os-release 2>/dev/null | cut -d= -f2 | tr -d '"'
    g++ --version | head -1
    echo "wsl: $([[ $IS_WSL == 1 ]] && echo sim || echo nao) | RAPL: $RAPL"
} > "$OUT/maquina.txt"
cat "$OUT/maquina.txt"
relogio inicio

# ------------------------------------------------------------------------------------------------
# Energia
# ------------------------------------------------------------------------------------------------
echo
if [[ $RAPL == "sim" ]]; then
    echo "== energia: RAPL disponivel — medida direto nas buscas (colunas energy_j)"
elif [[ $RAPL == "sem permissao" ]]; then
    aviso "RAPL existe mas nao e legivel: rode 'sudo chmod a+r /sys/class/powercap/intel-rapl:*/energy_uj' e recomece"
elif [[ -n "$PS" ]] && timeout 60 "$PS" -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$AMOSTRADOR")" \
        -Saida x -Parar x -Testar 2>/dev/null | tr -d '\r' | grep -qx ok; then
    # RAPL pelo Windows: o amostrador grava a energia acumulada do pacote da CPU a cada 200 ms, em segundo plano,
    # do inicio ao fim do experimento; a energia de cada repeticao sai no fim (scripts/energia/energia.py).
    mkdir -p "$OUT/energia"
    rm -f "$OUT/energia/.parar"
    "$PS" -NoProfile -ExecutionPolicy Bypass -File "$(wslpath -w "$AMOSTRADOR")" \
        -Saida "$(wslpath -w "$OUT/energia/windows_rapl.csv")" -Parar "$(wslpath -w "$OUT/energia/.parar")" \
        > "$OUT/logs/amostrador.log" 2>&1 &
    AMOSTRADOR_PID=$!
    for _ in $(seq 100); do [[ -s "$OUT/energia/windows_rapl.csv" ]] && break; sleep 0.2; done
    echo "== energia: RAPL pelo Windows (contador Energy Meter), amostrado a cada 200 ms em energia/windows_rapl.csv"
    echo "   (sem HWiNFO; a energia de cada repeticao e calculada no fim)"
    sed -i 's/| RAPL: .*/| RAPL: nao no Linux, sim pelo Windows (Energy Meter)/' "$OUT/maquina.txt"
elif [[ $IS_WSL == 1 ]]; then
    echo "== energia: WSL sem RAPL e sem o contador Energy Meter no Windows. Deixe o HWiNFO64 gravando o log"
    echo "   (Sensors > Logging Start) ANTES de continuar e pare so no fim do experimento."
    echo "   Depois: make energia MAQUINA=$MAQUINA HWINFO=<caminho do log>"
    if [[ -t 0 && -z "${SEM_PAUSA:-}" ]]; then read -rp "   Enter quando o log do HWiNFO estiver gravando... "; fi
else
    aviso "sem RAPL e fora do WSL (VM/Colab?): energia fica NA; use o tempo de CPU (cpu_s_mean) como proxy"
fi

# ------------------------------------------------------------------------------------------------
# Janela ociosa (potencia de base, descontada da energia das buscas) e banda de memoria
# ------------------------------------------------------------------------------------------------
echo
echo "== janela ociosa: ${IDLE_S}s sem carga (base da potencia) — nao mexa na maquina"
t=$(now); sleep "$IDLE_S"; fase ocioso - - "$t"

echo
echo "== banda de memoria"
for th in $THREAD_LIST; do
    t=$(now); bin/membw --threads "$th" --out "$OUT/membw"; fase membw - "t$th" "$t"
done

# ------------------------------------------------------------------------------------------------
# Datasets
# ------------------------------------------------------------------------------------------------
DB_PID=""
derruba_banco() {
    if [[ -n "$DB_PID" ]]; then
        kill -TERM "$DB_PID" 2>/dev/null || true
        wait "$DB_PID" 2>/dev/null || true
        DB_PID=""
    fi
}
para_amostrador() {
    if [[ -n "$AMOSTRADOR_PID" ]]; then
        touch "$OUT/energia/.parar"
        wait "$AMOSTRADOR_PID" 2>/dev/null || true
        rm -f "$OUT/energia/.parar"
        AMOSTRADOR_PID=""
    fi
}
trap 'derruba_banco; para_amostrador' EXIT

mem_avail_mb() { awk '/^MemAvailable:/ {print int($2 / 1024)}' /proc/meminfo; }
n_base() { case "$1" in sift1m) echo 1000000 ;; sift10m) echo 10000000 ;; siftsmall) echo 10000 ;; *) echo 0 ;; esac; }

for ds in $DATASETS; do
    echo
    echo "================================================================================"
    echo "== $ds"
    echo "================================================================================"
    if [[ ! -f "data/$ds.db" ]]; then
        aviso "$ds: data/$ds.db nao existe (make construct-db DATASET=$ds) — dataset pulado"
        continue
    fi

    if [[ -e "/dev/shm/pcd_$ds" ]]; then
        echo "  banco '$ds' ja esta no ar — usando o existente"
    else
        echo "  subindo o banco '$ds'..."
        bin/pcd_db up "$ds" > "$OUT/logs/up-$ds.log" 2>&1 &
        DB_PID=$!
        for _ in $(seq 600); do
            bin/pcd_db status "$ds" > /dev/null 2>&1 && break
            if ! kill -0 "$DB_PID" 2>/dev/null; then DB_PID=""; break; fi
            sleep 1
        done
        if [[ -z "$DB_PID" ]] || ! bin/pcd_db status "$ds" > /dev/null 2>&1; then
            aviso "$ds: o banco nao subiu (ver $OUT/logs/up-$ds.log) — dataset pulado"
            tail -3 "$OUT/logs/up-$ds.log" || true
            derruba_banco
            continue
        fi
        echo "  banco no ar"
    fi

    tl="$THREAD_LIST"; [[ $ds == sift10m ]] && tl="$THREAD_LIST_10M"
    echo
    echo "-- brute-force ($ds): sequencial + threads $tl"
    t=$(now)
    scripts/brute-force/bench.sh bin/brute_force "$tl" --dataset "$ds" --queries "$QUERIES" --k "$K" \
        --repeats "$REPEATS" --out "$OUT/brute-force"
    fase brute-force "$ds" - "$t"

    if [[ $ds == sift10m && -n "${PULAR_LSH_10M:-}" ]]; then
        aviso "sift10m: LSH pulado (PULAR_LSH_10M=1)"
    else
        cfgs="$LSH_CONFIGS"; [[ $ds == sift10m ]] && cfgs="$LSH_CONFIGS_10M"
        for cfg in $cfgs; do
            IFS=: read -r L HK W <<< "$cfg"
            # Pico de memoria da indexacao ~ indice (L*n ids) + codigos temporarios (L*n uint32) + folga
            need=$(( $(n_base "$ds") * L * 8 / 1048576 * 115 / 100 + 256 ))
            avail=$(mem_avail_mb)
            echo
            echo "-- lsh sequencial ($ds) L=$L K=$HK w=$W | precisa ~${need} MB, livres ${avail} MB"
            if (( need > avail )); then
                aviso "$ds L=$L K=$HK w=$W: pulado — precisa ~${need} MB e o Linux tem ${avail} MB livres"
                continue
            fi
            if [[ -n "$PS" ]]; then
                wfree=$(ps_run '[math]::Round((Get-CimInstance Win32_OperatingSystem).FreePhysicalMemory/1024)')
                if [[ "$wfree" =~ ^[0-9]+$ ]] && (( need > wfree )); then
                    aviso "$ds L=$L K=$HK w=$W: o Windows tem so ${wfree} MB livres (precisa ~${need}) — risco de" \
                          "paginacao, confira se o tempo ficou coerente antes de usar este resultado"
                fi
            fi
            t=$(now)
            bin/lsh --seq --dataset "$ds" --queries "$QUERIES" --k "$K" --repeats "$REPEATS" --tables "$L" \
                --hashes "$HK" --width "$W" --seed "$SEED" --out "$OUT/lsh"
            fase lsh "$ds" "$cfg" "$t"
        done
    fi
    derruba_banco
done

relogio fim
if [[ -n "$AMOSTRADOR_PID" ]]; then
    para_amostrador
    echo
    echo "== energia (RAPL pelo Windows)"
    python3 scripts/energia/energia.py --results "$OUT" || aviso "calculo da energia falhou — rode 'make energia MAQUINA=$MAQUINA'"
fi
echo
echo "== fim: $(date '+%F %T') — resultados em $OUT/"
[[ -s "$OUT/avisos.txt" ]] && { echo "   avisos:"; sed 's/^/     /' "$OUT/avisos.txt"; }
if [[ $IS_WSL == 1 && $RAPL != "sim" && ! -s "$OUT/energia/windows_rapl.csv" ]]; then
    echo "   energia: pare o log do HWiNFO e rode: make energia MAQUINA=$MAQUINA HWINFO=<caminho do log .CSV>"
fi
