CXX := g++
CXXFLAGS := -O2 -Wall -Wextra -std=c++17 -fopenmp
LDFLAGS := -lrt

# Numero de threads usado nas buscas (make brute-force THREADS=8)
THREADS ?= 4
# Dataset do banco: sift1m (padrao), sift10m (10M primeiros do SIFT1B) ou siftsmall (10K, testes rapidos).
# Atalhos sem precisar de DATASET=: make up-db-1m, make up-db-10m, make up-db-small (idem construct/down/status/test)
DATASET ?= sift1m
# Parametros dos experimentos de busca (protocolo: 1024 queries, 5 repeticoes, 1 a 32 threads —
# 32 = CPUs logicas da maior maquina testada, o limite fisico que importa)
QUERIES ?= 1024
K ?= 10
REPEATS ?= 5
THREAD_LIST ?= 1 2 4 8 16 32
# Parametros do LSH (fixos entre configuracoes: mesma semente = mesmo indice). LSH_BUCKETS=0 = automatico (~n/16)
LSH_TABLES ?= 32
LSH_HASHES ?= 10
LSH_WIDTH ?= 800
LSH_BUCKETS ?= 0
SEED ?= 42
# Queries por lote no schedule(dynamic) da busca LSH paralela
BATCH ?= 1
# Configuracoes do LSH sequencial rodadas no `make experimento` (curva recall x velocidade), como L:K:w.
# Recall@10 medido no SIFT1M (1024 queries): ~0.87, ~0.915 (padrao), ~0.945, ~0.97, ~0.99
LSH_CONFIGS ?= 8:8:1000 32:10:800 32:12:1000 32:14:1200 40:14:1300
LSH_CONFIGS_10M ?= $(LSH_CONFIGS)
# Threads do brute-force no SIFT10M (padrao = THREAD_LIST; o 10M e o que mais demora)
THREAD_LIST_10M ?= $(THREAD_LIST)
# Pasta dos resultados do `make experimento` (results/<MAQUINA>/); padrao = nome da maquina
MAQUINA ?= $(shell hostname)
# Rodadas completas do experimento (RODADAS=5 -> results/<MAQUINA>/rodada-1 ... rodada-5)
RODADAS ?= 1

# Fixa cada thread OpenMP num nucleo (threads vizinhas em nucleos vizinhos): sem isso o SO migra as
# threads entre nucleos no meio da medicao e o tempo fica ruidoso.
export OMP_PROC_BIND ?= close
export OMP_PLACES ?= cores

DATA_DIR := data
COMMON_DIR := common
DATABASE_DIR := database
SCRIPTS_DIR := scripts
RESULTS_DIR := results
BIN_DIR := bin

