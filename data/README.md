# data/

Arquivos do dataset — nada aqui é versionado no git (ver `.gitignore`), só este README.

- `sift/`, `siftsmall/` — arquivos brutos do corpus TEXMEX (SIFT1M e SIFT10K), baixados por `make download`:
  `*_base.fvecs` (base), `*_query.fvecs` (queries), `*_groundtruth.ivecs` (100 vizinhos corretos por query).
  Os `*_learn.fvecs` vêm junto no pacote mas não são usados.
- `sift1m.db`, `siftsmall.db` — o banco já convertido e validado, gerado por `make construct-db`
  (`DATASET=siftsmall` para o menor). É esse arquivo que o `make up-db` sobe para a memória.
