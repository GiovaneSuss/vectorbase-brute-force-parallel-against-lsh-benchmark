# Busca vetorial paralela: Brute-force vs LSH

Estudo comparativo de busca por vizinhos mais próximos com OpenMP sobre o dataset SIFT1M.

## Requisitos

- Linux (ou WSL2) com `g++` (C++17), `make` e `curl`
- ~1,5 GB livres em disco e ~600 MB livres em `/dev/shm` (confira com `df -h /dev/shm`)

## Como rodar

**1. Baixar o dataset** (só na primeira vez; ~160 MB):

```bash
make download
```

**2. Construir o banco** (converte e valida os dados, gerando `data/sift1m.db`):

```bash
make construct-db
```

**3. Subir o banco para a memória** (deixe este terminal aberto; Ctrl+C derruba o banco):

```bash
make up-db
```

**4. Em outro terminal**, conferir se o banco está no ar e íntegro:

```bash
make status-db
make test-db     # opcional: confirma que o banco é somente leitura
```

**5. Rodar o brute-force** (no mesmo terminal do passo 4, com o banco no ar):

```bash
make brute-force-seq          # sequencial (baseline)
make brute-force THREADS=4    # paralelo com 4 threads
make bench-brute-force        # sequencial + 1, 2, 4 e 8 threads, com speedup e eficiência
```

Os resultados ficam em `results/brute-force/<data-hora>_<seq|tN>/` e cada execução acrescenta uma linha em
`results/brute-force/runs.csv`.

O LSH ainda não foi implementado.

## Opções

- `DATASET=siftsmall`: usa o SIFT10K (10 mil vetores) para testes rápidos. Vale para todos os comandos, por
  exemplo `make construct-db DATASET=siftsmall` e depois `make up-db DATASET=siftsmall`.
- `QUERIES=100`, `K=10`, `REPEATS=3`: quantas queries buscar, quantos vizinhos e quantas repetições por
  execução (esses são os padrões). `THREAD_LIST="1 2 4 8"` define as threads do `bench-brute-force`.
- `make down-db`: remove o banco da memória se o terminal do `up-db` foi fechado sem Ctrl+C.
- `make clean`: apaga os binários compilados.