PCD_DB := $(BIN_DIR)/pcd_db
DB_SRCS := $(DATABASE_DIR)/pcd_db.cpp $(DATABASE_DIR)/fvecs_reader.cpp $(COMMON_DIR)/shm_db.cpp
DB_HDRS := $(wildcard $(DATABASE_DIR)/*.h $(COMMON_DIR)/*.h)

BF_BIN := $(BIN_DIR)/brute_force
BF_DIR := $(SCRIPTS_DIR)/brute-force
BF_SRCS := $(BF_DIR)/main.cpp $(BF_DIR)/brute_force.cpp $(COMMON_DIR)/shm_db.cpp $(COMMON_DIR)/metrics.cpp
BF_HDRS := $(wildcard $(BF_DIR)/*.h $(COMMON_DIR)/*.h)
# Nas buscas, sem DATASET= explicito o programa usa o banco que estiver no ar (auto); com DATASET= usa esse.
SEARCH_DATASET := $(if $(filter command line environment,$(origin DATASET)),$(DATASET),auto)
BF_ARGS := --dataset $(SEARCH_DATASET) --queries $(QUERIES) --k $(K) --repeats $(REPEATS) --out $(RESULTS_DIR)/brute-force

MEMBW_BIN := $(BIN_DIR)/membw
MEMBW_DIR := $(SCRIPTS_DIR)/membw
MEMBW_SRCS := $(MEMBW_DIR)/membw.cpp $(COMMON_DIR)/metrics.cpp

LSH_BIN := $(BIN_DIR)/lsh
LSH_DIR := $(SCRIPTS_DIR)/lsh
LSH_SRCS := $(LSH_DIR)/main.cpp $(LSH_DIR)/lsh.cpp $(COMMON_DIR)/shm_db.cpp $(COMMON_DIR)/metrics.cpp
LSH_HDRS := $(wildcard $(LSH_DIR)/*.h $(COMMON_DIR)/*.h)
LSH_ARGS := --dataset $(SEARCH_DATASET) --queries $(QUERIES) --k $(K) --repeats $(REPEATS) \
            --tables $(LSH_TABLES) --hashes $(LSH_HASHES) --width $(LSH_WIDTH) --buckets $(LSH_BUCKETS) \
            --seed $(SEED) --out $(RESULTS_DIR)/lsh

.PHONY: all database download download-10m construct-db up-db down-db status-db test-db brute-force \
        brute-force-seq bench-brute-force lsh lsh-seq bench-lsh membw experimento energia clean

all: database $(BF_BIN) $(LSH_BIN) $(MEMBW_BIN)

# Compila a ferramenta do banco (parser .fvecs/.ivecs + construcao + memoria compartilhada)
database: $(PCD_DB)

$(PCD_DB): $(DB_SRCS) $(DB_HDRS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(COMMON_DIR) -I$(DATABASE_DIR) -o $@ $(DB_SRCS) $(LDFLAGS)

# Baixa e extrai SIFT1M e SIFT10K (siftsmall) para data/ — pula o que ja existir
download:
	$(DATABASE_DIR)/download.sh all

# Baixa o SIFT10M: os primeiros 10M vetores do SIFT1B/BIGANN (~1,3 GB, em streaming) + queries + ground truth
# (~512 MB) para data/bigann/ — pula o que ja existir
download-10m:
	$(DATABASE_DIR)/download.sh sift10m

# Converte os .fvecs/.ivecs em data/$(DATASET).db (validando N, D e valores)
construct-db: database
	$(PCD_DB) construct $(DATASET)

# Sobe o banco para /dev/shm/pcd_$(DATASET) (somente leitura) e fica em primeiro plano; Ctrl+C derruba
up-db: database
	@$(PCD_DB) up $(DATASET)

# Remove o segmento se o up-db morreu sem limpar (ex.: kill -9)
down-db: database
	@$(PCD_DB) down $(DATASET)

# Conecta no banco no ar, confere checksum e mostra amostras
status-db: database
	@$(PCD_DB) status $(DATASET)

# Prova que o banco no ar nao aceita escrita
test-db: database
	@$(PCD_DB) test-readonly $(DATASET)

# Atalhos por tamanho: <comando>-db-1m / -10m / -small = <comando>-db DATASET=sift1m / sift10m / siftsmall.
# Ex.: `make construct-db-10m` e depois `make up-db-10m`. Dois bancos diferentes podem ficar no ar ao mesmo
# tempo (segmentos /dev/shm/pcd_sift1m e /dev/shm/pcd_sift10m), se couberem na memoria.
DB_SIZES := 1m=sift1m 10m=sift10m small=siftsmall
define DB_ALIAS
construct-db-$(1) up-db-$(1) down-db-$(1) status-db-$(1) test-db-$(1): DATASET = $(2)
construct-db-$(1): construct-db
up-db-$(1): up-db
down-db-$(1): down-db
status-db-$(1): status-db
test-db-$(1): test-db
.PHONY: construct-db-$(1) up-db-$(1) down-db-$(1) status-db-$(1) test-db-$(1)
endef
$(foreach s,$(DB_SIZES),$(eval $(call DB_ALIAS,$(word 1,$(subst =, ,$(s))),$(word 2,$(subst =, ,$(s))))))

$(BF_BIN): $(BF_SRCS) $(BF_HDRS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(COMMON_DIR) -I$(BF_DIR) -o $@ $(BF_SRCS) $(LDFLAGS)

# Busca brute-force paralela com THREADS threads (precisa do banco no ar: make up-db)
brute-force: $(BF_BIN)
	@$(BF_BIN) --threads $(THREADS) $(BF_ARGS)

# Busca brute-force sequencial (sem OpenMP) — baseline do speedup
brute-force-seq: $(BF_BIN)
	@$(BF_BIN) --seq $(BF_ARGS)

# Roda o sequencial e depois o paralelo para cada valor de THREAD_LIST, com speedup e eficiencia
bench-brute-force: $(BF_BIN)
	@$(BF_DIR)/bench.sh "$(BF_BIN)" "$(THREAD_LIST)" $(BF_ARGS)

$(LSH_BIN): $(LSH_SRCS) $(LSH_HDRS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(COMMON_DIR) -I$(LSH_DIR) -o $@ $(LSH_SRCS) $(LDFLAGS)

# LSH paralelo com THREADS threads: indexacao (medida a parte) + busca por lotes de queries (banco no ar)
lsh: $(LSH_BIN)
	@$(LSH_BIN) --threads $(THREADS) --batch $(BATCH) $(LSH_ARGS)

# LSH sequencial (sem OpenMP) — baseline do speedup da indexacao e da busca
lsh-seq: $(LSH_BIN)
	@$(LSH_BIN) --seq $(LSH_ARGS)

# Roda o sequencial e depois o paralelo para cada valor de THREAD_LIST, com speedup e eficiencia
bench-lsh: $(LSH_BIN)
	@$(LSH_DIR)/bench.sh "$(LSH_BIN)" "$(THREAD_LIST)" --batch $(BATCH) $(LSH_ARGS)

$(MEMBW_BIN): $(MEMBW_SRCS) $(COMMON_DIR)/metrics.h
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(COMMON_DIR) -o $@ $(MEMBW_SRCS) $(LDFLAGS)

# Banda de memoria (leitura sequencial e triad do STREAM) para cada valor de THREAD_LIST — nao precisa do banco
membw: $(MEMBW_BIN)
	@for t in $(THREAD_LIST); do $(MEMBW_BIN) --threads $$t --out $(RESULTS_DIR)/membw; done

# Protocolo completo numa tacada so, em results/$(MAQUINA)/: informacoes da maquina, janela ociosa (base da
# energia), banda de memoria, e para cada dataset sobe o banco sozinho, roda o brute-force (sequencial +
# THREAD_LIST) e o LSH sequencial em cada LSH_CONFIGS, e derruba o banco. Ver scripts/experimento.sh.
experimento: all
	@for r in $$(seq $(RODADAS)); do \
	  m="$(MAQUINA)"; if [ "$(RODADAS)" -gt 1 ]; then m="$(MAQUINA)/rodada-$$r"; fi; \
	  echo "### rodada $$r de $(RODADAS) -> results/$$m"; \
	 MAQUINA="$$m" QUERIES="$(QUERIES)" K="$(K)" REPEATS="$(REPEATS)" THREAD_LIST="$(THREAD_LIST)" \
	 THREAD_LIST_10M="$(THREAD_LIST_10M)" LSH_CONFIGS="$(LSH_CONFIGS)" LSH_CONFIGS_10M="$(LSH_CONFIGS_10M)" \
	 SEED="$(SEED)" $(SCRIPTS_DIR)/experimento.sh || exit 1; \
	done

# Energia no WSL (sem RAPL no Linux): refaz o calculo a partir do RAPL do Windows gravado pelo experimento
# (energia/windows_rapl.csv — o `make experimento` ja roda isto no fim) ou de um log do HWiNFO:
# make energia MAQUINA=suss [HWINFO=/mnt/c/.../log.csv]
energia:
	@python3 $(SCRIPTS_DIR)/energia/energia.py --results $(RESULTS_DIR)/$(MAQUINA) $(if $(HWINFO),--hwinfo "$(HWINFO)")

clean:
	rm -rf $(BIN_DIR)
