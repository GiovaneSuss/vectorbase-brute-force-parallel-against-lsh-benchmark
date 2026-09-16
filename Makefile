CXX := g++
CXXFLAGS := -O2 -Wall -std=c++17 -fopenmp

# Numero de threads usado nas buscas (make brute-force THREADS=8)
THREADS ?= 4

DATA_DIR := data
COMMON_DIR := common
DATABASE_DIR := database
SCRIPTS_DIR := scripts
RESULTS_DIR := results

.PHONY: all database populate brute-force lsh clean

all: database brute-force lsh

# Compila o parser .fvecs/.ivecs e o loader do VectorDataset (database/, common/)
database:
	@echo "TODO: compilar $(DATABASE_DIR)/ e $(COMMON_DIR)/ (parser + VectorDataset)"

# Le os arquivos brutos em data/ e prepara o dataset (VectorDataset) para as buscas
populate: database
	@echo "TODO: rodar script de populacao sobre $(DATA_DIR)/ (validar N, D e amostras)"

# Compila e roda a busca brute-force (sequencial e paralela) com THREADS threads
brute-force: populate
	@echo "TODO: compilar e rodar $(SCRIPTS_DIR)/brute-force com OMP_NUM_THREADS=$(THREADS)"

# Compila e roda a indexacao + busca LSH com THREADS threads
lsh: populate
	@echo "TODO: compilar e rodar $(SCRIPTS_DIR)/lsh com OMP_NUM_THREADS=$(THREADS)"

clean:
	@echo "TODO: remover binarios e artefatos gerados em bin/ e build/"
