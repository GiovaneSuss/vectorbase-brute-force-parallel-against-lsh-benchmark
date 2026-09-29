# data/

Arquivos do dataset — nada aqui é versionado no git (ver `.gitignore`), só este README.

- `sift/`, `siftsmall/` — arquivos brutos do corpus TEXMEX (SIFT1M e SIFT10K), baixados por `make download`:
  `*_base.fvecs` (base), `*_query.fvecs` (queries), `*_groundtruth.ivecs` (100 vizinhos corretos por query).
  Os `*_learn.fvecs` vêm junto no pacote mas não são usados.
- `bigann/` — SIFT10M, baixado por `make download-10m`: `bigann_base_10M.bvecs` (os 10M primeiros vetores do
  SIFT1B, em uint8), `bigann_query.bvecs` e `idx_10M.ivecs` (1000 vizinhos corretos por query nos 10M primeiros).
- `sift1m.db`, `sift10m.db`, `siftsmall.db` — o banco já convertido e validado, gerado por
  `make construct-db-1m`, `-10m` e `-small`. É esse arquivo que o `make up-db` sobe para a memória.
