# Busca vetorial paralela: Brute-force vs LSH

Estudo comparativo de busca por vizinhos mais próximos com OpenMP sobre o SIFT1M (1 milhão de vetores) e o SIFT10M (10 milhões,
os primeiros 10M do SIFT1B/BIGANN).

## Requisitos

- Linux (ou WSL2) com `g++` (C++17), `make` e `curl`
- SIFT1M: ~1,5 GB livres em disco e ~600 MB livres em `/dev/shm` (confira com `df -h /dev/shm`)
- SIFT10M: ~8 GB livres em disco e ~5,2 GB livres em `/dev/shm` (o banco em float32 ocupa 5,1 GB na RAM)

## Como rodar

**1. Baixar o dataset** (só na primeira vez; ~160 MB):

```bash
make download       # SIFT1M + SIFT10K
make download-10m   # SIFT10M (~1,3 GB dos vetores, baixados em streaming, + ~512 MB do ground truth)
```

**2. Construir o banco** (converte e valida os dados, gerando `data/sift1m.db` / `data/sift10m.db`):

```bash
make construct-db-1m
make construct-db-10m
```

**3. Subir o banco para a memória** (deixe este terminal aberto; Ctrl+C derruba o banco):

```bash
make up-db-1m     # sobe o banco de 1M
make up-db-10m    # sobe o banco de 10M
```

**4. Em outro terminal**, conferir se o banco está no ar e íntegro:

```bash
make status-db-1m    # ou status-db-10m
make test-db-1m      # opcional: confirma que o banco é somente leitura (ou test-db-10m)
```

**5. Rodar o brute-force** (no mesmo terminal do passo 4, com o banco no ar). A busca usa automaticamente o
banco que estiver de pé (1M ou 10M); se houver mais de um no ar, escolha com `DATASET=`, ex.:
`make bench-brute-force DATASET=sift10m`:

```bash
make brute-force-seq          # sequencial (baseline)
make brute-force THREADS=4    # paralelo com 4 threads
make bench-brute-force        # sequencial + 1, 2, 4 e 8 threads, com speedup e eficiência
```

Os resultados ficam em `results/brute-force/<data-hora>_<seq|tN>/` e cada execução acrescenta uma linha em
`results/brute-force/runs.csv`.

**6. Rodar o LSH** (idem; cada execução indexa e depois busca — a indexação é medida à parte):

```bash
make lsh-seq                  # sequencial (baseline da indexação e da busca)
make lsh THREADS=4            # paralelo com 4 threads
make bench-lsh                # sequencial + 1, 2, 4 e 8 threads, com speedup/eficiência das duas fases
```

Resultados em `results/lsh/`, no mesmo formato. Parâmetros do LSH: `LSH_TABLES=32`, `LSH_HASHES=10`,
`LSH_WIDTH=800`, `SEED=42`, `BATCH=1` (padrões; ver `scripts/lsh/README.md` para o trade-off recall × tempo).

## Opções

- `DATASET=sift1m|sift10m|siftsmall` escolhe o banco em qualquer comando (padrão `sift1m`); os atalhos
  `-1m`, `-10m` e `-small` (`make up-db-small`, `make status-db-10m`...) são o mesmo que passar `DATASET=`.
  O `siftsmall` é o SIFT10K (10 mil vetores), para testes rápidos.
- Os bancos de 1M e 10M podem ficar no ar ao mesmo tempo (um terminal para cada `up-db`), se couberem em
  `/dev/shm`. Se o `/dev/shm` for pequeno demais, o `up-db` avisa; com RAM sobrando dá para aumentá-lo com
  `sudo mount -o remount,size=8G /dev/shm`.
- `QUERIES=100`, `K=10`, `REPEATS=3`: quantas queries buscar, quantos vizinhos e quantas repetições por
  execução (esses são os padrões). `THREAD_LIST="1 2 4 8"` define as threads do `bench-brute-force`.
- `make down-db-1m` / `make down-db-10m`: remove o banco da memória se o terminal do `up-db` foi fechado sem Ctrl+C.
- `make clean`: apaga os binários compilados.
