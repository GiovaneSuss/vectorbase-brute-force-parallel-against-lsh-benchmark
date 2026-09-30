CXX := g++
CXXFLAGS := -O2 -Wall -Wextra -std=c++17 -fopenmp
LDFLAGS := -lrt

# Numero de threads usado nas buscas (make brute-force THREADS=8)
THREADS ?= 4
# Dataset do banco: sift1m (padrao), sift10m (10M primeiros do SIFT1B) ou siftsmall (10K, testes rapidos).
# Atalhos sem precisar de DATASET=: make up-db-1m, make up-db-10m, make up-db-small (idem construct/down/status/test)
DATASET ?= sift1m
# Parametros dos experimentos de busca
QUERIES ?= 100
K ?= 10
REPEATS ?= 3
THREAD_LIST ?= 1 2 4 8
# Parametros do LSH (fixos entre configuracoes: mesma semente = mesmo indice). LSH_BUCKETS=0 = automatico (~n/16)
LSH_TABLES ?= 32
LSH_HASHES ?= 10
LSH_WIDTH ?= 800
LSH_BUCKETS ?= 0
SEED ?= 42
# Queries por lote no schedule(dynamic) da busca LSH paralela
BATCH ?= 1

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

LSH_BIN := $(BIN_DIR)/lsh
LSH_DIR := $(SCRIPTS_DIR)/lsh
LSH_SRCS := $(LSH_DIR)/main.cpp $(LSH_DIR)/lsh.cpp $(COMMON_DIR)/shm_db.cpp $(COMMON_DIR)/metrics.cpp
LSH_HDRS := $(wildcard $(LSH_DIR)/*.h $(COMMON_DIR)/*.h)
LSH_ARGS := --dataset $(SEARCH_DATASET) --queries $(QUERIES) --k $(K) --repeats $(REPEATS) \
            --tables $(LSH_TABLES) --hashes $(LSH_HASHES) --width $(LSH_WIDTH) --buckets $(LSH_BUCKETS) \
            --seed $(SEED) --out $(RESULTS_DIR)/lsh

.PHONY: all database download download-10m construct-db up-db down-db status-db test-db brute-force \
        brute-force-seq bench-brute-force lsh lsh-seq bench-lsh clean

all: database $(BF_BIN) $(LSH_BIN)

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

clean:
	rm -rf $(BIN_DIR)
