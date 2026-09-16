# Estrutura do projeto

Este documento explica a organização de diretórios do repositório e como ela se relaciona com o
`docs/especificacao_tecnica.pdf`. Por enquanto só a estrutura (pastas, `README.md` de cada uma e o
`Makefile`) foi criada — a implementação em si ainda não existe.

## Diretórios

- **`database/`** — carga do dataset em memória: parser de `.fvecs`/`.ivecs` e construção do
  `VectorDataset`. Não é um banco persistente/serviço; é só a camada de "criação e população" da estrutura
  em memória usada pelas buscas.
- **`common/`** — código compartilhado entre `database/` e os dois algoritmos de busca: `VectorDataset`,
  função de distância euclidiana, utilitários de top-K e escrita de métricas (tempo, speedup, eficiência
  paralela, recall) em CSV. Existe para não duplicar esse código entre `scripts/brute-force/` e
  `scripts/lsh/`.
- **`scripts/brute-force/`** — busca exata (sequencial e paralela via OpenMP), dividindo o array por faixa
  de índice de vetor e fazendo merge dos top-K locais.
- **`scripts/lsh/`** — busca aproximada via LSH: estruturas `Bucket`/`LSHIndex`, indexação paralela
  (hash + buckets) e busca paralela por lote de queries.
- **`data/`** — arquivos brutos do dataset (SIFT1M ou subconjuntos), baixados externamente. Não versionado
  no git.
- **`results/`** — saída das execuções (CSV com as métricas do protocolo experimental). Não versionado no
  git.
- **`Makefile`** (raiz) — orquestra os passos: `make database`, `make populate`, `make brute-force
  THREADS=N`, `make lsh THREADS=N`. Hoje os alvos são placeholders (`TODO`), sem lógica real ainda.

## Por que separar `database/` de `common/`

`database/` é sobre *carregar* o dataset bruto (parsing de arquivo, validação). `common/` é sobre *o que os
dois algoritmos de busca usam depois de carregado* (a struct em si, distância, top-K, métricas). Essa
separação evita que `scripts/brute-force/` e `scripts/lsh/` dependam de lógica de parsing de arquivo, e evita
duplicar a definição de `VectorDataset` ou a função de distância entre os dois algoritmos.

## Por que `THREADS` é parâmetro dos dois algoritmos, não só do brute-force

O protocolo experimental (Etapa 6 da especificação) compara brute-force e LSH na mesma variação de threads
(1, 2, 4, 8...). O LSH também paraleliza (indexação e busca em lote), então o `Makefile` expõe `THREADS`
igualmente para `make brute-force` e `make lsh`.

## Próximos passos

Implementação seguindo as Etapas 1–7 da especificação técnica, já resumidas em `CLAUDE.md`.
