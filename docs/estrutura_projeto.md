# Estrutura do projeto

Este documento explica a organização de diretórios do repositório e como ela se relaciona com o
`docs/especificacao_tecnica.pdf`. Hoje está implementada a Etapa 1 (download,
parser e o banco em memória compartilhada); as buscas ainda não.

## Diretórios

- **`database/`** — ciclo de vida do banco: download, parser de `.fvecs`/`.ivecs`, conversão para o binário
  `data/<dataset>.db` e a ferramenta que sobe esse binário para um segmento de memória compartilhada
  somente leitura (`/dev/shm/pcd_<dataset>`). Não é um serviço de rede: as buscas mapeiam o segmento
  direto na memória delas.
- **`common/`** — código compartilhado entre `database/` e os dois algoritmos de busca: `VectorDataset`,
  formato binário do banco (`db_format.h`), API `attach_db` para se plugar no banco no ar (`shm_db.h`),
  função de distância euclidiana, utilitários de top-K e escrita de métricas (tempo, speedup, eficiência
  paralela, recall) em CSV. Existe para não duplicar esse código entre `scripts/brute-force/` e
  `scripts/lsh/`.
- **`scripts/brute-force/`** — busca exata (sequencial e paralela via OpenMP), dividindo o array por faixa
  de índice de vetor e fazendo merge dos top-K locais.
- **`scripts/lsh/`** — busca aproximada via LSH: estruturas `Bucket`/`LSHIndex`, indexação paralela
  (hash + buckets) e busca paralela por lote de queries.
- **`data/`** — arquivos brutos do dataset (SIFT1M e SIFT10K, baixados por `make download`) e o banco
  convertido `<dataset>.db`. Não versionado no git.
- **`results/`** — saída das execuções (CSV com as métricas do protocolo experimental). Não versionado no
  git.
- **`Makefile`** (raiz) — orquestra os passos: `make download`, `make construct-db`, `make up-db`,
  `make down-db`, `make status-db`, `make test-db` (todos com `DATASET=sift1m|siftsmall`), e
  `make brute-force THREADS=N` / `make lsh THREADS=N` (ainda placeholders `TODO`).

## Por que o banco fica num processo separado, em memória compartilhada

Separar "subir o banco" (`make up-db`, terminal 1) das buscas (terminal 2) evita recarregar ~500 MB a cada
experimento. A comunicação é por memória compartilhada POSIX em vez de um servidor com socket: o programa de
busca recebe um ponteiro para as mesmas páginas físicas, então ler `base[i * d + j]` custa o mesmo que num
array local — sem rede nem serialização contaminando as medições. O segmento é selado como somente leitura
(`mprotect(PROT_READ)` + permissão `0444`), e os clientes mapeiam com `PROT_READ`: uma escrita indevida
derruba o processo com SIGSEGV em vez de corromper o banco. A validação dos dados acontece uma vez só, no
`make construct-db`, por isso não existe um passo `populate` separado.

## Por que separar `database/` de `common/`

`database/` é sobre *construir e servir* o banco (parsing de arquivo, validação, memória compartilhada). `common/` é sobre *o que os
dois algoritmos de busca usam depois de carregado* (a struct em si, distância, top-K, métricas). Essa
separação evita que `scripts/brute-force/` e `scripts/lsh/` dependam de lógica de parsing de arquivo, e evita
duplicar a definição de `VectorDataset` ou a função de distância entre os dois algoritmos.

## Por que `THREADS` é parâmetro dos dois algoritmos, não só do brute-force

O protocolo experimental (Etapa 6 da especificação) compara brute-force e LSH na mesma variação de threads
(1, 2, 4, 8...). O LSH também paraleliza (indexação e busca em lote), então o `Makefile` expõe `THREADS`
igualmente para `make brute-force` e `make lsh`.

## Próximos passos

Implementação seguindo as Etapas 1–7 da especificação técnica, já resumidas em `CLAUDE.md`.
