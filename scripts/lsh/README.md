# scripts/lsh/

Busca aproximada via Locality-Sensitive Hashing (LSH), Etapas 4 e 5 da especificação técnica. Lê a base direto
do banco em memória compartilhada (`attach_db`, ponteiros `const` — sem cópia), como o brute-force.

- `lsh.{h,cpp}` — estruturas (`Bucket`, `LSHIndex` da especificação), família de hash, indexação e busca.
- `main.cpp` — o experimento (`bin/lsh`): R repetições de indexação + busca sobre as Q primeiras queries,
  medições e CSVs.
- `bench.sh` — varredura: sequencial + paralelo para cada número de threads, com speedup e eficiência da
  indexação e da busca.

## Algoritmo

**Família de hash** — projeções aleatórias p-estáveis (E2LSH, Datar et al. 2004), a família LSH para distância
euclidiana: `h(v) = floor((a·v + b) / w)`, com `a ~ N(0, I)` e `b ~ U[0, w)`. Cada tabela concatena **K** funções
(só colidem vetores que colidem nas K) e o índice tem **L** tabelas independentes (um vizinho só precisa colidir em
uma). A tupla `(h_1..h_K)` vira um índice de bucket por hash. Um `LSHIndex` é uma tabela; o índice é um
`std::vector<LSHIndex>` com L delas. Projeções sorteadas com semente fixa (`--seed`, padrão 42): mesma semente,
mesmo índice em todas as configurações.

**Busca** — os ids dos L buckets da query viram a lista de candidatos (repetidos descartados por um bitmap),
e só para eles se calcula a distância exata; o top-K sai desses candidatos. Recall < 1.0: um vizinho verdadeiro
que não colidiu com a query em nenhuma tabela é perdido.

## Paralelização

- **Indexação**, em duas fases (esquema "acumulação local + merge no final" da especificação, sem locks):
  1. *hash*: cada thread calcula os L buckets da sua faixa contígua de vetores (como no brute-force) e grava no
     array de códigos só nas posições dos seus vetores;
  2. *buckets*: depois da barreira, cada tabela inteira é montada por uma thread (tabelas não compartilham
     nada). O paralelismo desta fase é limitado a L — com mais threads que tabelas, as excedentes ficam ociosas.
  Os ids entram nos buckets em ordem crescente, então o índice é idêntico em qualquer número de threads.
- **Busca**: o trabalho de uma query é pequeno, então a paralelização é *entre* queries: `schedule(dynamic,
  BATCH)` distribui lotes de queries entre as threads (queries com buckets maiores custam mais; o dynamic
  reequilibra). Cada thread tem sua memória de trabalho e escreve só no trecho de resultado das suas queries.

A indexação é medida e reportada **separadamente** da busca — a comparação principal com o brute-force usa só
o tempo de busca (que tem os mesmos nomes de coluna no `runs.csv` dos dois).

## Parâmetros (`make lsh LSH_TABLES=.. LSH_HASHES=.. LSH_WIDTH=..`)

| Makefile | Opção | Padrão | Efeito |
|---|---|---|---|
| `LSH_TABLES` | `--tables` | 32 | L: mais tabelas = mais recall, mais memória e indexação mais lenta |
| `LSH_HASHES` | `--hashes` | 10 | K: maior = buckets menores (menos candidatos, busca mais rápida, menos recall) |
| `LSH_WIDTH` | `--width` | 800 | w: maior = buckets maiores (mais candidatos, mais recall) |
| `LSH_BUCKETS` | `--buckets` | 0 (≈ n/16) | buckets por tabela (potência de 2) |
| `SEED` | `--seed` | 42 | semente das projeções |
| `BATCH` | `--batch` | 1 | queries por lote no schedule dynamic da busca |

Calibração no SIFT1M (sequencial, 1024 queries, i5-1135G7; recall@10 considerando empates). São as
configurações do `make experimento` (`LSH_CONFIGS`), a curva recall × velocidade:

| L | K | w | recall | candidatos/query | ms/query |
|---|---|---|---|---|---|
| 8 | 8 | 1000 | 0.873 | 186 mil (19%) | 18.0 |
| **32** | **10** | **800** | **0.915** | **84 mil (8%)** | **10.4** |
| 32 | 12 | 1000 | 0.945 | 112 mil (11%) | 12.5 |
| 32 | 14 | 1200 | 0.969 | 157 mil (16%) | 16.7 |
| 40 | 14 | 1300 | 0.989 | 222 mil (22%) | 21.0 |

`16:10:1000` foi descartada (recall 0.911, pior e mais lenta que a padrão); `64:14:1400` chega a 0.999 mas
calcula distância para 40% da base (32 ms/query, mais lento que o brute-force sequencial). Brute-force na
mesma máquina: ~38 ms/query sequencial, ~15 ms/query no melhor número de threads. No SIFT10M a densidade é 10x maior e os mesmos
parâmetros deixam ~5% da base como candidatos (recall 0.90, índice de ~2 GB): vale testar `LSH_HASHES=12` ou
mais para cortar candidatos.

## O que é medido

Por execução (`results/lsh/<data-hora>_<seq|tN>/`):

- `summary.csv` — por repetição: indexação (total, fase hash, fase buckets, CPU, energia) e busca (tempo,
  ms/query, CPU, utilização, energia, trocas de contexto), com horário de início/fim de cada fase.
- `threads.csv` — por thread e repetição: faixa de vetores e tempo da fase hash, tabelas montadas e tempo da
  fase buckets, queries e candidatos processados, tempo e CPU da busca, ocupação e núcleo.
- `queries.csv` — por query: recall, recall estrito, razão de distância (devolvido ÷ verdadeiro) e candidatos.
- `run.csv` — uma linha agregada (parâmetros, médias/desvios, forma do índice — memória, buckets não vazios,
  maior bucket —, candidatos por query, recall, speedups e eficiências da indexação e da busca). A mesma linha
  é acrescentada em `results/lsh/runs.csv`.
