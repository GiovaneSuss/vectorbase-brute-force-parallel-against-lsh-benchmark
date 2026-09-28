CXX := g++
CXXFLAGS := -O2 -Wall -Wextra -std=c++17 -fopenmp
LDFLAGS := -lrt

# Numero de threads usado nas buscas (make brute-force THREADS=8)
THREADS ?= 4
# Dataset do banco: sift1m (padrao) ou siftsmall (10K vetores, para testes rapidos)
DATASET ?= sift1m

DATA_DIR := data
COMMON_DIR := common
DATABASE_DIR := database
SCRIPTS_DIR := scripts
RESULTS_DIR := results
BIN_DIR := bin

PCD_DB := $(BIN_DIR)/pcd_db
DB_SRCS := $(DATABASE_DIR)/pcd_db.cpp $(DATABASE_DIR)/fvecs_reader.cpp $(COMMON_DIR)/shm_db.cpp
DB_HDRS := $(wildcard $(DATABASE_DIR)/*.h $(COMMON_DIR)/*.h)

.PHONY: all database download construct-db up-db down-db status-db test-db brute-force lsh clean

all: database

# Compila a ferramenta do banco (parser .fvecs/.ivecs + construcao + memoria compartilhada)
database: $(PCD_DB)

$(PCD_DB): $(DB_SRCS) $(DB_HDRS)
	@mkdir -p $(BIN_DIR)
	$(CXX) $(CXXFLAGS) -I$(COMMON_DIR) -I$(DATABASE_DIR) -o $@ $(DB_SRCS) $(LDFLAGS)

# Baixa e extrai SIFT1M e SIFT10K (siftsmall) para data/ — pula o que ja existir
download:
	$(DATABASE_DIR)/download.sh all

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

# Compila e roda a busca brute-force (sequencial e paralela) com THREADS threads
brute-force:
	@echo "TODO: compilar e rodar $(SCRIPTS_DIR)/brute-force com OMP_NUM_THREADS=$(THREADS) sobre o banco $(DATASET)"

# Compila e roda a indexacao + busca LSH com THREADS threads
lsh:
	@echo "TODO: compilar e rodar $(SCRIPTS_DIR)/lsh com OMP_NUM_THREADS=$(THREADS) sobre o banco $(DATASET)"

clean:
	rm -rf $(BIN_DIR)
